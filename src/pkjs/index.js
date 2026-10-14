// Pebble Maps - phone side
//
// The watch asks for things (maps, searches, routes); this file talks to
// Google Maps Platform with the user's own API key and sends results back.

var P = require('./protocol');
var geo = require('./geo');
var fmt = require('./format');
var image = require('./image');
var google = require('./google');
var msg = require('./msg');
var settings = require('./settings');
var nav = require('./nav');
var Clay = require('./vendor/clay');
var clayConfig = require('./clay-config');
var clayComponents = require('./clay-components');
var dev = require('./dev');
var tiles = require('./tiles');
tiles.setIdleCheck(function () { return msg.pending() === 0; });
var voice = require('./voice');
var jpeg = require('jpeg-js/lib/decoder');

var CMD = P.CMD;

var clay = new Clay(clayConfig.config(), clayConfig.customFn, { autoHandleEvents: false });
clayComponents.forEach(function (c) { clay.registerComponent(c); });

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
var watch = { w: 144, h: 168, fmt: 1, mic: true, round: false, touch: false, inbox: 4096 };
var me = null;            // [lat, lng]
var meAcc = 50;
var meTime = 0;
var results = [];
var dest = null;
var destWaiters = null;   // callbacks waiting for dest coordinates
var route = null;         // built route (nav model)
var routeKey = '';
var routeTime = 0;
var navigator_ = null;
var navSession = 0;       // invalidates route/GPS callbacks after stop or restart
var watchId = null;
var simTimer = null;
var gpsTimer = null;
var currentMapSeq = 0;
var mapVisible = false;   // a map screen (not navigation) is showing on the watch
var followMode = 0;       // home map (watch button): 0 heading up (turns with you), 1 follow north up, 2 plain map
var homeDrive = true;     // heading up: turns with you, you near the bottom
var STILL_NORTH = 30000;  // heading up: standing still this long turns the map back to north up
var PAN_HOLD = 7000;      // after you move the map, how long before it follows you again (while you move)
var poiMode = 1;          // places on the home map: 0 off, 1 dots, 2 dots with names (watch button)
var DRIVE_AHEAD = 0.22;   // heading up: how far below the middle your arrow sits (share of height)
// Live location while a map is showing (off = battery saver: the spot when the map opened)
var live = { on: false, watchId: null, simTimer: null, stopTimer: null, prev: null, heading: 0, moving: false, lastMove: 0, lastRender: 0 };
var pois = [];            // places drawn on the home map
var poiCache = {};        // grid cell -> places
var poiState = { busy: false, last: 0, failUntil: 0 };
var info = null;          // place on the info page
var infoCache = [];
var photoCache = [];
var lastMk = '';

var S = settings.get();

function reloadSettings() {
  S = settings.get();
  var key = S.apiKey || dev.testKey || '';
  google.setKey(key);
  google.setLanguage(S.language);
}

// ---------------------------------------------------------------------------
// Sending helpers
// ---------------------------------------------------------------------------
function send(dict, tag) { msg.send(dict, tag); }

function sendStatus() {
  send({
    cmd: CMD.STATUS,
    num: google.hasKey() ? 1 : 0,
    mode: S.defaultMode,
    flags: (S.imperial ? 1 : 0) | (S.vibrate ? 2 : 0) | ((S.textSize & 3) << 2) | (S.mapStyle === 'dark' ? 16 : 0) |
      (S.disableTouch ? 32 : 0) | ((S.nonTouchUI || S.disableTouch) ? 64 : 0)
  });
}

function sendError(err, fallbackCode) {
  err = err || {};
  send({
    cmd: CMD.ERROR,
    num: err.code || fallbackCode || P.ERR.API,
    text: fmt.clip(err.text || 'Something went wrong. Please try again.', 220),
    text2: fmt.clip(err.title || '', 40)
  });
}

function sendBusy(text) { send({ cmd: CMD.BUSY, text: text }); }
function toast(text) { send({ cmd: CMD.TOAST, text: fmt.clip(text, 60) }); }

function utf8Len(str) { return unescape(encodeURIComponent(str)).length; }

// Fit the list into one message for this watch (drops items from the end if needed)
function sendList(kind, items) {
  var packed = fmt.packList(items) || '';
  var room = watch.inbox - 80;
  while (items.length > 1 && utf8Len(packed) > room) {
    items = items.slice(0, items.length - 1);
    packed = fmt.packList(items);
  }
  send({ cmd: CMD.LIST, num: kind, list: packed });
}

// ---------------------------------------------------------------------------
// Location
// ---------------------------------------------------------------------------
var LOCATION_ERROR = {
  code: P.ERR.NO_LOCATION,
  title: 'Where are you?',
  text: 'Your phone couldn\'t find your location. Make sure Location is turned on and the Pebble app is allowed to use it.'
};

// Where you are. Phones sometimes never answer a location request, so if there's no answer
// after a few seconds we ask again (a rougher or slightly older fix is fine), then once more
// with longer to try, before giving up. Everyone asking meanwhile shares the answer.
var locWait = null;

var FALLBACK_AGE = 600000;   // a position this old (10 min) still beats no map at all

// Take a position from the phone. Its own timestamp is kept when it looks right; phones that
// send none (or one in seconds, or from a wrong clock) get "now" instead of being rejected.
function acceptFix(pos) {
  var c = pos && pos.coords;
  if (!c || !isFinite(c.latitude) || !isFinite(c.longitude) ||
      Math.abs(c.latitude) > 90 || Math.abs(c.longitude) > 180 || (c.latitude === 0 && c.longitude === 0)) return false;
  var now = Date.now();
  var ts = pos.timestamp instanceof Date ? pos.timestamp.getTime() : Number(pos.timestamp);
  var t = (isFinite(ts) && ts <= now + 5000 && ts >= now - 3600000) ? Math.min(ts, now) : now;
  if (me && t < meTime) return false;   // older than what we already have
  me = [c.latitude, c.longitude];
  meAcc = (typeof c.accuracy === 'number' && isFinite(c.accuracy) && c.accuracy > 0) ? c.accuracy : 50;
  meTime = t;
  return true;
}

function recentLocation(age) {
  return !!me && Date.now() - meTime <= age;
}

function getLocation(maxAge, cb) {
  if (dev.simLocation) {
    me = me || dev.simLocation.slice();
    meTime = Date.now();
    return cb(null, me);
  }
  if (recentLocation(maxAge)) return cb(null, me);
  if (locWait) { locWait.cbs.push(cb); return; }
  var w = locWait = { cbs: [cb], timers: [] };
  function finish(err, loc) {
    if (locWait !== w) return;
    locWait = null;
    w.timers.forEach(clearTimeout);
    w.cbs.forEach(function (f) { f(err, loc); });
  }
  function ask(high, age, timeout) {
    navigator.geolocation.getCurrentPosition(function (pos) {
      if (locWait !== w) return;
      acceptFix(pos);
      if (recentLocation(age + 5000)) finish(null, me);
    }, function (e) {
      if (locWait !== w) return;
      if (e && e.code === 1) return finish(LOCATION_ERROR);   // location turned off for the Pebble app
      if (recentLocation(FALLBACK_AGE)) finish(null, me);     // the last spot, if it's fairly recent
      // otherwise keep waiting for the other tries
    }, { enableHighAccuracy: high, maximumAge: age, timeout: timeout });
  }
  ask(true, Math.min(maxAge, 60000), 15000);
  w.timers.push(setTimeout(function () { ask(false, 300000, 10000); }, 4000));
  w.timers.push(setTimeout(function () { ask(true, 300000, 30000); }, 15000));
  w.timers.push(setTimeout(function () {
    finish(recentLocation(FALLBACK_AGE) ? null : LOCATION_ERROR, recentLocation(FALLBACK_AGE) ? me : null);
  }, 50000));
}

// A fix from live tracking answers anyone still waiting
function locationArrived() {
  if (locWait && recentLocation(20000)) {
    var w = locWait;
    locWait = null;
    w.timers.forEach(clearTimeout);
    w.cbs.forEach(function (f) { f(null, me); });
  }
}

// ---------------------------------------------------------------------------
// Maps: fetch from Google, convert, stream to the watch
// ---------------------------------------------------------------------------
function mapStyle() {
  if (watch.fmt === P.FMT.BIT1) return 'bw';
  return S.mapStyle === 'dark' ? 'dark' : 'light';
}

// The watch keeps one picture buffer; on roomier watches it is bigger than the
// screen so panning shows real map right away.
var buf = { w: 144, h: 168 };

function outSize(fw, fh) {
  var m = Math.max(0, Math.min(Math.floor((buf.w - fw) / 2), Math.floor((buf.h - fh) / 2)));
  return [fw + 2 * m, fh + 2 * m];
}

function encodeMarkers(list) {
  var data = [];
  list.slice(0, 24).forEach(function (m) {
    var x = m.x & 0xffff, y = m.y & 0xffff;
    data.push(x & 0xff, (x >> 8) & 0xff, y & 0xff, (y >> 8) & 0xff, m.kind, m.index || 0);
  });
  return data;
}

// Where point p is on a view's picture (the picture may be turned)
function screenPos(p, view) {
  var a = geo.project(p[0], p[1], view.zoom), c = geo.project(view.center[0], view.center[1], view.zoom);
  var d = [a[0] - c[0], a[1] - c[1]];
  if (view.heading) d = toScreenOffset(d[0], d[1], view.heading);
  return [Math.round(d[0] + view.w / 2), Math.round(d[1] + view.h / 2)];
}

function markerFor(p, view, kind, index) {
  var s = screenPos(p, view);
  var os = outSize(view.w, view.h);
  var mx = (os[0] - view.w) / 2 + 30, my = (os[1] - view.h) / 2 + 30;
  if (s[0] < -mx || s[1] < -my || s[0] > view.w + mx || s[1] > view.h + my) return null;
  return { x: s[0], y: s[1], kind: kind, index: index || 0 };
}

// You: a blue dot, or an arrow while you're moving (live location)
function meMarker(v) {
  if (!me) return null;
  if (live.on && live.moving) {
    var rel = (((live.heading - (v.heading || 0)) % 360) + 360) % 360;
    return markerFor(me, v, P.MK.ARROW, Math.round(rel / 2) % 180);
  }
  return markerFor(me, v, P.MK.ME);
}

// What the home map shows: the watch's choice
function placesShown() {
  if (S.economical) return 0;
  return watch.lowmem ? Math.min(poiMode, 1) : poiMode;   // Pebble Time / Time Round: dots only
}

function poiMarkers(v) {
  var mode = placesShown();
  if (!mode || v.zoom < 15) return [];
  var out = [];
  for (var i = 0; i < pois.length && out.length < 22; i++) {
    var m = markerFor([pois[i].lat, pois[i].lng], v, P.MK.POI + pois[i].cat, i);
    if (m && m.x >= -8 && m.y >= -8 && m.x <= v.w + 8 && m.y <= v.h + 8) {
      if (mode === 2) m.name = fmt.clip(plainText(pois[i].name), watch.lowmem ? 12 : 18).replace(/\x1e/g, ' ');
      out.push(m);
    }
  }
  return out;
}

