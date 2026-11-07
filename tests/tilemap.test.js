const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const root = path.resolve(__dirname, '../src/pkjs');

let last = null;
test.afterEach(() => { if (last) last.stop(); });

// Fakes for the pieces tilemap.js talks to: the send queue and the cache of Google pictures
function setup(opts) {
  opts = opts || {};
  const sent = [];
  const inflight = [];
  const fakeMsg = {
    send(dict, tag, done) { const item = { dict, tag, done }; sent.push(item); inflight.push(item); },
    drop() {},
    pending() { return inflight.length; }
  };
  const covered = [];   // rectangles the "cache" has pictures for
  const fetches = [];
  const fakeTiles = {
    SIZE: 640,
    peekRect(o, r) { return covered.some(c => c.z === o.zoom && r.x0 >= c.x0 && r.y0 >= c.y0 && r.x1 <= c.x1 && r.y1 <= c.y1) ? {} : null; },
    centreFor(r, want) { return want; },
    fetchPicture(o, c, cb) {
      fetches.push({ o, c });
      covered.push({ z: o.zoom, x0: c[0] - 300, y0: c[1] - 300, x1: c[0] + 300, y1: c[1] + 300 });
      setImmediate(() => cb(null, {}));
    },
    cutBlock(pic, wx, wy, size) {
      const out = new Uint8Array(size * size);
      for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
        out[y * size + x] = ((wx + x) % 40 === 0 || (wy + y) % 33 === 0) ? 0x15 : 0xe0;   // flat fill with thin lines, like a map
      }
      return out;
    },
    prefetchAround() {}
  };
  for (const [name, exp] of [['msg', fakeMsg], ['tiles', fakeTiles], ['dev', {}]]) {
    const file = require.resolve(root + '/' + name);
    require.cache[file] = { id: file, filename: file, loaded: true, exports: exp };
  }
  delete require.cache[require.resolve(root + '/tilemap')];
  const tilemap = require(root + '/tilemap');
  const state = { seq: 7, failed: [], worked: 0 };
  const watch = { inbox: 4096, tileStart: 1280, tileEnd: opts.end || 49000, tileSlots: opts.slots || 80 };
  tilemap.init({
    watch: () => watch, style: () => 'light', economical: () => !!opts.economical,
    seq: () => state.seq, fail: e => state.failed.push(e), worked: () => state.worked++
  });
  const ack = () => { const it = inflight.shift(); if (it && it.done) it.done(true); return it; };
  const settle = async (n) => { for (let i = 0; i < (n || 60); i++) { await new Promise(r => setImmediate(r)); while (inflight.length) ack(); } };
  last = tilemap;
  return { tilemap, sent, inflight, state, watch, fetches, covered, ack, settle, fakeTiles };
}

const CMD = require(root + '/protocol').CMD;
function records(dict) {
  // parse a TILES message into table entries
  const b = Buffer.from(dict.data), out = [];
  for (let p = 0; p + 16 <= b.length;) {
    const len = b.readUInt16LE(p + 10);
    out.push({ x: b.readInt32LE(p), y: b.readInt32LE(p + 4), off: b.readUInt16LE(p + 8), len, z: b[p + 12], slot: b[p + 13] });
    p += 16 + len;
  }
  return out;
}
const tilesOf = (sent) => sent.filter(s => s.dict.cmd === CMD.TILES);

test('a new session says where the view is, then sends the screen first and flags it when complete', async () => {
  const t = setup();
  const v = { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 };
  t.tilemap.begin(7, v);
  assert.equal(t.sent[0].dict.cmd, CMD.MAP_BEGIN);
  assert.equal(t.sent[0].dict.fmt, 2);
  assert.equal(t.sent[0].dict.flags, 1);
  assert.equal(t.sent[0].dict.idx, 6500);
  assert.equal(t.sent[0].dict.palette.length, 16);
  t.tilemap.kick();
  await t.settle();
  const batches = tilesOf(t.sent);
  assert.ok(batches.length >= 1);
  const all = [].concat(...batches.map(b => records(b.dict)));
  // the 16 screen tiles come before anything else
  const inScreen = e => e.x < 6600 && e.x + 64 > 6400 && e.y < 6628 && e.y + 64 > 6400;
  const firstOutside = all.findIndex(e => !inScreen(e));
  assert.ok(firstOutside >= 16, 'screen tiles first, got ' + firstOutside);
  // the screen-complete flag rides on the batch with the last screen tile and nothing earlier
  const flagged = batches.findIndex(b => b.dict.flags === 1);
  let n = 0;
  batches.forEach((b, i) => { n += records(b.dict).filter(inScreen).length; if (i === flagged) assert.ok(n >= 16); });
  assert.equal(batches.filter(b => b.dict.flags === 1).length, 1);
  assert.ok(flagged <= 5);
});

