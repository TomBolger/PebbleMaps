const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const ts = require(path.resolve(__dirname, '../src/pkjs/tilestore'));

function rng(seed) { let s = seed; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

// the store's books must always add up: tiles and free space don't overlap and fill the arena exactly
function check(st) {
  const ext = Object.values(st.tiles).map(t => ({ off: t.off, len: t.len, what: 't' }))
    .concat(st.free.map(f => ({ off: f.off, len: f.len, what: 'f' }))).sort((a, b) => a.off - b.off);
  let pos = st.start;
  ext.forEach(e => { assert.equal(e.off, pos); pos += e.len; });
  assert.equal(pos, st.end);
  for (let i = 1; i < st.free.length; i++) assert.ok(st.free[i - 1].off + st.free[i - 1].len < st.free[i].off, 'free extents merged');
  const slots = Object.values(st.tiles).map(t => t.slot);
  assert.equal(new Set(slots).size, slots.length);
  slots.forEach(s => assert.ok(s >= 0 && s < st.nslots && st.used[s]));
}

test('first fit places tiles one after another and reuses freed room', () => {
  const st = new ts.Store(960, 5960, 8);
  const a = st.place({ x: 0, y: 0, z: 15 }, 1000, () => false, () => 0);
  const b = st.place({ x: 1, y: 0, z: 15 }, 500, () => false, () => 0);
  assert.deepEqual([a.slot, a.off, b.slot, b.off], [0, 960, 1, 1960]);
  st.remove(st.get(0, 0, 15));
  const c = st.place({ x: 2, y: 0, z: 15 }, 800, () => false, () => 0);
  assert.equal(c.off, 960);
  assert.equal(c.slot, 0);
  check(st);
});

test('evicts the farthest tiles, never the ones that must stay', () => {
  const st = new ts.Store(0, 3000, 10);
  for (let i = 0; i < 3; i++) st.place({ x: i, y: 0, z: 15 }, 1000, () => false, () => 0);
  const rank = t => t.x;                       // x = 2 is the farthest
  const r = st.place({ x: 9, y: 9, z: 15 }, 900, () => false, rank);
  assert.deepEqual(r.victims.map(v => v.x), [2]);
  assert.equal(r.off, 2000);
  // when everything is protected there is no way
  const none = st.place({ x: 8, y: 8, z: 15 }, 900, () => true, rank);
  assert.equal(none, null);
  assert.equal(st.count(), 3);
  check(st);
});

test('a failed placement changes nothing', () => {
  const st = new ts.Store(0, 2000, 4);
  st.place({ x: 0, y: 0, z: 1 }, 900, () => false, () => 0);
  st.place({ x: 1, y: 0, z: 1 }, 900, () => false, () => 0);
  const before = JSON.stringify([st.tiles, st.free, st.used]);
  assert.equal(st.place({ x: 2, y: 0, z: 1 }, 1500, t => t.x === 1, () => 0), null);
  assert.equal(JSON.stringify([st.tiles, st.free, st.used]), before);
});

test('table slots run out before the arena does', () => {
  const st = new ts.Store(0, 100000, 3);
  for (let i = 0; i < 3; i++) st.place({ x: i, y: 0, z: 5 }, 100, () => false, () => 0);
  assert.equal(st.place({ x: 7, y: 0, z: 5 }, 100, () => true, () => 0), null);
  const r = st.place({ x: 7, y: 0, z: 5 }, 100, () => false, t => t.x);
  assert.equal(r.victims.length, 1);
  assert.equal(r.slot, 2);
  check(st);
});

test('random use keeps the books straight', () => {
  const rnd = rng(7);
  const st = new ts.Store(960, 49200, 96);
  const view = { cx: 0, cy: 0, z: 15 };
  for (let n = 0; n < 3000; n++) {
    const t = { x: Math.floor(rnd() * 40) - 20, y: Math.floor(rnd() * 40) - 20, z: 15 };
    if (st.has(t.x, t.y, t.z)) { if (rnd() < 0.3) st.remove(t); continue; }
    const len = 200 + Math.floor(rnd() * 1800);
    st.place(t, len, () => false, c => ts.rank(c, view));
    if (n % 50 === 0) check(st);
  }
  check(st);
  assert.ok(st.count() > 20);
});

test('the screen comes first, centre-out, then rings leaning the way we move', () => {
  const v = { cx: 6500, cy: 6514, w: 200, h: 228, z: 15 };
  const w = ts.want(v, { rings: 2, dir: null });
  assert.equal(w.screen, 16);   // 4 x 4 tiles when the view lines up, 5 x 5 at worst
  const screen = w.list.slice(0, w.screen);
  // every tile of the screen rectangle, nothing else
  screen.forEach(t => {
    assert.ok(t.x * 64 < v.cx + 100 && t.x * 64 + 64 > v.cx - 100);
    assert.ok(t.y * 64 < v.cy + 114 && t.y * 64 + 64 > v.cy - 114);
  });
  for (let i = 1; i < screen.length; i++) assert.ok(screen[i - 1].d <= screen[i].d);
  // moving east: ring tiles to the east come before ring tiles to the west at the same distance
  const e = ts.want(v, { rings: 2, dir: [1, 0] }).list.slice(w.screen);
  const firstEast = e.findIndex(t => t.x * 64 + 32 > v.cx + 100);
  const firstWest = e.findIndex(t => t.x * 64 + 32 < v.cx - 100);
  assert.ok(firstEast >= 0 && firstWest >= 0 && firstEast < firstWest);
  // no duplicates, all inside the world
  const keys = new Set(w.list.map(t => t.x + ',' + t.y));
  assert.equal(keys.size, w.list.length);
});

test('want stays inside the world and honours the cap', () => {
  const w = ts.want({ cx: 10, cy: 10, w: 200, h: 228, z: 2 }, { rings: 3, max: 30 });
  assert.ok(w.list.length <= 30);
  w.list.forEach(t => { assert.ok(t.x >= 0 && t.y >= 0 && t.x < 16 && t.y < 16); });
});

test('rank: far tiles and tiles of other zooms go first', () => {
  const v = { cx: 6400, cy: 6400, z: 15 };
  const near = ts.rank({ x: 100, y: 100, z: 15 }, v), far = ts.rank({ x: 110, y: 100, z: 15 }, v);
  assert.ok(far > near);
  const old = ts.rank({ x: 50, y: 50, z: 14 }, v);   // z14 tile (50,50) = world (6400, 6400) at z15: right under the view
  assert.ok(old > near);
});

test('tile compression round-trips and beats plain bytes on map-like pictures', () => {
  const rnd = rng(3);
  let packed = 0, raw = 0;
  for (let n = 0; n < 300; n++) {
    const T = 64, px = new Uint8Array(T * T);
    const kind = n % 4;
    for (let y = 0; y < T; y++) {
      for (let x = 0; x < T; x++) {
        let v;
        if (kind === 0) v = Math.floor(rnd() * 16);                                    // noise: worst case
        else if (kind === 1) v = 0;                                                     // blank
        else if (kind === 2) v = (x === 20 || y === 33 || (x + y) % 40 === 0) ? 5 : 0; // thin lines
        else v = rnd() < 0.9 ? (y > 30 ? 3 : 1) : 7;                                    // mostly flat with specks
        px[y * T + x] = v;
      }
    }
    const bytes = ts.encodeTile(px, T);
    assert.deepEqual(Array.from(ts.decodeTile(bytes, T)), Array.from(px));
    assert.ok(bytes.length <= T * (T / 2 + 1), 'noise costs one byte a row more than raw');
    if (kind >= 2) { packed += bytes.length; raw += T * T / 2; }
  }
  assert.ok(packed < raw / 2, 'map-like tiles shrink by more than half (' + packed + ' of ' + raw + ')');
});

test('runs longer than the row and all token sizes decode', () => {
  const T = 64;
  const px = new Uint8Array(T * T);
  for (let x = 0; x < T; x++) px[x] = x < 10 ? 4 : (x < 40 ? 9 : 2);   // 10, 30, 24 long runs
  for (let x = 0; x < T; x++) px[T + x] = px[x];                         // same as above, whole row
  for (let x = 0; x < T; x++) px[2 * T + x] = x % 2 ? 1 : 2;            // alternating: literal
  for (let y = 3; y < T; y++) px.copyWithin(y * T, 2 * T, 3 * T);
  const bytes = ts.encodeTile(px, T);
  assert.deepEqual(Array.from(ts.decodeTile(bytes, T)), Array.from(px));
  assert.equal(ts.decodeTile(bytes.slice(0, bytes.length - 1), T), null);
});

test('records are the watch\'s table entry followed by the bytes', () => {
  const out = [];
  ts.encodeRecord(out, { slot: 5, z: 17, x: 70000 * 64, y: 3 * 64, off: 12345, bytes: [9, 8, 7] });
  const b = Buffer.from(out);
  assert.equal(b.readInt32LE(0), 70000 * 64);
  assert.equal(b.readInt32LE(4), 192);
  assert.equal(b.readUInt16LE(8), 12345);
  assert.equal(b.readUInt16LE(10), 3);
  assert.deepEqual([b[12], b[13], b[14], b[15]], [17, 5, 0, 0]);
  assert.deepEqual(Array.from(b.subarray(16)), [9, 8, 7]);
  assert.equal(out.length, ts.HEADER + 3);
});