// The markers message: positions, plus place names (in marker order) when shown
function markersDict(seq, list) {
  list = list.filter(Boolean).slice(0, 24);
  var mk = encodeMarkers(list);
  var names = list.filter(function (m) { return m.kind >= P.MK.POI; }).map(function (m) { return m.name || ''; });
  var d = { cmd: CMD.MARKERS, seq: seq };
  if (mk.length) d.data = mk;
  if (names.some(Boolean)) d.list = names.join('\x1e');
  return d;
}

function homeMarkers(v) {
  return [meMarker(v)].concat(poiMarkers(v));
}

// Send just the markers for what the watch shows (your dot moving, places arriving)
function sendMarkers(st, force) {
  var d = markersDict(st.seq, st.markers(st.view));
  var key = (d.data || []).join(',') + (d.list || '');
  if (!force && key === lastMk) return;
  lastMk = key;
  msg.drop('mk');
  send(d, 'mk');
}

// --- What the watch's picture buffer holds ------------------------------------
// The watch keeps its current picture on screen and moves it (slide / zoom /
// turn) to line up with each new one; we then only send what is new.
var wimg = null;          // { id, c:[x,y] world px at center, zoom, heading, w, h, key, ok, valid }
var imgCounter = 0;
var panAck = 0;           // newest map move from the watch that we've applied
var renderToken = 0;
var refreshFn = null;
var refreshTimer = null;

// If a map picture couldn't be fetched or delivered (network blip, watch out of
// range), try again by itself: soon at first, then less often, until it works.
var retryTimer = null;
var retryCount = 0;

function retryLater(err) {
  if (err && err.code === P.ERR.NO_KEY) return;
  if (retryTimer || !refreshFn) return;
  var network = !err || err.code === P.ERR.NETWORK;
  if (!network && retryCount >= 4) return;   // a real Google error: don't keep asking
  var delay = Math.min(30000, 1500 * Math.pow(2, Math.min(retryCount, 5)));
  retryCount++;
  console.log('map retry in ' + delay + ' ms');
  retryTimer = setTimeout(function () {
    retryTimer = null;
    if (refreshFn) refreshFn();
  }, delay);
}

function mapWorked() {
  retryCount = 0;
  try { if (S.apiKey && localStorage.getItem('pm-key-works') !== S.apiKey) localStorage.setItem('pm-key-works', S.apiKey); } catch (e) { /* ignore */ }
  if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
}

function scheduleRefresh() {
  if (wimg) wimg.valid = false;
  retryLater({ code: P.ERR.NETWORK });
}

// The watch zooms its picture the moment the button is pressed; keep our copy in step
function mirrorAdjust(p) {
  if (p.num !== undefined && p.num !== null) panAck = p.num;
  if (wimg && (p.idx === P.ADJ.ZOOM_IN || p.idx === P.ADJ.ZOOM_OUT)) {
    var f = p.idx === P.ADJ.ZOOM_IN ? 2 : 0.5;
    wimg.c = [wimg.c[0] * f, wimg.c[1] * f];
    wimg.zoom += p.idx === P.ADJ.ZOOM_IN ? 1 : -1;
    wimg.valid = false;
  }
}

// Choose the exact picture center so it is a whole-pixel slide of what the watch has
function planImage(c, zoom, heading, w, h) {
  var p = wimg;
  if (p && p.ok && p.zoom === zoom && p.heading === heading && p.w === w && p.h === h) {
    var v = toScreenOffset(p.c[0] - c[0], p.c[1] - c[1], heading);
    v = [Math.round(v[0]), Math.round(v[1])];
    var dw = toWorld(v[0], v[1], heading);
    return { c: [p.c[0] - dw[0], p.c[1] - dw[1]], ref: p.id, v: v };
  }
  return { c: c, ref: 0, v: [0, 0] };
}

// Parts of a w x h picture that are new after sliding by v (null = send it all)
function stripRects(v, w, h) {
  var vx = v[0], vy = v[1];
  var ax = Math.abs(vx), ay = Math.abs(vy);
  if (ax >= w || ay >= h) return null;
  if ((ay * w + ax * (h - ay)) > 0.6 * w * h) return null;
  var rects = [];
  if (vy > 0) rects.push({ y0: 0, y1: vy, x0: 0, x1: w });
  else if (vy < 0) rects.push({ y0: h + vy, y1: h, x0: 0, x1: w });
  var ry0 = vy > 0 ? vy : 0, ry1 = vy < 0 ? h + vy : h;
  if (vx > 0) rects.push({ y0: ry0, y1: ry1, x0: 0, x1: vx });
  else if (vx < 0) rects.push({ y0: ry0, y1: ry1, x0: w + vx, x1: w });
  return rects;
}

// Whole picture, as bands of rows from the middle outward
function centerOutRects(w, h, rowsPer) {
  var rects = [];
  var mid = Math.floor(h / 2);
  var top = Math.max(0, mid - Math.floor(rowsPer / 2)), bot = Math.min(h, top + rowsPer);
  rects.push({ y0: top, y1: bot, x0: 0, x1: w });
  while (top > 0 || bot < h) {
    if (top > 0) { var t0 = Math.max(0, top - rowsPer); rects.push({ y0: t0, y1: top, x0: 0, x1: w }); top = t0; }
    if (bot < h) { var b1 = Math.min(h, bot + rowsPer); rects.push({ y0: bot, y1: b1, x0: 0, x1: w }); bot = b1; }
  }
  return rects;
}

// PackBits: n (0..127) = n+1 bytes follow as they are; n (129..255) = repeat the next byte 257-n times.
// Map pictures are mostly flat color, so this usually cuts the bytes to send by 3x or more.
function packRow(src, off, n, out) {
  var i = 0;
  while (i < n) {
    var v = src[off + i], j = i + 1;
    while (j < n && j - i < 128 && src[off + j] === v) j++;
    if (j - i >= 2) { out.push(257 - (j - i), v); i = j; continue; }
    var k = i + 1;
    while (k < n && k - i < 128 && !(k + 2 < n && src[off + k] === src[off + k + 1] && src[off + k] === src[off + k + 2])) k++;
    out.push(k - i - 1);
    for (var q = i; q < k; q++) out.push(src[off + q]);
    i = k;
  }
}

// Cut rects into AppMessage-sized pieces of packed rows: { y, xb, stride, data }
function chunksFor(img, rects) {
  var out = [];
  var room = Math.max(64, watch.inbox - 90);
  rects.forEach(function (r, ri) {
    var xb0, xb1;
    if (img.format === 1) { xb0 = r.x0 >> 1; xb1 = (r.x1 + 1) >> 1; }
    else { xb0 = r.x0 >> 3; xb1 = (r.x1 + 7) >> 3; }
    xb1 = Math.min(xb1, img.stride);
    var bw = xb1 - xb0;
    if (bw <= 0 || r.y1 <= r.y0) return;
    var cur = null;
    for (var y = r.y0; y < r.y1; y++) {
      var row = [];
      packRow(img.data, y * img.stride + xb0, bw, row);
      if (!cur || cur.data.length + row.length > room) {
        cur = { y: y, xb: xb0, stride: bw, data: [], rect: ri };
        out.push(cur);
      }
      for (var i = 0; i < row.length; i++) cur.data.push(row[i]);
    }
  });
  return out;
}

// Screen first (from the middle outward), then the spare margin around it
function screenFirstRects(w, h, sw, sh, rowsPer) {
  if (!sw || !sh || (sw >= w && sh >= h)) return centerOutRects(w, h, rowsPer);
  sw = Math.min(sw, w); sh = Math.min(sh, h);
  var x0 = (w - sw) >> 1, y0 = (h - sh) >> 1, x1 = x0 + sw, y1 = y0 + sh;
  var r = [{ x0: x0, x1: x1, y0: y0, y1: y1 }];   // the screen in as few pieces as possible
  if (y0 > 0) r.push({ x0: 0, x1: w, y0: 0, y1: y0 });
  if (y1 < h) r.push({ x0: 0, x1: w, y0: y1, y1: h });
  if (x0 > 0) r.push({ x0: 0, x1: x0, y0: y0, y1: y1 });
  if (x1 < w) r.push({ x0: x1, x1: w, y0: y0, y1: y1 });
  return r;
}

// Send a picture. plan: from planImage; info: { zoom, heading, key }
// onBegin runs right after the start message is queued; onDone when every piece is delivered (or failed).
function pushImage(seq, img, plan, info, markers, opts) {
  opts = opts || {};
  var prev = wimg;
  var ref = prev && plan.ref && prev.id === plan.ref ? plan.ref : 0;
  var rects = null, whole = false;
  if (ref && prev.valid && prev.key === info.key) rects = stripRects(plan.v, img.width, img.height);
  if (!rects) {
    whole = true;
    msg.drop('map:');   // a whole new picture replaces anything still waiting
    rects = screenFirstRects(img.width, img.height, opts.sw, opts.sh, 16);
  }
  imgCounter = imgCounter % 30000 + 1;
  var cur = { id: imgCounter, c: plan.c, zoom: info.zoom, heading: info.heading, w: img.width, h: img.height,
              key: info.key, ok: true, valid: true };
  wimg = cur;
  var dict = {
    cmd: CMD.MAP_BEGIN, seq: seq, width: img.width, height: img.height, fmt: img.format,
    stride: img.stride, palette: img.palette, img: cur.id, ref: ref, vx: plan.v[0], vy: plan.v[1],
    idx: Math.round(plan.c[0]), mode: Math.round(plan.c[1]), num: info.zoom,
    num2: ((Math.round(info.heading || 0) % 360) + 360) % 360
  };
  if (opts.ack) dict.ack = panAck;
  var chunks = chunksFor(img, rects);
  var pending = chunks.length + 1;
  var finished = false;
  function oneDone(ok) {
    if (!ok) {
      cur.valid = false;
      scheduleRefresh();
    }
    if (--pending === 0 && !finished) {
      finished = true;
      if (opts.onDone) opts.onDone();
    }
  }
  msg.send(dict, 'map:' + seq, function (ok) {
    if (!ok) cur.ok = false;
    oneDone(ok);
  });
  if (markers) {
    var md = markersDict(seq, markers);
    lastMk = (md.data || []).join(',') + (md.list || '');
    send(md, 'map:' + seq);
  }
  if (opts.onBegin) opts.onBegin();
  // The watch shows the new picture all at once: when the screen part is complete
  // (flags 1 on its last piece), and again with the last piece of all
  var lastScreen = -1;
  chunks.forEach(function (ch, i) { if (!whole || ch.rect === 0) lastScreen = i; });
  chunks.forEach(function (ch, i) {
    msg.send({ cmd: CMD.MAP_CHUNK, seq: seq, img: cur.id, offset: ch.y, num: ch.xb, stride: ch.stride, data: ch.data,
               flags: (i === lastScreen || i === chunks.length - 1) ? 1 : 0 },
      'map:' + seq, oneDone);
  });
}

