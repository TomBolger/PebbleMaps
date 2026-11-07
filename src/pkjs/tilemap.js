// Pebble Maps - tile mode for the home map on Pebble Time 2 / Round 2
//
// The watch keeps a block of memory full of small compressed map tiles around its view, so panning shows real
// map straight away. This file keeps the phone's picture of that memory (see tilestore.js), decides what to send
// next, and cuts the tiles from Google's pictures (tiles.js). It sends one batch at a time and plans the next one
// from where the view is by then, so nothing stale is ever queued.

var P = require('./protocol');
var geo = require('./geo');
var image = require('./image');
var msg = require('./msg');
var tiles = require('./tiles');
var ts = require('./tilestore');
var dev = require('./dev');

var CMD = P.CMD;
var TILE = ts.TILE;
var RECORD = ts.HEADER;   // bytes before each tile's data (the watch's table entry as it is)
var HOLD_MS = 2500;       // a recentring waits this long for its tiles at most

var env = null;           // { watch(), style(), economical(), seq(), fail(err), worked() }
var tm = null;            // the running session

function init(e) { env = e; }
function active() { return !!tm; }
// Is the running session this screen's, in this style and size?
function runningFor(seq, style, v) { return !!tm && tm.seq === seq && tm.style === style && tm.v.w === v.w && tm.v.h === v.h; }
function stop() {
  if (tm && tm.timer) clearTimeout(tm.timer);
  if (tm && tm.holdTimer) clearTimeout(tm.holdTimer);
  tm = null;
}

// The view as the watch counts it: whole world pixels
function viewOf(view) {
  var c = geo.project(view.center[0], view.center[1], view.zoom);
  return { cx: Math.round(c[0]), cy: Math.round(c[1]), z: view.zoom, w: view.w, h: view.h };
}

function palette(style) { return image.PALETTES[style].map(function (c) { return 0xC0 | c; }); }

function viewDict(seq, v, fresh) {
  var d = { cmd: CMD.MAP_BEGIN, seq: seq, width: v.w, height: v.h, fmt: P.FMT.TILES, img: 1,
            idx: v.cx, mode: v.cy, num: v.z, flags: fresh ? 1 : 0 };
  if (fresh) d.palette = palette(tm.style);
  return d;
}

// A new map screen: the watch has just emptied its tiles (or is about to). Call kick() once the markers are queued.
function begin(seq, v) {
  stop();
  var w = env.watch();
  tm = { seq: seq, v: v, style: env.style(), store: new ts.Store(w.tileStart, w.tileEnd, w.tileSlots), dir: null,
         dirT: 0, busy: false, shown: false, hold: false, timer: null, blocked: null, rings: 0 };
  msg.drop('map:');
  msg.send(viewDict(seq, v, true), 'map:' + seq);
}

// The map moved to v at the phone's wish (following you, "back to you"): the watch jumps once the tiles for
// the new spot are there
function adopt(v) {
  if (dev.logTiles) console.log('tiles: adopt view ' + v.cx + ',' + v.cy + ' z' + v.z);
  setView(v);
  tm.hold = true;
  clearTimeout(tm.holdTimer);
  var mine = tm;
  tm.holdTimer = setTimeout(function () { if (tm === mine) release(); }, HOLD_MS);
}

function release() {
  if (!tm || !tm.hold) return;
  if (dev.logTiles) console.log('tiles: release view ' + tm.v.cx + ',' + tm.v.cy);
  tm.hold = false;
  clearTimeout(tm.holdTimer);
  msg.send(viewDict(tm.seq, tm.v, false), 'map:' + tm.seq);
}

// The watch moved or zoomed by itself and says where it is
function move(v) {
  if (dev.logTiles) console.log('tiles: watch view ' + v.cx + ',' + v.cy + ' z' + v.z);
  tm.hold = false;
  setView(v);
  pump();
}

function setView(v) {
  var old = tm.v, now = Date.now();
  if (old.z !== v.z) {
    tm.dir = null;
  } else {
    var dx = v.cx - old.cx, dy = v.cy - old.cy, len = Math.sqrt(dx * dx + dy * dy);
    if (len > 6) { tm.dir = [dx / len, dy / len]; tm.dirT = now; }
    else if (now - tm.dirT > 8000) tm.dir = null;
  }
  tm.v = v;
}

// Is every tile of the screen in the watch?
function screenReady(v) {
  var w = ts.want(v, { rings: 0 });
  for (var i = 0; i < w.list.length; i++) if (!tm.store.has(w.list[i].x, w.list[i].y, v.z)) return false;
  return true;
}

function sourceOf(v) { return { zoom: v.z, style: tm.style, format: 1 }; }