test('tiles never overlap in the arena and stay inside it', async () => {
  const t = setup({ end: 20000, slots: 60 });
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const live = new Map();
  for (const b of tilesOf(t.sent)) {
    for (const r of records(b.dict)) {
      assert.ok(r.off >= 1280 && r.off + r.len <= 20000);
      for (const [slot, o] of live) {
        if (slot !== r.slot && o.off < r.off + r.len && r.off < o.off + o.len) live.delete(slot);   // the watch drops what is overwritten
      }
      live.set(r.slot, r);
    }
  }
  const list = [...live.values()].sort((a, b) => a.off - b.off);
  for (let i = 1; i < list.length; i++) assert.ok(list[i - 1].off + list[i - 1].len <= list[i].off);
  // what the watch still holds covers the whole screen
  for (let x = 6400; x < 6600; x += 64) for (let y = 6400; y < 6628; y += 64) {
    assert.ok(list.some(e => e.x === Math.floor(x / 64) * 64 && e.y === Math.floor(y / 64) * 64), 'screen tile ' + x + ',' + y);
  }
});

test('moving the view loads the tiles around the new spot and pushes out the farthest ones', async () => {
  const t = setup({ end: 24000, slots: 60 });
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const before = tilesOf(t.sent).length;
  t.tilemap.move({ cx: 6500 + 640, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const after = tilesOf(t.sent).slice(before);
  assert.ok(after.length > 0);
  const recs = [].concat(...after.map(b => records(b.dict)));
  assert.ok(recs.slice(0, 16).every(e => e.x >= 7140 - 128 && e.x <= 7140 + 128), 'the new screen is first');
});

test('a recentring waits for the new screen\'s tiles, then moves the view', async () => {
  const t = setup();
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const n = t.sent.length;
  t.tilemap.adopt({ cx: 9000, cy: 9000, w: 200, h: 228, z: 15 });   // far away: needs new tiles first
  t.tilemap.kick();
  await t.settle();
  const after = t.sent.slice(n);
  const view = after.findIndex(s => s.dict.cmd === CMD.MAP_BEGIN);
  assert.ok(view > 0, 'view message comes after some tiles');
  assert.equal(after[view].dict.flags, 0);
  assert.equal(after[view].dict.idx, 9000);
  const before = [].concat(...after.slice(0, view).filter(s => s.dict.cmd === CMD.TILES).map(s => records(s.dict)));
  for (let x = 8960; x < 9100; x += 64) assert.ok(before.some(e => e.x === x), 'tile column ' + x + ' arrived first');
});

test('tiles already there make a recentring immediate', async () => {
  const t = setup();
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const n = t.sent.length;
  t.tilemap.adopt({ cx: 6520, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const view = t.sent.slice(n).find(x => x.dict.cmd === CMD.MAP_BEGIN);
  assert.ok(view, 'view message sent without waiting');
  assert.equal(view.dict.idx, 6520);
});

test('a batch that is not delivered is sent again', async () => {
  const t = setup();
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.ack();   // the begin message
  t.tilemap.kick();
  await new Promise(r => setImmediate(r));
  await new Promise(r => setImmediate(r));
  const first = tilesOf(t.sent)[0];
  assert.ok(first);
  const firstSlots = records(first.dict).map(r => r.x + ',' + r.y);
  t.inflight.splice(t.inflight.indexOf(first), 1);
  first.done(false);
  await new Promise(r => setTimeout(r, 1100));
  await t.settle(80);
  const again = [].concat(...tilesOf(t.sent).slice(1).map(b => records(b.dict).map(r => r.x + ',' + r.y)));
  firstSlots.forEach(k => assert.ok(again.includes(k), 'resent ' + k));
});

test('nothing is sent once another screen has the watch', async () => {
  const t = setup();
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle(3);
  t.state.seq = 8;
  const n = t.sent.length;
  t.tilemap.kick();
  await t.settle();
  assert.equal(t.sent.length, n);
});

test('economical mode keeps to one ring', async () => {
  const t = setup({ economical: true });
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const recs = [].concat(...tilesOf(t.sent).map(b => records(b.dict)));
  assert.ok(recs.length <= 36, 'screen (16) plus one ring (20): ' + recs.length);
  assert.ok(recs.every(e => e.x >= 6336 - 64 && e.x <= 6600 + 64));
});

test('zooming keeps the old tiles for a while and loads the new zoom centre-out', async () => {
  const t = setup({ end: 30000 });
  t.tilemap.begin(7, { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 });
  t.tilemap.kick();
  await t.settle();
  const n = tilesOf(t.sent).length;
  t.tilemap.move({ cx: 13000, cy: 13028, w: 200, h: 228, z: 16 });
  t.tilemap.kick();
  await t.settle();
  const recs = [].concat(...tilesOf(t.sent).slice(n).map(b => records(b.dict)));
  assert.ok(recs.length >= 16);
  assert.ok(recs.slice(0, 16).every(e => e.z === 16 && e.x >= 12800 - 64 && e.x <= 13200));
});