// view: {center, zoom, w, h (screen area), path?, gmarkers?}
// markers: array of {x,y,kind,index} or null. done(err) once the picture starts streaming.
function streamMap(seq, view, markers, done, quiet) {
  currentMapSeq = seq;
  var token = ++renderToken;
  var os = outSize(view.w, view.h);
  var hd = view.heading || 0;
  var c0 = geo.project(view.center[0], view.center[1], view.zoom);
  var plan = planImage(c0, view.zoom, hd, os[0], os[1]);
  live.lastRender = Date.now();
  var o = { c: plan.c, center: view.center, zoom: view.zoom, w: os[0], h: os[1], heading: hd,
            style: mapStyle(), format: watch.fmt, path: view.path, markers: view.gmarkers };
  // walking or driving with the home map: fetch pictures centered further along the way
  if (views.home && views.home.seq === seq && live.on && live.moving && live.heading !== undefined) {
    var ha = geo.rad(live.heading);
    o.ahead = [Math.sin(ha), -Math.cos(ha)];
  }
  refreshFn = function () { if (currentMapSeq === seq && !navState) streamMap(seq, view, markers, null, true); };
  tiles.get(o, function (err, img) {
    if (token !== renderToken || seq !== currentMapSeq) { if (done) done({ stale: true }); return; }
    if (err) {
      console.log('map error ' + JSON.stringify(err));
      // a network blip: a small note, then keep retrying quietly; real problems get the error screen once
      if (!quiet && retryCount === 0) {
        if (err.code === P.ERR.NETWORK) toast('No connection. Retrying...');
        else if (err.code === P.ERR.NO_KEY || err.title) sendError(err);
      }
      if (done) done(err);
      retryLater(err);
      return;
    }
    mapWorked();
    pushImage(seq, img, plan, { zoom: view.zoom, heading: hd, key: tiles.styleKey(o) }, markers, {
      ack: true, sw: view.w, sh: view.h,
      onBegin: function () { if (done) done(null); },
      // get the next pictures ready once this one is on the watch
      onDone: function () { if (!S.economical && token === renderToken) tiles.prefetchAround(o, true); }
    });
  });
}

// Each screen's map remembers where the user moved it
var views = {};      // kind -> { view, base, markers: fn(view) }
var lastKind = null;

function cloneView(v) {
  return { center: v.center.slice(), zoom: v.zoom, w: v.w, h: v.h, path: v.path, gmarkers: v.gmarkers, heading: v.heading || 0 };
}

// Home map following you: centered on you (north up), or heading up: turned
// to your direction of travel with you near the bottom
function followView(v) {
  var nv = cloneView(v);
  if (!me) {
    if (!homeDrive) nv.heading = 0;
    return nv;
  }
  if (homeDrive) {
    var q = function (h) { return (Math.round(h / 5) * 5) % 360; };
    // moving: your direction; a short stop keeps it; still for 30 s: north up
    var still = !live.on || (!live.moving && Date.now() - live.lastMove > STILL_NORTH);
    var hd = live.on && live.moving ? q(live.heading) : (still ? 0 : (v.heading || 0));
    nv.heading = hd;
    var pos = geo.project(me[0], me[1], nv.zoom), a = geo.rad(hd), ahead = DRIVE_AHEAD * nv.h;
    nv.center = geo.unproject(pos[0] + Math.sin(a) * ahead, pos[1] - Math.cos(a) * ahead, nv.zoom);
  } else {
    nv.heading = 0;
    nv.center = me.slice();
  }
  return nv;
}

function showView(seq, kind, base, markersFn, restore) {
  var st = views[kind];
  if (restore && st && st.view.w === base.w && st.view.h === base.h) {
    st.base = base;
    st.markers = markersFn;
  } else {
    st = views[kind] = { view: cloneView(base), base: base, markers: markersFn };
  }
  st.seq = seq;
  lastKind = kind;
  streamMap(seq, st.view, markersFn(st.view));
}

function onAdjust(p) {
  mirrorAdjust(p);
  if (navigator_ && navState) return navAdjust(p);
  var st = views[lastKind];
  if (!st) return;
  var v = cloneView(st.view);
  var dx = p.width || 0, dy = p.height || 0;
  var home = lastKind === 'home';
  st.seq = p.seq;
  setMapVisible(true);
  switch (p.idx) {
    case P.ADJ.ZOOM_IN: v.zoom = Math.min(20, v.zoom + 1); break;
    case P.ADJ.ZOOM_OUT: v.zoom = Math.max(2, v.zoom - 1); break;
    case P.ADJ.PAN: {
      var q = geo.project(v.center[0], v.center[1], v.zoom);
      var wd = toWorld(dx, dy, v.heading || 0);
      v.center = geo.unproject(q[0] + wd[0], q[1] + wd[1], v.zoom);
      if (home) { st.follow = false; st.lastPan = Date.now(); }
      break;
    }
    default:
      v = cloneView(st.base);
      if (home) { st.follow = followMode !== 2; v = followView(v); }
      break;
  }
  if (home && st.follow) v = followView(v);
  st.view = v;
  streamMap(p.seq, v, st.markers(v));
  if (home) maybeFetchPois(v);
}