// One tile's bytes, or null when no picture is cached for it yet (then `need` says where to get one)
function render(t, v) {
  var o = sourceOf(v);
  var r = { x0: t.x * TILE, y0: t.y * TILE, x1: t.x * TILE + TILE, y1: t.y * TILE + TILE };
  var pic = tiles.peekRect(o, r);
  if (!pic) return { need: r };
  var px = tiles.cutBlock(pic, r.x0, r.y0, TILE);
  var packed = image.pack(px, TILE, TILE, 1, tm.style);
  return { bytes: ts.encodeTile(ts.nibbles(packed.data, packed.stride, TILE), TILE) };
}

function wantNow() {
  var rings = env.economical() ? 1 : 3;
  return ts.want(tm.v, { rings: rings, max: tm.store.nslots, dir: tm.dir });
}

function kick() { if (tm) { tm.blocked = null; pump(); } }

function later(ms) {
  if (tm.timer) return;
  var mine = tm;
  tm.timer = setTimeout(function () { if (tm === mine) { tm.timer = null; pump(); } }, ms);
}

// Send the next batch of tiles (if any are due), then the next, until the watch holds what it should
function pump() {
  if (!tm || tm.busy || tm.timer) return;
  if (tm.seq !== env.seq()) return;               // another screen has the watch now
  if (msg.pending() >= 2) return later(120);      // the watch is still busy with what we sent
  var s = tm, v = s.v, w = wantNow();
  var keep = {}, recs = [], placedTiles = [], bytes = 0, room = Math.max(600, env.watch().inbox - 200);
  var blocked = s.blocked && s.blocked.v === v && s.blocked.version === s.store.version ? s.blocked.key : null;
  var i = 0;

  function mine(c) { return c.z === v.z && keep[c.x + ',' + c.y] === true; }
  function rank(c) { return ts.rank(c, v); }

  function finish() {
    s.busy = false;
    if (tm !== s) return;
    var flags = 0;
    if (!s.shown && screenReady(v)) { flags = 1; s.shown = true; }
    if (recs.length || flags) {
      var data = [];
      recs.forEach(function (r) { data.push.apply(data, r); });
      if (dev.logTiles) {
        console.log('tiles: +' + recs.length + ' tiles ' + bytes + ' B (msg ' + data.length + ') flags ' + flags + ' | holding ' +
          s.store.count() + ' tiles ' + s.store.bytes() + ' of ' + (s.store.end - s.store.start) + ' B');
      }
      msg.send({ cmd: CMD.TILES, seq: s.seq, data: data, flags: flags }, 'map:' + s.seq, function (ok) {
        if (tm !== s) return;
        if (ok) return pump();
        // not delivered: forget those tiles so they get sent again
        placedTiles.forEach(function (t) { s.store.remove(t); });
        s.shown = false;
        later(1000);
      });
    }
    if (s.hold && screenReady(s.v)) release();
    if (!recs.length && !flags && !s.prefetched && !env.economical()) {
      s.prefetched = true;   // idle: get the zoomed-in and zoomed-out pictures ready
      tiles.prefetchAround({ c: [v.cx, v.cy], zoom: v.z, w: v.w, h: v.h, heading: 0, style: s.style, format: 1 }, true);
    }
  }

  function fetch(rect) {
    var want = [v.cx, v.cy];
    if (s.dir) want = [v.cx + s.dir[0] * 100, v.cy + s.dir[1] * 100];
    var c = dev.snapTiles ? [(rect.x0 + rect.x1) / 2, (rect.y0 + rect.y1) / 2] : tiles.centreFor(rect, want);
    tiles.fetchPicture(sourceOf(v), c, function (err) {
      if (tm !== s) return;
      s.busy = false;
      if (err) return env.fail(err);
      env.worked();
      pump();
    });
  }

  function step() {
    for (; i < w.list.length; i++) {
      var t = w.list[i], k = t.x + ',' + t.y;
      if (s.store.has(t.x, t.y, v.z)) { keep[k] = true; continue; }
      if (k === blocked) break;                    // known not to fit; nothing has changed since
      var r = render(t, v);
      if (r.need) {
        if (recs.length) break;                    // send what's ready first
        return fetch(r.need);
      }
      if (bytes + r.bytes.length + RECORD > room) break;
      var placed = s.store.place(t, r.bytes.length, mine, rank);
      if (!placed) { s.blocked = { key: k, v: v, version: s.store.version }; break; }
      keep[k] = true;
      var rec = [];
      ts.encodeRecord(rec, { slot: placed.slot, z: v.z, x: t.x * TILE, y: t.y * TILE, off: placed.off, bytes: r.bytes });
      recs.push(rec);
      placedTiles.push(placed.tile);
      bytes += rec.length;
    }
    finish();
  }

  s.busy = true;
  step();
}

module.exports = { init: init, active: active, runningFor: runningFor, stop: stop, viewOf: viewOf, begin: begin, adopt: adopt, move: move,
  kick: kick, release: release };
