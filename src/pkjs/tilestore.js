// Pebble Maps - the watch's tile arena, planned on the phone
//
// Pebble Time 2 / Round 2 keep the home map as world-aligned square tiles (TILE x TILE pixels,
// 4-bit, compressed row by row exactly as sent). The watch has a small table (slot -> x, y, zoom, arena
// offset, length) and a block of memory, but no allocator: THIS file is the allocator. It
//   - decides which tiles the watch should hold (screen first, then rings outward, leaning the
//     way the view is moving),
//   - places each tile in the arena (first fit) and picks a table slot,
//   - evicts the tiles farthest from the view when something more important needs the room.
// Everything here is pure (no I/O) so it can be tested on its own.

var TILE = 64;
var HEADER = 16;   // bytes before each tile's data in a TILES message

function Store(arenaStart, arenaEnd, slots) {
  this.start = arenaStart;
  this.end = arenaEnd;
  this.nslots = slots;
  this.reset();
}

Store.prototype.reset = function () {
  this.tiles = {};
  this.used = [];                          // slot -> tile
  this.free = [{ off: this.start, len: this.end - this.start }];
  this.version = 0;
};

function key(x, y, z) { return z + '/' + x + '/' + y; }

Store.prototype.get = function (x, y, z) { return this.tiles[key(x, y, z)] || null; };
Store.prototype.has = function (x, y, z) { return !!this.tiles[key(x, y, z)]; };
Store.prototype.count = function () { return Object.keys(this.tiles).length; };
Store.prototype.bytes = function () {
  var n = 0;
  for (var k in this.tiles) n += this.tiles[k].len;
  return n;
};

function insertFree(list, off, len) {
  var i = 0;
  while (i < list.length && list[i].off < off) i++;
  list.splice(i, 0, { off: off, len: len });
  if (i + 1 < list.length && list[i].off + list[i].len === list[i + 1].off) {
    list[i].len += list[i + 1].len;
    list.splice(i + 1, 1);
  }
  if (i > 0 && list[i - 1].off + list[i - 1].len === list[i].off) {
    list[i - 1].len += list[i].len;
    list.splice(i, 1);
  }
}

function firstFit(list, len) {
  for (var i = 0; i < list.length; i++) if (list[i].len >= len) return list[i].off;
  return -1;
}

function takeFree(list, off, len) {
  for (var i = 0; i < list.length; i++) {
    var f = list[i];
    if (off >= f.off && off + len <= f.off + f.len) {
      var before = off - f.off, after = f.off + f.len - off - len;
      var repl = [];
      if (before > 0) repl.push({ off: f.off, len: before });
      if (after > 0) repl.push({ off: off + len, len: after });
      list.splice.apply(list, [i, 1].concat(repl));
      return;
    }
  }
}

Store.prototype.remove = function (t) {
  var k = key(t.x, t.y, t.z);
  var mine = this.tiles[k];
  if (!mine) return;
  delete this.tiles[k];
  this.used[mine.slot] = null;
  insertFree(this.free, mine.off, mine.len);
  this.version++;
};

// Find room for a tile of `len` bytes. keep(tile) says which stored tiles must stay; rank(tile) says how
// soon a tile should go (bigger = sooner). Returns { slot, off, victims } (victims already removed and
// to be forgotten by the caller), or null when it can't be done without losing something that must stay.
Store.prototype.place = function (t, len, keep, rank) {
  var self = this;
  if (len <= 0 || len > this.end - this.start) return null;
  var free = this.free.map(function (f) { return { off: f.off, len: f.len }; });
  var slotsLeft = this.nslots;
  for (var s = 0; s < this.nslots; s++) if (this.used[s]) slotsLeft--;
  var cands = Object.keys(this.tiles).map(function (k) { return self.tiles[k]; })
    .filter(function (c) { return !keep(c); });
  cands.sort(function (a, b) { return rank(b) - rank(a); });
  var victims = [], i = 0;
  while (firstFit(free, len) < 0 || slotsLeft < 1) {
    if (i >= cands.length) return null;
    var v = cands[i++];
    victims.push(v);
    insertFree(free, v.off, v.len);
    slotsLeft++;
  }
  victims.forEach(function (v) { self.remove(v); });
  var off = firstFit(this.free, len);
  takeFree(this.free, off, len);
  var slot = 0;
  while (this.used[slot]) slot++;
  var tile = { x: t.x, y: t.y, z: t.z, off: off, len: len, slot: slot };
  this.tiles[key(t.x, t.y, t.z)] = tile;
  this.used[slot] = tile;
  this.version++;
  return { slot: slot, off: off, victims: victims, tile: tile };
};