function clampSize(w, h) {
  return [Math.max(16, Math.min(640, w || watch.w)), Math.max(16, Math.min(640, h || watch.h))];
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------
function needKey() {
  if (google.hasKey()) return false;
  sendError({ code: P.ERR.NO_KEY, title: 'Setup needed', text: 'Add your Google Maps key in the Pebble app: Pebble Maps → Settings.' });
  return true;
}

function onHomeMap(p) {
  if (needKey()) return;
  var size = clampSize(p.width, p.height);
  var zoom = p.idx > 0 ? p.idx : 15;
  var restore = ((p.mode || 0) & 1) === 1;
  followMode = ((p.mode || 0) >> 1) & 3;
  homeDrive = followMode === 0;
  setMapVisible(true);
  getLocation(20000, function (err, loc) {
    if (err) return sendError(err);
    var base = { center: loc.slice(), zoom: zoom, w: size[0], h: size[1], heading: 0 };
    var st = views.home;
    if (!(restore && st && st.view.w === base.w && st.view.h === base.h)) {
      st = views.home = { view: cloneView(base), base: base, follow: true };
    }
    st.base = base;
    st.markers = homeMarkers;
    st.seq = p.seq;
    lastKind = 'home';
    if (st.follow) st.view = followView(st.view);
    else if (!homeDrive) st.view.heading = 0;
    streamMap(p.seq, st.view, st.markers(st.view));
    maybeFetchPois(st.view);
  });
}

// --- Live location ------------------------------------------------------------
function setMapVisible(on) {
  mapVisible = on;
  updateTracking();
}

function updateTracking() {
  var want = S.liveLocation && mapVisible && !navState && google.hasKey();
  if (want) {
    clearTimeout(live.stopTimer);
    live.stopTimer = null;
    if (!live.on) startLive();
  } else if (live.on && !live.stopTimer) {
    // a short grace period: moving between map screens shouldn't restart the GPS
    live.stopTimer = setTimeout(function () {
      live.stopTimer = null;
      if (!(S.liveLocation && mapVisible && !navState)) stopLive();
    }, navState ? 0 : 4000);
  }
}

function startLive() {
  live.on = true;
  live.prev = null;
  live.moving = false;
  if (dev.simLocation) {
    if (dev.simWalk) startSimWalk();
    return;
  }
  live.watchId = navigator.geolocation.watchPosition(function (pos) {
    if (!acceptFix(pos) || Date.now() - meTime > 20000) return;
    var c = pos.coords;
    onLiveFix(me, meAcc, c.heading, c.speed, meTime);
  }, function (e) {
    console.log('live location: ' + (e && e.message));
  }, { enableHighAccuracy: true, maximumAge: 2000, timeout: 30000 });
}

function stopLive() {
  if (live.watchId !== null) navigator.geolocation.clearWatch(live.watchId);
  live.watchId = null;
  if (live.simTimer) clearInterval(live.simTimer);
  live.simTimer = null;
  live.on = false;
  live.moving = false;
}

// Test mode only: walk/drive in a straight line, then stop
function startSimWalk() {
  var w = dev.simWalk, t0 = Date.now(), step = (dev.simInterval || 1000) / 1000;
  var pos = (me || dev.simLocation).slice();
  live.simTimer = setInterval(function () {
    var moving = !w.stopAfter || (Date.now() - t0) / 1000 < w.stopAfter;
    if (moving) {
      var d = w.speed * step, a = geo.rad(w.bearing);
      pos = [pos[0] + d * Math.cos(a) / 111320, pos[1] + d * Math.sin(a) / (111320 * Math.cos(geo.rad(pos[0])))];
    }
    onLiveFix(pos.slice(), 5, moving ? w.bearing : NaN, moving ? w.speed : 0, Date.now());
  }, dev.simInterval || 1000);
}

function onLiveFix(p, acc, cHeading, cSpeed, t) {
  if (navState) return;
  me = p;
  meAcc = acc;
  meTime = t;
  locationArrived();
  var prev = live.prev;
  var moved = prev ? geo.haversine(prev.p, p) : 0;
  var dt = prev ? (t - prev.t) / 1000 : 0;
  var far = moved >= Math.max(8, Math.min(acc, 40) * 0.8);   // more than GPS wobble
  var speed = (typeof cSpeed === 'number' && !isNaN(cSpeed) && cSpeed >= 0) ? cSpeed : null;
  if (speed === null && far && dt > 0.5) speed = moved / dt;
  if (typeof cHeading === 'number' && !isNaN(cHeading) && cHeading >= 0 && (speed || 0) > 0.6) live.heading = cHeading;
  else if (far) live.heading = geo.bearing(prev.p, p);
  if (!prev || far || dt > 20) live.prev = { p: p, t: t };
  if ((speed || 0) >= 0.8 && acc <= 60) {
    live.moving = true;
    live.lastMove = t;
  } else if (live.moving && t - live.lastMove > 6000) {
    live.moving = false;   // stopped: the arrow goes back to a dot
    // after 30 s of standing still, heading up turns back to north (fixes may stop coming)
    clearTimeout(live.northTimer);
    live.northTimer = setTimeout(liveUpdate, STILL_NORTH + 500);
  }
  liveUpdate();
}

function liveUpdate() {
  if (!mapVisible || navState) return;
  var st = views[lastKind];
  if (!st || !st.markers) return;
  // you moved the map: it stays there while you stand still, and follows you again
  // once you're on the move (a few seconds after your last touch)
  if (lastKind === 'home' && !st.follow && followMode !== 2 && live.moving && Date.now() - (st.lastPan || 0) > PAN_HOLD) {
    st.follow = true;
  }
  if (lastKind === 'home' && st.follow && me) {
    var v = st.view, t = followView(v), need = false;
    if (angleDiff(v.heading || 0, t.heading || 0) > 12) need = true;
    else {
      var sp = screenPos(me, v);
      var ax = v.w / 2, ay = v.h / 2 + (homeDrive ? DRIVE_AHEAD * v.h : 0);
      var dd = Math.sqrt((sp[0] - ax) * (sp[0] - ax) + (sp[1] - ay) * (sp[1] - ay));
      need = dd > (homeDrive ? 14 : Math.min(v.w, v.h) * 0.22);
    }
    if (need && Date.now() - live.lastRender > 1200 && msg.pending() < 6) {
      st.view = t;
      streamMap(st.seq, t, st.markers(t), null, true);
      maybeFetchPois(t);
      return;
    }
  }
  sendMarkers(st);
}

// --- Places on the map ---------------------------------------------------------
// Kinds of places on the map: the Places menu's categories, then Bars and Other (same order as the watch)
var MAP_CATS = P.CATEGORIES.concat([
  { name: 'Bars', types: ['bar', 'pub', 'wine_bar'], rank: 'POPULARITY' },
  { name: 'Other places', types: ['tourist_attraction', 'museum', 'art_gallery', 'shopping_mall', 'clothing_store',
    'book_store', 'movie_theater', 'performing_arts_theater', 'bakery', 'ice_cream_shop'], rank: 'POPULARITY' }
]);
var CAT_OTHER = MAP_CATS.length - 1;

// Which kind a Google place type belongs to
function poiCategory(t) {
  t = t || '';
  if (/cafe|coffee|tea_house/.test(t)) return 1;
  if (/^bar$|_bar$|pub|night_club|brewery|winery/.test(t)) return 12;
  if (/restaurant|food|meal|diner|pizza|sandwich|steak|brunch|deli/.test(t)) return 0;
  for (var i = 0; i < MAP_CATS.length - 1; i++) {
    if (MAP_CATS[i].types.indexOf(t) >= 0) return i;
  }
  if (/hotel|lodging|motel|hostel|inn$|resort/.test(t)) return 5;
  if (/park|garden|playground/.test(t)) return 4;
  return CAT_OTHER;
}

var poiOpen = true;       // places on the map: open now only (watch switch)
var poiMask = (1 << 0) | (1 << 1) | (1 << 12) | (1 << CAT_OTHER);   // the watch sends its choice

function enabledCats() {
  var out = [];
  for (var i = 0; i < MAP_CATS.length; i++) if ((poiMask >> i) & 1) out.push(i);
  return out;
}

// Map dots with "Open now": two cheap lookups (all places, and open-now places). A place
// that only shows up in the first is closed now, or has no opening hours on Google. Kinds
// that normally post hours (restaurants, shops, museums...) are taken as closed and hidden;
// the rest (parks, ATMs, parking, stations, landmarks...) stay.
var POSTS_HOURS = /restaurant|food|meal|cafe|coffee|bar$|^bar|pub|bakery|ice_cream|dessert|tea|store|shop|market|mall|museum|gallery|gym|pharmacy|drugstore|bank$|salon|spa|theater|cinema|movie|library|gas_station|car_wash|winery|brewery|night_club|bowling|zoo|aquarium|post_office/;

function poisNotClosed(center, radius, cat, cb) {
  google.searchPois(center, radius, cat, function (e1, all) {
    if (e1) return cb(e1);
    google.searchOpen(center, radius, cat, function (e2, open) {
      if (e2) return cb(null, all);   // couldn't check: show them all rather than none
      var isOpen = {};
      open.forEach(function (pl) { isOpen[pl.placeId] = true; });
      var out = open.slice();
      all.forEach(function (pl) {
        if (isOpen[pl.placeId]) return;
        if (!POSTS_HOURS.test(pl.primaryType || '')) out.push(pl);   // probably no hours: keep
      });
      cb(null, out);
    });
  });
}

var POI_CELL = 512;   // map pixels per lookup area (about one Google map picture)

function poiCell(v, dx, dy) {
  var z = Math.min(18, v.zoom);
  var c = geo.project(v.center[0], v.center[1], z);
  return [z, Math.floor(c[0] / POI_CELL) + (dx || 0), Math.floor(c[1] / POI_CELL) + (dy || 0)];
}

// Gather the places near this view from what we've already looked up, taking turns
// between the kinds so each one shows up
function rebuildPois(v) {
  var lists = enabledCats().map(function (cat) {
    var l = [];
    for (var dy = -1; dy <= 1; dy++) {
      for (var dx = -1; dx <= 1; dx++) {
        (poiCache[poiCell(v, dx, dy).join(':') + ':' + cat + (poiOpen ? 'o' : '')] || []).forEach(function (pl) { l.push(pl); });
      }
    }
    return l;
  });
  var seen = {}, out = [];
  for (var k = 0; out.length < 80; k++) {
    var any = false;
    lists.forEach(function (l) {
      var pl = l[k];
      if (!pl) return;
      any = true;
      if (seen[pl.placeId] || out.length >= 80) return;
      seen[pl.placeId] = true;
      out.push(pl);
    });
    if (!any) break;
  }
  pois = out;
}

function maybeFetchPois(v) {
  if (!placesShown() || v.zoom < 15 || !google.hasKey()) return;
  rebuildPois(v);
  var cell = poiCell(v);
  var base = cell.join(':');
  var oflag = poiOpen ? 'o' : '';
  var cat = enabledCats().filter(function (c) { return !poiCache[base + ':' + c + oflag]; })[0];
  if (cat === undefined || poiState.busy || Date.now() < poiState.failUntil) return;
  poiState.busy = true;
  var z = cell[0];
  var center = geo.unproject((cell[1] + 0.5) * POI_CELL, (cell[2] + 0.5) * POI_CELL, z);
  var mpp = 156543.03 * Math.cos(geo.rad(center[0])) / Math.pow(2, z);
  var radius = Math.max(150, Math.min(3000, mpp * POI_CELL * 0.72));
  (oflag ? poisNotClosed : google.searchPois)(center, radius, MAP_CATS[cat], function (err, list) {
    poiState.busy = false;
    if (err) {
      console.log('places on map: ' + (err.text || err.title));
      poiState.failUntil = Date.now() + 60000;
      return;
    }
    list.forEach(function (pl) { pl.cat = cat; });
    poiCache[base + ':' + cat + oflag] = list;
    var keys = Object.keys(poiCache);
    if (keys.length > 150) delete poiCache[keys[0]];
    var st = views.home;
    if (st && lastKind === 'home' && mapVisible) {
      rebuildPois(st.view);
      sendMarkers(st);
      maybeFetchPois(st.view);   // the next kind, or the area you're looking at now
    }
  });
}

function resultItems() {
  return results.map(function (r) {
    var bits = [];
    if (r.type) bits.push(r.type);
    if (me) bits.push(fmt.distance(geo.haversine(me, [r.lat, r.lng]), S.imperial));
    if (r.address) bits.push(r.address);
    return { title: fmt.clip(r.name, 46), sub: fmt.clip(bits.join(' · '), 62) };
  });
}

function onSearch(p) {
  if (needKey()) return;
  var query = String(p.text || '').trim();
  if (!query) return sendError({ code: P.ERR.NO_RESULTS, text: 'Say a place, like "coffee" or "public library".' });
  sendBusy('Searching for "' + fmt.clip(query, 30) + '"');
  getLocation(60000, function (err, loc) {
    var open = p.mode === 1;   // the "Open now" switch
    google.searchText(query, err ? null : loc, function (e2, list) {
      if (e2) return sendError(e2);
      results = list;
      if (!list.length) {
        return sendError({ code: P.ERR.NO_RESULTS, text: 'No results' + (open ? ' open right now' : '') + ' for "' + fmt.clip(query, 40) + '"' +
          (open ? '. Turn off "Open now" in Places to see all.' : '') });
      }
      sendList(P.LIST.RESULTS, resultItems());
    }, open);
  });
}

// --- Suggestions while typing on the watch keyboard ------------------------
var suggestions = [];
var acToken = null, acTokenTime = 0;
var acRequest = 0, suggestionSets = [], acGeneration = 0;

function newToken() {
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function (ch) {
    var r = Math.random() * 16 | 0;
    return (ch === 'x' ? r : (r & 3 | 8)).toString(16);
  });
}

function onAutocomplete(p) {
  if (!google.hasKey()) return;
  var text = String(p.text || '').trim();
  var reqId = p.idx || 0;
  if (!text) return;
  acRequest = reqId;
  var generation = ++acGeneration;
  if (!acToken || p.mode === 1 || Date.now() - acTokenTime > 170000) {
    acToken = newToken();
    acTokenTime = Date.now();
  }
  var token = acToken;
  getLocation(120000, function (err, loc) {
    if (generation !== acGeneration) return;
    google.autocomplete(text, err ? null : loc, token, function (e2, list) {
      if (generation !== acGeneration || reqId !== acRequest) return;
      if (e2) { console.log('autocomplete: ' + (e2.text || e2.title)); list = []; }
      suggestions = list.slice(0, 5);
      suggestionSets.unshift({ id: reqId, list: suggestions, token: token });
      if (suggestionSets.length > 5) suggestionSets.pop();
      var items = suggestions.map(function (sg) {
        var bits = [];
        if (sg.distance !== undefined) bits.push(fmt.distance(sg.distance, S.imperial));
        if (sg.address) bits.push(sg.address);
        return { title: fmt.clip(sg.name, 46), sub: fmt.clip(bits.join(' · '), 62) };
      });
      send({ cmd: CMD.LIST, num: P.LIST.SUGGEST, idx: reqId, list: fmt.packList(items) || '' });
    });
  });
}

function onNearby(p) {
  if (needKey()) return;
  var cat = P.CATEGORIES[p.idx] || P.CATEGORIES[0];
  sendBusy('Finding ' + cat.name.toLowerCase());
  getLocation(60000, function (err, loc) {
    if (err) return sendError(err);
    var open = p.mode === 1;   // the "Open now" switch: hide places that are closed right now
    google.searchNearby(cat, loc, function (e2, list) {
      if (e2) return sendError(e2);
      results = list;
      if (!list.length) {
        return sendError({ code: P.ERR.NO_RESULTS, text: 'No ' + cat.name.toLowerCase() + (open ? ' open right now' : '') + ' nearby.' +
          (open ? ' Turn off "Open now" in Places to see all.' : '') });
      }
      sendList(P.LIST.RESULTS, resultItems());
    }, open);
  });
}

function onResultsMap(p) {
  if (!results.length) return;
  var size = clampSize(p.width, p.height);
  var restore = p.mode >= 10000;
  var bottom = (p.mode > 0 ? p.mode : 60) % 10000;
  var pts = results.map(function (r) { return [r.lat, r.lng]; });
  var fit = geo.fitBounds(pts, size[0], size[1], { top: 52, bottom: bottom + 4, left: 16, right: 16 }, 16);
  var list = results;
  showView(p.seq, 'results', { center: fit.center, zoom: fit.zoom, w: size[0], h: size[1] }, function (v) {
    var markers = list.map(function (r, i) { return markerFor([r.lat, r.lng], v, P.MK.PIN, i); });
    markers.unshift(meMarker(v));
    return markers;
  }, restore);
}

// Resolve the chosen destination's coordinates (favorites may only have an address)
function withDest(cb) {
  if (!dest) return cb({ code: P.ERR.API, text: 'Pick a place first.' });
  if (dest.lat !== undefined && dest.lat !== null && !isNaN(dest.lat)) return cb(null, dest);
  var target = dest;
  if (destWaiters && destWaiters.target === target) { destWaiters.cbs.push(cb); return; }
  var pendingDest = destWaiters = { target: target, cbs: [cb] };
  function finish(err) {
    if (destWaiters === pendingDest) destWaiters = null;
    pendingDest.cbs.forEach(function (fn) { fn(err, err ? null : target); });
  }
  if (target.fromSuggest && target.placeId) {
    // a place picked from the typing suggestions: ask Google where it is
    var token = target.suggestToken;
    acToken = null;
    return google.placeDetails(target.placeId, token, function (e0, pl) {
      if (e0 || !pl || pl.lat === undefined) return finish(e0 || { code: P.ERR.NO_RESULTS, text: 'Couldn\'t find that place.' });
      target.lat = pl.lat;
      target.lng = pl.lng;
      target.fullAddress = pl.fullAddress;
      target.type = pl.type;
      if (pl.name) target.name = pl.name;
      finish(null);
    });
  }
  google.geocode(target.address || target.name, function (err, g) {
    if (!err && g) {
      target.lat = g.lat;
      target.lng = g.lng;
      target.placeId = target.placeId || g.placeId;
      target.fullAddress = g.address;
      if (target.favIndex !== undefined) settings.updateFavoriteLocation(target.favIndex, g);
      return finish(null);
    }
    // fall back to a place search (handles names like "Riverfront Park")
    google.searchText(target.address || target.name, me, function (e2, list) {
      if (!e2 && list && list.length) {
        target.lat = list[0].lat;
        target.lng = list[0].lng;
        target.placeId = target.placeId || list[0].placeId;
        if (target.favIndex !== undefined) settings.updateFavoriteLocation(target.favIndex, list[0]);
        return finish(null);
      }
      finish(err || e2 || { code: P.ERR.NO_RESULTS, title: 'Address not found', text: 'Google couldn\'t find "' + fmt.clip(target.address || target.name, 60) + '". Check the address in Settings.' });
    });
  });
}

function destMode(d) {
  if (d && d.favIndex !== undefined) {
    var f = settings.favorites()[d.favIndex];
    return settings.favMode(f, S);
  }
  return S.defaultMode;
}

function sendPlace() {
  var selected = dest;
  withDest(function (err, d) {
    if (dest !== selected) return;
    if (err) return sendError(err);
    var distText = me ? fmt.distance(geo.haversine(me, [d.lat, d.lng]), S.imperial) + ' away' : '';
    send({
      cmd: CMD.PLACE,
      text: fmt.clip(d.name || d.address, 60),
      text2: fmt.clip(d.type || (d.favIndex !== undefined ? 'Favorite' : ''), 40),
      text3: fmt.clip(d.fullAddress || d.address || '', 90),
      text4: distText,
      flags: settings.findFavorite(d) >= 0 ? 1 : 0,
      mode: destMode(d)
    });
  });
}

function onSelect(p) {
  var src = p.mode, idx = p.idx;
  var d = null;
  if (src === P.SRC.RESULTS) d = results[idx];
  else if (src === P.SRC.FAVS) {
    var f = settings.favorites()[idx];
    if (f) {
      d = JSON.parse(JSON.stringify(f));
      d.favIndex = idx;
      if (f.geocodedFrom !== undefined && f.geocodedFrom !== f.address) { d.lat = undefined; d.lng = undefined; }
      if (f.lat === undefined || f.lat === null || f.lat === '') { d.lat = undefined; d.lng = undefined; }
    }
  } else if (src === P.SRC.RECENTS) d = settings.recents()[idx];
  else if (src === P.SRC.POI && pois[idx]) d = JSON.parse(JSON.stringify(pois[idx]));
  else if (src === P.SRC.SUGGEST) {
    var set = suggestionSets.filter(function (s) { return s.id === p.num; })[0];
    if (!set && p.num === undefined) set = suggestionSets[0];   // older watch builds
    var sg = set && set.list[idx];
    if (sg) d = { name: sg.name, address: sg.address, placeId: sg.placeId, fromSuggest: true, suggestToken: set.token };
  }
  if (!d) return sendError({ code: P.ERR.API, text: 'That place is no longer available.' });
  dest = d;
  route = null;
  sendPlace();
}

function onPlaceMap(p) {
  var size = clampSize(p.width, p.height);
  withDest(function (err, d) {
    if (err) return;
    // move the pin down a little so its body is centered
    var q = geo.project(d.lat, d.lng, 16);
    var base = { center: geo.unproject(q[0], q[1] - 10, 16), zoom: 16, w: size[0], h: size[1] };
    showView(p.seq, 'place', base, function (v) {
      var markers = [markerFor([d.lat, d.lng], v, P.MK.DEST)];
      markers.unshift(meMarker(v));
      return markers;
    }, false);
  });
}

function prefs() {
  return { imperial: S.imperial, avoidTolls: S.avoidTolls, avoidHighways: S.avoidHighways, avoidFerries: S.avoidFerries };
}

var modeTimes = null;   // last travel times: { key, time, items }

function onModeTimes() {
  withDest(function (err, d) {
    if (err) return sendError(err);
    var items = P.MODE_NAMES.map(function (n) { return { title: n, sub: '' }; });
    if (!S.allModeTimes || S.economical) return sendList(P.LIST.MODES, items);
    getLocation(60000, function (e1, loc) {
      if (e1) return sendList(P.LIST.MODES, items);
      // same place, about the same spot, in the last two minutes: show those times right away
      var ck = [d.lat.toFixed(5), d.lng.toFixed(5), loc[0].toFixed(3), loc[1].toFixed(3), S.imperial].join(',');
      if (modeTimes && modeTimes.key === ck && Date.now() - modeTimes.time < 120000) return sendList(P.LIST.MODES, modeTimes.items);
      var pending = 4, first = true;
      items.forEach(function (it) { it.sub = 'Checking...'; });
      [0, 1, 2, 3].forEach(function (mode) {
        google.computeRoute(loc, d, mode, prefs(), false, function (e2, r) {
          if (dest !== d) return;
          if (e2 || !r) items[mode].sub = e2 && e2.code === P.ERR.NO_RESULTS ? 'Not available here' : 'Unavailable';
          else items[mode].sub = fmt.duration(fmt.seconds(r.duration)) + ' · ' + fmt.distance(r.distanceMeters, S.imperial);
          if (--pending === 0) {
            modeTimes = { key: ck, time: Date.now(), items: items };
            sendList(P.LIST.MODES, items);
          } else if (first && pending <= 2) {
            first = false;   // half the answers in: show them, the rest fill in when ready
            sendList(P.LIST.MODES, items);
          }
        });
      });
    });
  });
}

function getRoute(mode, force, cb) {
  withDest(function (err, d) {
    if (err) return cb(err);
    getLocation(60000, function (e1, loc) {
      if (dest !== d) return cb({ stale: true });
      if (e1) return cb(e1);
      var key = [d.lat.toFixed(5), d.lng.toFixed(5), mode].join(',');
      if (!force && route && routeKey === key && Date.now() - routeTime < 180000 &&
          geo.haversine(route.points[0] || loc, loc) < 150) {
        return cb(null, route);
      }
      google.computeRoute(loc, d, mode, prefs(), true, function (e2, r) {
        if (dest !== d) return cb({ stale: true });
        if (e2) return cb(e2);
        route = nav.buildRoute(r, mode, d);
        routeKey = key;
        routeTime = Date.now();
        cb(null, route);
      });
    });
  });
}

function routeExtra(rt) {
  var tr = rt.steps.filter(function (s) { return s.transit; })[0];
  if (tr) {
    var t = tr.transit;
    return fmt.clip(t.line + (t.departs ? ' at ' + t.departs : '') + (t.from ? ' from ' + t.from : ''), 60);
  }
  if (rt.hasTolls) return 'Route has tolls';
  return '';
}

function onRoute(p) {
  var mode = p.mode >= 0 && p.mode <= 3 ? p.mode : S.defaultMode;
  var size = clampSize(p.width, p.height);
  sendBusy('Getting directions');
  getRoute(mode, false, function (err, rt) {
    if (err && err.stale) return;
    if (err) return sendError(err);
    settings.addRecent(rt.dest);
    var traffic = 0;
    if (mode === P.MODE.DRIVE && rt.staticDuration > 0) {
      var ratio = rt.duration / rt.staticDuration;
      traffic = ratio > 1.35 ? 2 : (ratio > 1.12 ? 1 : 0);
    }
    send({
      cmd: CMD.ROUTE_INFO,
      text: fmt.duration(rt.duration),
      text2: fmt.distance(rt.distanceMeters || rt.total, S.imperial),
      text3: rt.description ? fmt.clip('via ' + rt.description, 60) : fmt.clip('to ' + (rt.dest.name || ''), 60),
      text4: routeExtra(rt),
      num2: Math.round(Date.now() / 1000 + rt.duration),
      flags: traffic,
      mode: mode
    });
    var pts = rt.points.length ? rt.points : [me, [rt.dest.lat, rt.dest.lng]];
    var fit = geo.fitBounds(pts, size[0], size[1], { top: 34, bottom: 14, left: 14, right: 14 }, 17);
    var base = { center: fit.center, zoom: fit.zoom, w: size[0], h: size[1], path: geo.thin(pts, 600) };
    showView(p.seq, 'route', base, function (v) {
      return [markerFor(pts[0], v, P.MK.START), markerFor(pts[pts.length - 1], v, P.MK.DEST)];
    }, false);
  });
}

function onSteps() {
  var rt = (navigator_ && navigator_.route) || route;
  if (!rt) return sendList(P.LIST.STEPS, []);
  sendList(P.LIST.STEPS, nav.stepList(rt, S.imperial).slice(0, 40));
}

// --- Navigation ------------------------------------------------------------
// The navigation map follows you. The phone cuts it out of a big Google
// picture, turned so your direction of travel points up (or north-up), and
// sends a fresh one only when you turn or get near its edge. In between, the
// watch slides the picture under your position arrow.
var navState = null;
var voiceOn = false;

voice.onProblem(function (err) {
  console.log('voice: ' + (err.title || '') + ' ' + (err.text || ''));
  var t = err.code === P.ERR.NETWORK ? 'Voice: no connection' :
    err.blocked ? 'Voice: add Text-to-Speech to your key (Settings step 5)' :
    / is off$/.test(err.title || '') ? 'Voice: turn on Text-to-Speech (Settings step 4)' :
    'Voice: ' + (err.title || 'not working');
  toast(t);
});
var voiceTimer = null;

function navBaseZoom(mode) {
  return mode === P.MODE.DRIVE ? 16 : (mode === P.MODE.WALK ? 17 : 16);
}

function angleDiff(a, b) {
  return Math.abs(((a - b + 540) % 360) - 180);
}

// screen offset (dx,dy) -> world offset, for a picture turned to heading h
function toWorld(dx, dy, h) {
  var a = geo.rad(h), c = Math.cos(a), s = Math.sin(a);
  return [dx * c - dy * s, dx * s + dy * c];
}
function toScreenOffset(wx, wy, h) {
  var a = geo.rad(h), c = Math.cos(a), s = Math.sin(a);
  return [wx * c + wy * s, -wx * s + wy * c];
}

function navZoom() {
  return Math.max(3, Math.min(20, navBaseZoom(navigator_.route.mode) + navState.zoomDelta));
}