// --- What the watch should hold -------------------------------------------------------
// v = { cx, cy, w, h, z } view centre in world pixels at zoom z; o = { rings, max, dir: [dx, dy] unit vector or null }
// Returns tiles in the order they matter: the screen's tiles centre-out, then the rest by distance with a
// pull toward the direction of travel.
function want(v, o) {
  o = o || {};
  var rings = o.rings === undefined ? 3 : o.rings, max = o.max || 96, dir = o.dir || null;
  var lim = Math.pow(2, v.z) * 256 / TILE;   // tiles per side of the world
  var x0 = Math.floor((v.cx - v.w / 2) / TILE), x1 = Math.floor((v.cx + v.w / 2 - 1) / TILE);
  var y0 = Math.floor((v.cy - v.h / 2) / TILE), y1 = Math.floor((v.cy + v.h / 2 - 1) / TILE);
  var screen = [], rest = [];
  for (var ty = y0 - rings; ty <= y1 + rings; ty++) {
    if (ty < 0 || ty >= lim) continue;
    for (var tx = x0 - rings; tx <= x1 + rings; tx++) {
      if (tx < 0 || tx >= lim) continue;
      var ox = tx * TILE + TILE / 2 - v.cx, oy = ty * TILE + TILE / 2 - v.cy;
      var d = Math.sqrt(ox * ox + oy * oy);
      var t = { x: tx, y: ty, z: v.z, d: d };
      if (tx >= x0 && tx <= x1 && ty >= y0 && ty <= y1) {
        screen.push(t);
      } else {
        t.k = d - (dir ? 0.9 * (ox * dir[0] + oy * dir[1]) : 0);
        rest.push(t);
      }
    }
  }
  screen.sort(function (a, b) { return a.d - b.d; });
  rest.sort(function (a, b) { return a.k - b.k; });
  var out = screen.concat(rest);
  return { list: out.slice(0, max), screen: screen.length };
}

// How soon a stored tile should be dropped: far from the view (in pixels at the view's zoom), and
// tiles of other zooms go before tiles of this one
function rank(t, v) {
  var f = Math.pow(2, v.z - t.z);
  var ox = (t.x * TILE + TILE / 2) * f - v.cx, oy = (t.y * TILE + TILE / 2) * f - v.cy;
  var d = Math.max(Math.abs(ox), Math.abs(oy));
  var dz = Math.abs(v.z - t.z);
  return d + (dz === 0 ? 0 : (dz === 1 ? 400 : 4000));
}

// --- Wire format ------------------------------------------------------------------------
// TILES message data = records: the watch's table entry as it is (little-endian, 16 bytes), then the tile's bytes:
// x and y (i32: the tile's corner in world pixels at its zoom), arena offset (u16), length (u16), zoom, slot, 2 spare.
function encodeRecord(out, r) {
  var x = r.x | 0, y = r.y | 0, n = r.bytes.length;
  out.push(x & 0xff, (x >> 8) & 0xff, (x >> 16) & 0xff, (x >> 24) & 0xff,
           y & 0xff, (y >> 8) & 0xff, (y >> 16) & 0xff, (y >> 24) & 0xff,
           r.off & 0xff, r.off >> 8, n & 0xff, n >> 8, r.z, r.slot, 0, 0);
  for (var i = 0; i < n; i++) out.push(r.bytes[i]);
}