// Where the screen should be focused right now (world pixels) and the picture turn
function navTarget() {
  var st = navState, r = navigator_.route;
  if (st.overview) {
    var pts = r.points.length ? r.points : [navigator_.pos];
    var fit = geo.fitBounds(pts, st.w, st.h, { top: 24, bottom: 24, left: 16, right: 16 }, 17);
    return { zoom: fit.zoom, focus: geo.project(fit.center[0], fit.center[1], fit.zoom), heading: 0, follow: false };
  }
  var z = navZoom();
  var heading = st.headingUp ? (navigator_.heading || 0) : 0;
  if (st.pan) {
    return { zoom: z, focus: geo.project(st.pan[0], st.pan[1], z), heading: st.panHeading, follow: false };
  }
  // look ahead: your arrow sits a bit below the middle
  var pos = geo.project(navigator_.pos[0], navigator_.pos[1], z);
  var hd = geo.rad(navigator_.heading || 0);
  var ahead = 0.10 * st.h;
  return { zoom: z, focus: [pos[0] + Math.sin(hd) * ahead, pos[1] - Math.cos(hd) * ahead], heading: heading, follow: true };
}

// Fill NAV message with arrow / focus positions inside the picture the watch has
function navAugment(d) {
  var st = navState;
  if (!st || !st.shown || !wimg || !navigator_ || !navigator_.pos) return d;
  var sh = wimg;
  var pos = geo.project(navigator_.pos[0], navigator_.pos[1], sh.zoom);
  var pv = toScreenOffset(pos[0] - sh.c[0], pos[1] - sh.c[1], sh.heading);
  d.width = Math.round(pv[0] + sh.w / 2);
  d.height = Math.round(pv[1] + sh.h / 2);
  var t = navTarget();
  if (t.zoom === sh.zoom && !st.overview) {
    var fv = toScreenOffset(t.focus[0] - sh.c[0], t.focus[1] - sh.c[1], sh.heading);
    d.stride = Math.round(fv[0] + sh.w / 2);
    d.total = Math.round(fv[1] + sh.h / 2);
  } else {
    d.stride = Math.round(sh.w / 2);
    d.total = Math.round(sh.h / 2);
  }
  d.offset = Math.round((((navigator_.heading || 0) - sh.heading) % 360 + 360) % 360);
  d.mode = ((Math.round(sh.heading) % 360) + 360) % 360;
  d.img = sh.id;
  d.ack = panAck;
  return d;
}

// Direction of travel inside a picture turned to `heading` (unit vector, screen coords)
function travelDir(heading) {
  var hd = geo.rad(navigator_.heading || 0);
  return toScreenOffset(Math.sin(hd), -Math.cos(hd), heading);
}

function navNeedsRender(t) {
  var st = navState, sh = st.shown, w = wimg;
  if (!sh || !w || w.zoom !== t.zoom || sh.follow !== t.follow || sh.overview !== st.overview) return true;
  if (angleDiff(w.heading, t.heading) > 15) return true;
  // how much picture is left around the whole screen (cards may be hidden)
  var fv = toScreenOffset(t.focus[0] - w.c[0], t.focus[1] - w.c[1], w.heading);
  var mx = (w.w - st.w) / 2, my = (w.h - st.h) / 2;
  var left = mx + fv[0], right = mx - fv[0], top = my + fv[1], bottom = my - fv[1];
  if (Math.min(left, right, top, bottom) < 0) return true;
  if (sh.follow && (mx > 8 || my > 8)) {
    var d = travelDir(w.heading);
    var ahead = Math.abs(d[1]) >= Math.abs(d[0]) ? (d[1] < 0 ? top : bottom) : (d[0] < 0 ? left : right);
    if (ahead < 8) return true;
  }
  return false;
}

function navRender(force) {
  var st = navState;
  if (!st || !navigator_ || !navigator_.pos || navigator_.arrived) return;
  var t = navTarget();
  if (!force && !navNeedsRender(t)) return;
  if (st.streaming && !force) { st.pending = true; return; }
  var r = navigator_.route;
  var d = r.dest;
  // keep the picture's turn while the heading only drifts a little: the watch can then just slide it
  var heading = t.heading;
  if (wimg && wimg.zoom === t.zoom && angleDiff(wimg.heading, heading) <= 15) heading = wimg.heading;
  // picture center: shifted toward where you're going, so the spare picture is in front of
  // you, while still covering the whole screen behind (even with the cards hidden)
  var os = outSize(st.w, st.h);
  var c = t.focus;
  if (t.follow) {
    var dir = travelDir(heading);
    var ax = Math.max(0, (os[0] - st.w) / 2 - 6), ay = Math.max(0, (os[1] - st.h) / 2 - 6);
    var off = toWorld(dir[0] * ax, dir[1] * ay, heading);
    c = [t.focus[0] + off[0], t.focus[1] + off[1]];
  }
  var plan = planImage(c, t.zoom, heading, os[0], os[1]);
  var seq = st.seq;
  var token = ++renderToken;
  st.streaming = true;
  st.pending = false;
  clearTimeout(st.streamTimer);
  st.streamTimer = setTimeout(function () { if (navState) navState.streaming = false; }, 9000);
  var view = { zoom: t.zoom, heading: heading, focus: t.focus, follow: t.follow, overview: st.overview };
  currentMapSeq = seq;
  refreshFn = function () { if (navState && navState.seq === seq) { navState.streaming = false; navRender(true); } };
  var o = {
    c: plan.c, center: geo.unproject(plan.c[0], plan.c[1], t.zoom), zoom: t.zoom, w: os[0], h: os[1], heading: heading,
    style: mapStyle(), format: watch.fmt, path: geo.thin(r.points, 300),
    markers: d ? ['size:small|color:red|' + d.lat.toFixed(6) + ',' + d.lng.toFixed(6)] : null,
    ahead: t.follow ? toWorld(0, -1, navigator_.heading || 0) : null
  };
  tiles.get(o, function (err, img) {
    if (!navState || navState.seq !== seq || token !== renderToken) return;
    if (err) { navState.streaming = false; retryLater(err); return; }
    mapWorked();
    pushImage(seq, img, plan, { zoom: t.zoom, heading: heading, key: tiles.styleKey(o) }, null, {
      sw: st.w, sh: st.h,
      onBegin: function () {
        navState.shown = view;
        if (navigator_ && navigator_.lastDict) send(navAugment(JSON.parse(JSON.stringify(navigator_.lastDict))), 'nav');
      },
      onDone: function () {
        if (!navState) return;
        navState.streaming = false;
        if (navState.pending) navRender(false);
      }
    });
  });
}

function navSend(d) {
  if (d.cmd === CMD.NAV && !(d.flags & P.NAV_FLAG.ARRIVED)) {
    navRender(false);
    navAugment(d);
  }
  send(d, 'nav');
}

function navAdjust(p) {
  var st = navState;
  if (!navigator_ || !navigator_.pos) return;
  st.seq = p.seq;
  switch (p.idx) {
    case P.ADJ.ZOOM_IN: st.zoomDelta = Math.min(4, st.zoomDelta + 1); break;
    case P.ADJ.ZOOM_OUT: st.zoomDelta = Math.max(-6, st.zoomDelta - 1); break;
    case P.ADJ.PAN: {
      var t = navTarget();
      var sh = st.shown;
      var h = sh ? sh.heading : t.heading;
      var w = toWorld(p.width || 0, p.height || 0, h);
      var z = t.zoom;
      if (st.overview) { st.zoomDelta = z - navBaseZoom(navigator_.route.mode); st.overview = false; }
      st.pan = geo.unproject(t.focus[0] + w[0], t.focus[1] + w[1], z);
      st.panHeading = h;
      break;
    }
    case P.ADJ.FIT_ROUTE: st.overview = true; st.pan = null; break;
    default: st.pan = null; st.overview = false; break;
  }
  st.streaming = false;
  navRender(true);
}

function onNavView(p) {
  if (!navState) return;
  navState.headingUp = p.idx === 1;
  if (p.seq !== undefined) navState.seq = p.seq;
  navState.streaming = false;
  navRender(true);
}

function stopNav() {
  navSession++;
  voice.cancel();
  clearTimeout(voiceTimer);
  if (watchId !== null) {
    navigator.geolocation.clearWatch(watchId);
    watchId = null;
  }
  if (simTimer) { clearInterval(simTimer); simTimer = null; }
  if (gpsTimer) { clearInterval(gpsTimer); gpsTimer = null; }
  navigator_ = null;
  if (navState) clearTimeout(navState.streamTimer);
  navState = null;
  refreshFn = null;
}

function startSimulation() {
  // Test mode only: drive along the route
  var speed = { 0: 22, 1: 6, 2: 10, 3: 14 }[navigator_.route.mode] || 10;
  var s = 0;
  simTimer = setInterval(function () {
    if (!navigator_) return;
    var r = navigator_.route;
    s += speed;
    if (dev.simStopAt && s > dev.simStopAt) s = dev.simStopAt;   // test: park at a fixed spot
    var i = 0;
    while (i < r.cum.length - 2 && r.cum[i + 1] < s) i++;
    var segLen = Math.max(1, r.cum[i + 1] - r.cum[i]);
    var t = Math.min(1, (s - r.cum[i]) / segLen);
    var a = r.points[i], b = r.points[i + 1] || a;
    me = [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t];
    navigator_.update(me, 5);
    if (navigator_ && navigator_.arrived) { clearInterval(simTimer); simTimer = null; }
  }, dev.simInterval || 1000);
}

function onNavStart(p) {
  stopNav();
  var mode = p.mode >= 0 && p.mode <= 3 ? p.mode : S.defaultMode;
  navState = { seq: p.seq, w: clampSize(p.width, p.height)[0], h: clampSize(p.width, p.height)[1],
    zoomDelta: 0, pan: null, panHeading: 0, overview: false, shown: null, streaming: false, pending: false,
    headingUp: p.num !== 0, voice: voiceOn };
  var session = navSession, state = navState;
  getRoute(mode, false, function (err, rt) {
    if (session !== navSession || navState !== state) return;
    if (err) { stopNav(); if (!err.stale) sendError(err); return; }
    navigator_ = new nav.Navigator({
      route: rt,
      view: { w: p.width > 0 ? p.width : watch.w, h: p.height > 0 ? p.height : watch.h - 100 },
      settings: S,
      send: navSend,
      speak: function (text) { if (navState && navState.voice) voice.speak(text); },
      prepare: function (texts) { if (navState && navState.voice) voice.prepare(texts); },
      reroute: function (pos, cb) {
        google.computeRoute(pos, rt.dest, mode, prefs(), true, function (e2, r) {
          if (session !== navSession || navState !== state) return;
          if (e2) return cb(e2);
          route = nav.buildRoute(r, mode, rt.dest);
          routeTime = Date.now();
          cb(null, route);
          setTimeout(function () { if (session === navSession && navState === state) { state.streaming = false; navRender(true); } }, 0);
        });
      },
      onArrive: function () {
        setTimeout(function () { if (session === navSession) stopNav(); }, 1000);
      }
    });
    if (me) navigator_.update(me, meAcc, meTime);
    if (dev.simLocation) return startSimulation();
    watchId = navigator.geolocation.watchPosition(function (pos) {
      if (session !== navSession || navState !== state || !acceptFix(pos)) return;
      if (navigator_) navigator_.update(me, meAcc, meTime);
    }, function (e) {
      console.log('watchPosition error ' + (e && e.message));
    }, { enableHighAccuracy: true, maximumAge: 0, timeout: 30000 });
    gpsTimer = setInterval(function () {
      if (navigator_ && !navigator_.arrived && Date.now() - navigator_.lastFix > 20000) navigator_.noGps();
    }, 10000);
  });
}

// --- Place info page (ratings, hours, reviews, photos) ---------------------------
function plainText(t) {
  return String(t || '').replace(/[   ‎‏ ]/g, ' ').replace(/[\x1e\x1f]/g, ' ');
}

function clockText(iso) {
  var d = new Date(iso);
  if (isNaN(d.getTime())) return '';
  var h = d.getHours(), m = d.getMinutes();
  var mm = (m < 10 ? '0' : '') + m;
  if (S.imperial) return (h % 12 || 12) + (m ? ':' + mm : '') + (h < 12 ? ' AM' : ' PM');
  return h + ':' + mm;
}

function dayText(iso) {
  var d = new Date(iso), now = new Date();
  if (isNaN(d.getTime()) || d.toDateString() === now.toDateString()) return '';
  if (d.toDateString() === new Date(now.getTime() + 86400000).toDateString()) return ' tomorrow';
  return ' ' + ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'][d.getDay()];
}

function openLine(inf) {
  if (inf.status === 'CLOSED_PERMANENTLY') return { text: 'Permanently closed', known: true, open: false };
  if (inf.status === 'CLOSED_TEMPORARILY') return { text: 'Temporarily closed', known: true, open: false };
  if (inf.openNow === true) {
    if (inf.always) return { text: 'Open 24 hours', known: true, open: true };
    return { text: 'Open' + (inf.nextClose ? ' · Closes ' + clockText(inf.nextClose) + dayText(inf.nextClose) : ''), known: true, open: true };
  }
  if (inf.openNow === false) {
    return { text: 'Closed' + (inf.nextOpen ? ' · Opens ' + clockText(inf.nextOpen) + dayText(inf.nextOpen) : ''), known: true, open: false };
  }
  return { text: '', known: false, open: false };
}

// "Monday: 11:00 AM – 10:00 PM" -> "Mon  11 AM–10 PM"
function shortHours(h) {
  h = plainText(h).replace(/\s+/g, ' ').trim();
  h = h.replace(/^(Mon|Tue|Wed|Thu|Fri|Sat|Sun)[a-z]*day:/, '$1');
  return h.replace(/:00(?= ?[AP]M)/g, '').replace(/ ?– ?/g, '–');
}