// --- Tile compression ---------------------------------------------------------------------
// A tile is TILE rows of TILE pixels (4-bit colours). Each row is coded on its own, from left to right, in tokens:
//   1rrrcccc            a run of colour c, rrr + 2 pixels (rrr = 7: 9 + the next byte)
//   01nnnnnn            n + 1 pixels the same as the row above (the first row has zeros above it)
//   00nnnnnn + bytes    n + 1 pixels as they are, two to a byte (high nibble first)
// Maps are flat fills with thin lines and text, so runs and "same as above" do most of the work (about 1.6x
// smaller than PackBits on whole bytes, 1.3x without "same as above"). The watch decodes rows in order.
function encodeTile(px, T) {
  var out = [], above = null;
  for (var y = 0; y < T; y++) {
    var row = px.subarray(y * T, (y + 1) * T), i = 0, lit = [], mine = [];
    var flush = function () {
      while (lit.length) {
        var m = Math.min(lit.length, 64);
        mine.push(m - 1);
        for (var k = 0; k < m; k += 2) mine.push((lit[k] << 4) | (k + 1 < m ? lit[k + 1] : 0));
        lit = lit.slice(m);
      }
    };
    while (i < T) {
      var j = i + 1;
      while (j < T && row[j] === row[i]) j++;
      var run = j - i, c = i;
      while (c < T && row[c] === (above ? above[c] : 0)) c++;
      var same = c - i;
      if (same >= 3 && same >= run) {
        flush();
        mine.push(0x40 | (same - 1));
        i = c;
      } else if (run >= 2) {
        flush();
        if (run <= 8) mine.push(0x80 | ((run - 2) << 4) | row[i]);
        else mine.push(0xf0 | row[i], run - 9);
        i = j;
      } else {
        lit.push(row[i]);
        i++;
      }
    }
    flush();
    if (mine.length > T / 2 + 1) {   // busy row (noise, photos): plain is shorter
      mine = [T - 1];
      for (var q = 0; q < T; q += 2) mine.push((row[q] << 4) | row[q + 1]);
    }
    for (var w = 0; w < mine.length; w++) out.push(mine[w]);
    above = row;
  }
  return out;
}

// The inverse, as the watch does it (for tests): T x T pixels, or null if the data is broken
function decodeTile(bytes, T) {
  var out = new Uint8Array(T * T), p = 0;
  for (var y = 0; y < T; y++) {
    var n = 0;
    while (n < T) {
      if (p >= bytes.length) return null;
      var c = bytes[p++], cnt = (c & 0x3f) + 1, v = c & 15, b = 0;
      if (c & 0x80) { cnt = ((c >> 4) & 7) + 2; if (cnt === 9) cnt += bytes[p++]; }
      for (var k = 0; k < cnt && n < T; k++, n++) {
        if (c < 0x40 && !(k & 1)) b = bytes[p++];
        out[y * T + n] = (c & 0x80) ? v : ((c & 0x40) ? (y ? out[(y - 1) * T + n] : 0) : ((k & 1) ? b & 15 : b >> 4));
      }
    }
  }
  return p === bytes.length ? out : null;
}

// 4-bit packed rows (as image.pack makes them: high nibble first) -> one byte per pixel
function nibbles(data, stride, T) {
  var out = new Uint8Array(T * T);
  for (var y = 0; y < T; y++) {
    for (var x = 0; x < T; x++) {
      var b = data[y * stride + (x >> 1)];
      out[y * T + x] = (x & 1) ? b & 15 : b >> 4;
    }
  }
  return out;
}

module.exports = { TILE: TILE, HEADER: HEADER, Store: Store, want: want, rank: rank, encodeRecord: encodeRecord,
  encodeTile: encodeTile, decodeTile: decodeTile, nibbles: nibbles };