function domainOf(u) {
  var m = /^[a-z]+:\/\/(?:www\.)?([^\/?#]+)/i.exec(u || '');
  return m ? m[1] : '';
}

// The first photo starts downloading as soon as the place details arrive (the watch asks for
// it a moment later); the raw picture is kept by name so the watch's request can use it.
var rawPhotos = {};
var rawOrder = [];
function photoBytes(name, cb) {
  var r = rawPhotos[name];
  if (r && r.done) return cb(r.err, r.bytes);
  if (r) { r.waiters.push(cb); return; }
  r = rawPhotos[name] = { waiters: [cb] };
  rawOrder.push(name);
  while (rawOrder.length > 4) delete rawPhotos[rawOrder.shift()];
  var side = Math.min(1200, Math.max(buf.w, buf.h, watch.w) * 3);
  google.placePhoto(name, side, side, function (err, bytes) {
    r.done = true; r.err = err; r.bytes = bytes;
    if (err) { delete rawPhotos[name]; }
    var ws = r.waiters; r.waiters = [];
    ws.forEach(function (f) { f(err, bytes); });
  });
}

function sendInfo(inf) {
  info = inf;
  if (!watch.lowmem && inf.photos && inf.photos[0]) photoBytes(inf.photos[0].name, function () {});
  var ol = openLine(inf);
  var meta = [];
  if (inf.price) meta.push(inf.price);
  if (inf.type) meta.push(inf.type);
  if (me && inf.lat !== undefined) meta.push(fmt.distance(geo.haversine(me, [inf.lat, inf.lng]), S.imperial));
  var small = watch.inbox < 4000;
  var items = [];
  if (inf.summary) items.push({ title: '', sub: fmt.clip(plainText(inf.summary), small ? 140 : 240), icon: 1 });
  if (inf.phone) items.push({ title: plainText(inf.phone), sub: '', icon: 2 });
  if (inf.website) items.push({ title: domainOf(inf.website), sub: '', icon: 3 });
  if (inf.hours.length) {
    items.push({ title: 'Hours', sub: inf.hours.map(shortHours).join('\n'), icon: 4 });
  }
  var revLen = small ? 160 : 320;
  inf.reviews.slice(0, 5).forEach(function (r) {
    if (!r.text) return;
    items.push({ title: fmt.clip(plainText(r.author), 30) + '\n' + plainText(r.when), sub: fmt.clip(plainText(r.text), revLen), icon: 5, extra: Math.round(r.rating) });
  });
  if (watch.lowmem) return sendInfoText(inf, ol, meta);
  var room = watch.inbox - 360;
  var packed = fmt.packList(items);
  while (items.length > 1 && utf8Len(packed) > room) {
    items.pop();
    packed = fmt.packList(items);
  }
  send({
    cmd: CMD.INFO_DATA,
    text: fmt.clip(plainText(inf.name), 60),
    text2: fmt.clip(meta.join(' · '), 60),
    text3: fmt.clip(ol.text, 44),
    text4: fmt.clip(plainText(inf.fullAddress || inf.address), 90),
    num: Math.round((inf.rating || 0) * 10),
    num2: inf.ratingCount || 0,
    flags: (ol.known ? 1 : 0) | (ol.open ? 2 : 0),
    idx: inf.photos.length,
    mode: poiCategory(inf.primaryType),
    list: packed || ''
  });
}

// Pebble Time / Time Round: the same page as plain text (no photos)
function sendInfoText(inf, ol, meta) {
  var lines = [];
  if (inf.rating) lines.push('Rated ' + inf.rating.toFixed(1) + ' of 5 (' + inf.ratingCount + ')');
  if (meta.length) lines.push(meta.join(' · '));
  if (ol.text) lines.push(ol.text);
  var body = lines.join('\n');
  var more = [];
  if (inf.summary) more.push(fmt.clip(plainText(inf.summary), 160));
  more.push(plainText(inf.fullAddress || inf.address));
  if (inf.phone) more.push(plainText(inf.phone));
  if (inf.website) more.push(domainOf(inf.website));
  if (inf.hours.length) more.push(inf.hours.map(shortHours).join('\n'));
  inf.reviews.slice(0, 3).forEach(function (r) {
    if (r.text) more.push(plainText(r.author) + ' · ' + Math.round(r.rating) + '/5 · ' + plainText(r.when) + '\n' + fmt.clip(plainText(r.text), 150));
  });
  more.forEach(function (t) {
    if (t && utf8Len(body + '\n\n' + t) < 900) body += '\n\n' + t;
  });
  send({ cmd: CMD.INFO_DATA, text: fmt.clip(plainText(inf.name), 44), list: body });
}

function onInfo(p) {
  if (needKey()) return;
  setMapVisible(false);
  var src = p.mode, idx = p.idx;
  function got(err, pl) {
    if (err) return sendError(err);
    function show(id) {
      for (var i = 0; i < infoCache.length; i++) if (infoCache[i].placeId === id) return sendInfo(infoCache[i]);
      google.placeInfo(id, function (e2, inf) {
        if (e2) return sendError(e2);
        infoCache.unshift(inf);
        if (infoCache.length > 8) infoCache.pop();
        sendInfo(inf);
      });
    }
    if (pl.placeId) return show(pl.placeId);
    google.searchText([pl.name, pl.address || pl.fullAddress].filter(Boolean).join(' '), me, function (e3, list) {
      if (e3 || !list || !list.length) {
        return sendError(e3 || { code: P.ERR.NO_RESULTS, title: 'No details', text: 'Google has no details for this place.' });
      }
      pl.placeId = list[0].placeId;
      show(pl.placeId);
    });
  }
  if (src === P.SRC.RESULTS && results[idx]) return got(null, results[idx]);
  if (src === P.SRC.POI && pois[idx]) return got(null, pois[idx]);
  if (src === P.SRC.DEST) return withDest(got);
  got({ code: P.ERR.API, text: 'That place is no longer available.' });
}

function onPhoto(p) {
  setMapVisible(false);
  currentMapSeq = p.seq;
  var token = ++renderToken;
  refreshFn = null;
  if (retryTimer) { clearTimeout(retryTimer); retryTimer = null; }
  var ph = info && info.photos[p.idx];
  if (!ph) return;
  var w = Math.max(16, Math.min(buf.w, p.width || watch.w)), h = Math.max(16, Math.min(buf.h, p.height || 100));
  var key = ph.name + '|' + w + 'x' + h + '|' + watch.fmt;
  var seq = p.seq;
  function push(img) {
    if (seq !== currentMapSeq || token !== renderToken) return;
    pushImage(seq, img, { c: [0, 0], ref: 0, v: [0, 0] }, { zoom: 0, heading: 0, key: 'photo' }, null);
  }
  for (var i = 0; i < photoCache.length; i++) if (photoCache[i].key === key) return push(photoCache[i].img);
  photoBytes(ph.name, function (err, bytes) {
    if (token !== renderToken) return;
    if (err) {
      console.log('photo: ' + (err.text || err.title));
      return toast('Photo didn\'t load');
    }
    var img;
    try {
      var d = bytes[0] === 0x89 ? image.decodePNG(bytes) : (function () {
        var j = jpeg(bytes, { useTArray: true, formatAsRGBA: false });
        return { width: j.width, height: j.height, rgb: j.data };
      })();
      img = image.photo(d.rgb, d.width, d.height, w, h, watch.fmt);
    } catch (e) {
      console.log('photo decode: ' + e.message);
      return toast('Photo didn\'t load');
    }
    photoCache.unshift({ key: key, img: img });
    if (photoCache.length > 6) photoCache.pop();
    push(img);
  });
}

// --- Transit schedule for one ride in the directions --------------------------------
function onTransitInfo(p) {
  var rt = (navigator_ && navigator_.route) || route;
  var step = rt && rt.steps[p.idx];
  var t = step && step.transit;
  function page(title, body) { send({ cmd: CMD.INFO_DATA, text: fmt.clip(plainText(title), 44), list: body }); }
  if (!t) return page('Schedule', 'No transit details for this step.');
  var title = t.line + (t.headsign ? ' to ' + t.headsign : '');
  if (!t.fromLoc || !t.toLoc) return page(title, 'Google didn\'t send stop locations for this ride.');
  google.transitDepartures(t.fromLoc, t.toLoc, t.vehicleType, t.line, function (err, res) {
    var lines = [];
    if (t.from) lines.push('From ' + plainText(t.from));
    if (t.agency) lines.push(plainText(t.agency));
    if (t.depTime) lines.push('Your trip: ' + clockText(t.depTime) + leaveIn(t.depTime));
    if (err) {
      lines.push('', 'Couldn\'t get more departures (' + (err.title || 'no connection') + ').');
      return page(title, lines.join('\n'));
    }
    lines.push('', 'Next departures');
    if (!res.deps.length) lines.push('None found in the next few hours.');
    res.deps.forEach(function (d) {
      var mine = t.depTime && Math.abs(new Date(d.time) - new Date(t.depTime)) < 60000;
      lines.push(clockText(d.time) + leaveIn(d.time) + (mine ? '  (yours)' : ''));
    });
    if (res.warnings.length) lines.push('', 'Notices', res.warnings.slice(0, 3).map(plainText).join('\n'));
    lines.push('', 'Times include live updates where the agency shares them with Google. Google doesn\'t pass on cancellation alerts.');
    var body = lines.join('\n');
    while (utf8Len(body) > watch.inbox - 200 && lines.length > 4) { lines.splice(lines.length - 3, 1); body = lines.join('\n'); }
    page(title, body);
  });
}

function leaveIn(iso) {
  var min = Math.round((new Date(iso).getTime() - Date.now()) / 60000);
  if (isNaN(min)) return '';
  if (min <= 0) return ' (now)';
  return min < 60 ? ' (in ' + min + ' min)' : '';
}

// --- Favorites --------------------------------------------------------------
function onFavList() {
  var favs = settings.favorites();
  sendList(P.LIST.FAVS, favs.map(function (f) {
    var m = parseInt(f.mode, 10);
    var modeText = isNaN(m) ? P.MODE_NAMES[S.defaultMode] + ' (default)' : P.MODE_NAMES[m];
    return { title: fmt.clip(f.name || f.address, 46), sub: fmt.clip(modeText + (f.address && f.name ? ' · ' + f.address : ''), 62) };
  }));
  sendList(P.LIST.RECENTS, settings.recents().map(function (r) {
    return { title: fmt.clip(r.name, 46), sub: fmt.clip(r.address || r.type || '', 62) };
  }));
}

function onFavToggle() {
  if (!dest) return;
  withDest(function (err, d) {
    if (err) return sendError(err);
    var i = settings.findFavorite(d);
    if (i >= 0) {
      settings.removeFavorite(i);
      if (d.favIndex !== undefined) delete d.favIndex;
      toast('Removed from favorites');
    } else {
      if (!settings.addFavorite(d)) return toast('Favorites full (20). Remove one first.');
      d.favIndex = settings.findFavorite(d);
      toast('Saved to favorites');
    }
  });
}

function onSaveHere() {
  getLocation(5000, function (err, loc) {
    if (err) return sendError(err);
    google.reverseGeocode(loc, function (e2, g) {
      var addr = (!e2 && g && g.address) || (loc[0].toFixed(5) + ', ' + loc[1].toFixed(5));
      var name = addr.split(',')[0];
      var place = { name: name, address: addr, lat: loc[0], lng: loc[1], placeId: (g && g.placeId) || '' };
      if (!settings.addFavorite(place)) return toast(settings.findFavorite(place) >= 0 ? 'Already saved' : 'Favorites full (20). Remove one first.');
      toast('Saved ' + fmt.clip(name, 30));
    });
  });
}

// ---------------------------------------------------------------------------
// Message routing
// ---------------------------------------------------------------------------
function onMessage(e) {
  var p = e.payload || {};
  var cmd = p.cmd;
  if (cmd === CMD.HOME_MAP || cmd === CMD.RESULTS_MAP || cmd === CMD.PLACE_MAP || cmd === CMD.ROUTE || cmd === CMD.NAV_START) {
    // the watch has moved on to a new picture: pieces of the old one would only hold it up
    msg.drop('map:');
    msg.drop('mk');
    tiles.cancelPrefetch();
  }
  try {
    switch (cmd) {
      case CMD.HELLO:
        watch.w = p.width || watch.w;
        watch.h = p.height || watch.h;
        watch.fmt = p.num === 0 ? 0 : 1;
        watch.mic = !!(p.mode & 1);
        watch.round = !!(p.mode & 2);
        watch.touch = !!(p.mode & 4);
        watch.lowmem = !!(p.mode & 8);
        watch.inbox = p.idx > 0 ? p.idx : 4096;
        voice.setInbox(watch.inbox);
        if (p.text && /^\d+,\d+$/.test(p.text)) {
          buf.w = parseInt(p.text.split(',')[0], 10);
          buf.h = parseInt(p.text.split(',')[1], 10);
        } else {
          buf.w = watch.w;
          buf.h = watch.h;
        }
        sendStatus();
        break;
      case CMD.HOME_MAP: wimg = null; onHomeMap(p); break;
      case CMD.CANCEL: setMapVisible(false); break;
      case CMD.INFO: onInfo(p); break;
      case CMD.PHOTO: onPhoto(p); break;
      case CMD.POI_MODE: {
        poiMode = !(p.idx & 1) ? 0 : ((p.idx & 2) ? 2 : 1);
        poiOpen = !!(p.idx & 4);
        if (typeof p.num === 'number') poiMask = p.num;
        var hs = views.home;
        if (hs && lastKind === 'home' && hs.markers) {
          if (p.seq !== undefined) hs.seq = p.seq;
          maybeFetchPois(hs.view);
          sendMarkers(hs, true);
        }
        break;
      }
      case CMD.FOLLOW_MODE: {
        // heading up / north up / plain map: each change brings the map back to you
        followMode = p.idx || 0;
        homeDrive = followMode === 0;
        var fs = views.home;
        if (fs && lastKind === 'home' && fs.markers) {
          if (p.seq !== undefined) fs.seq = p.seq;
          fs.follow = followMode !== 2;
          fs.view = followView(fs.view);
          streamMap(fs.seq, fs.view, fs.markers(fs.view));
          maybeFetchPois(fs.view);
        }
        break;
      }
      case CMD.TRANSIT_INFO: onTransitInfo(p); break;
      case CMD.SEARCH: onSearch(p); break;
      case CMD.NEARBY: onNearby(p); break;
      case CMD.AUTOCOMPLETE: onAutocomplete(p); break;
      case CMD.RESULTS_MAP: wimg = null; setMapVisible(true); onResultsMap(p); break;
      case CMD.SELECT: onSelect(p); break;
      case CMD.PLACE_MAP: wimg = null; setMapVisible(true); onPlaceMap(p); break;
      case CMD.MODE_TIMES: onModeTimes(); break;
      case CMD.ROUTE: wimg = null; setMapVisible(true); onRoute(p); break;
      case CMD.STEPS: onSteps(); break;
      case CMD.NAV_START: wimg = null; mapVisible = false; onNavStart(p); updateTracking(); break;
      case CMD.NAV_STOP: stopNav(); mapVisible = false; updateTracking(); break;
      case CMD.VOICE_DONE: voice.completed(p.idx, p.mode === 1); break;
      case CMD.FAV_LIST: onFavList(); break;
      case CMD.FAV_TOGGLE: onFavToggle(); break;
      case CMD.FAV_DELETE:
        settings.removeFavorite(p.idx);
        toast('Favorite removed');
        break;
      case CMD.FAV_SETMODE:
        settings.setFavoriteMode(p.idx, p.mode);
        toast(p.mode === P.MODE_USE_DEFAULT ? 'Uses app default' : 'Default: ' + P.MODE_NAMES[p.mode]);
        break;
      case CMD.SAVE_HERE: onSaveHere(); break;
      case CMD.MAP_ADJUST: onAdjust(p); break;
      case CMD.NAV_VIEW: onNavView(p); break;
      case CMD.NAV_VOICE:
        voiceOn = p.idx === 1;
        if (navState) navState.voice = voiceOn;
        clearTimeout(voiceTimer);
        if (!voiceOn) { voice.cancel(); break; }
        voice.reset();
        // speak the current direction after a moment (so tapping past "voice" to "off" stays quiet)
        voiceTimer = setTimeout(function () {
          if (!voiceOn || Date.now() - voice.lastSpoke() < 3000) return;   // already talking
          voice.speak(navigator_ && navigator_.lastSay ? navigator_.currentPhrase() : 'Voice directions on');
        }, 2000);
        if (navigator_ && navigator_.currentPhrase) voice.prepare([navigator_.currentPhrase()]);
        break;
      default:
        break;
    }
  } catch (ex) {
    console.log('Error handling ' + cmd + ': ' + ex + ' ' + (ex.stack || ''));
    sendError({ code: P.ERR.API, text: 'Phone error: ' + ex.message });
  }
}

Pebble.addEventListener('ready', function () {
  reloadSettings();
  if (dev.simLocation) me = dev.simLocation.slice();
  sendStatus();
  // warm up the location so the first map is quick
  if (google.hasKey() && !dev.simLocation) getLocation(0, function () {});
});

Pebble.addEventListener('appmessage', onMessage);

// ---------------------------------------------------------------------------
// Settings page (Clay)
// ---------------------------------------------------------------------------
Pebble.addEventListener('showConfiguration', function () {
  clay.meta = clay.meta || {};
  clay.meta.userData = {
    lat: me ? me[0] : null,
    lng: me ? me[1] : null,
    color: watch.fmt === 1,
    keyOk: (function () { try { return !!S.apiKey && localStorage.getItem('pm-key-works') === S.apiKey; } catch (e) { return false; } })()
  };
  try {
    clay.meta.activeWatchInfo = Pebble.getActiveWatchInfo ? Pebble.getActiveWatchInfo() : null;
    clay.meta.accountToken = Pebble.getAccountToken ? Pebble.getAccountToken() : '';
    clay.meta.watchToken = Pebble.getWatchToken ? Pebble.getWatchToken() : '';
  } catch (e) { /* not available in every app */ }
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function (e) {
  if (!e || !e.response) return;
  var oldKey = S.apiKey;
  try {
    clay.getSettings(e.response, false);
  } catch (ex) {
    console.log('settings parse error ' + ex);
    return;
  }
  reloadSettings();
  tiles.clear();
  route = null;
  sendStatus();
  updateTracking();
  var hs = views.home;
  if (hs && lastKind === 'home' && mapVisible && hs.markers) {
    sendMarkers(hs, true);
    maybeFetchPois(hs.view);
  }
  if (google.hasKey() && S.apiKey !== oldKey) {
    toast('Checking your key...');
    google.checkKey(me, function (res) {
      var bad = res.filter(function (r) { return !r.ok; });
      if (!bad.length) return toast('Your Google key works!');
      var first = bad[0];
      var names = bad.map(function (b) { return b.name; }).join(', ');
      sendError({
        code: first.error && first.error.code === P.ERR.NO_KEY ? P.ERR.NO_KEY : P.ERR.API,
        title: first.error && first.error.title ? first.error.title : 'Key needs a fix',
        text: (first.error && first.error.text ? first.error.text + ' ' : '') + '(Not working: ' + names + ')'
      });
    });
  }
});

module.exports = { _test: { onMessage: onMessage, state: function () { return { results: results, dest: dest, route: route }; } } };
