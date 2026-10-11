// Pebble Maps - big map pictures kept on the phone, cut (and rotated) for the watch
//
// Google's Static Maps returns up to 640x640 pixels. We fetch that size once,
// keep it on the phone, and cut each watch screen out of it. Panning within
// the picture costs nothing; we also prefetch the next picture and the
// zoomed-in / zoomed-out versions in the background.

var geo = require('./geo');
var image = require('./image');
var google = require('./google');
var dev = require('./dev');

var SIZE = 640;
var MAX_TILES = 6;
var tiles = [];            // most recent first
var inflight = {};         // tileId -> [callbacks]
var prefetchQueue = [];
var prefetching = false;

// What makes two pictures interchangeable (apart from position)
function styleKey(o) {
  var path = o.path && o.path.length > 1 ? geo.encodePolyline(geo.thin(o.path, 400)) : '';
  return [o.zoom, o.style, o.format, path, (o.markers || []).join(';')].join('|');
}

// Does the tile cover a view of w x h (rotated by heading) centered at world point c?
function covers(t, c, w, h, rotated) {
  var hx, hy;
  if (rotated) {
    hx = hy = Math.sqrt(w * w + h * h) / 2;
  } else {
    hx = w / 2;
    hy = h / 2;
  }
  return c[0] - hx >= t.x0 - 1 && c[0] + hx <= t.x0 + t.w + 1 &&
         c[1] - hy >= t.y0 - 1 && c[1] + hy <= t.y0 + t.h + 1;
}

function findTile(key, c, w, h, rotated) {
  for (var i = 0; i < tiles.length; i++) {
    var t = tiles[i];
    if (t.key === key && covers(t, c, w, h, rotated)) {
      if (i > 0) { tiles.splice(i, 1); tiles.unshift(t); }
      return t;
    }
  }
  return null;
}

// Fetch a tile centered on world point c. cb(err, tile)
function fetchTile(o, key, c, cb) {
  if (dev.snapTiles) {
    // screenshot test mode: only ask for pictures on a fixed grid (prepared in advance)
    var sn = dev.snapTiles;
    c = [Math.round(c[0] / sn) * sn, Math.round(c[1] / sn) * sn];
  }
  var id = key + '@' + Math.round(c[0]) + ',' + Math.round(c[1]);
  if (inflight[id]) { inflight[id].push(cb); return; }
  inflight[id] = [cb];
  var center = geo.unproject(c[0], c[1], o.zoom);
  google.staticMap({
    center: center, zoom: o.zoom, w: SIZE, h: SIZE, style: o.style, path: o.path, markers: o.markers
  }, function (err, bytes) {
    var cbs = inflight[id];
    delete inflight[id];
    var tile = null;
    if (!err) {
      try {
        var b = image.toBase(image.decodePNG(bytes), o.format);
        // the picture is centered on the rounded center we asked for
        var pc = geo.project(center[0], center[1], o.zoom);
        tile = { key: key, zoom: o.zoom, format: o.format, style: o.style, w: b.width, h: b.height, px: b.px,
                 x0: pc[0] - b.width / 2, y0: pc[1] - b.height / 2 };
        tiles.unshift(tile);
        if (tiles.length > MAX_TILES) tiles.pop();
      } catch (e) {
        err = { code: 3, title: 'Map problem', text: 'Couldn\'t read the map picture from Google (' + e.message + ').' };
      }
    }
    cbs.forEach(function (fn) { fn(err, tile); });
  });
}

// Cut a w x h picture centered on world point c, rotated so that `heading`
// (degrees clockwise from north) points up. Returns packed watch image.
function render(t, c, w, h, heading, format) {
  var out = new Uint8Array(w * h);
  var bg = image.background(format, t.style);
  var a = geo.rad(heading || 0);
  var cs = Math.cos(a), sn = Math.sin(a);
  // screen (dx, dy) -> world (dx*cs - dy*sn, dx*sn + dy*cs)
  var tw = t.w, th = t.h, px = t.px;
  var ox = c[0] - t.x0, oy = c[1] - t.y0;
  for (var y = 0; y < h; y++) {
    var dy = y - h / 2 + 0.5;
    var dx0 = -w / 2 + 0.5;
    var sx = ox + dx0 * cs - dy * sn;
    var sy = oy + dx0 * sn + dy * cs;
    var row = y * w;
    for (var x = 0; x < w; x++) {
      var ix = sx | 0, iy = sy | 0;
      out[row + x] = (sx >= 0 && sy >= 0 && ix < tw && iy < th) ? px[iy * tw + ix] : bg;
      sx += cs;
      sy += sn;
    }
  }
  return image.pack(out, w, h, format, t.style);
}

function centerOf(o) {
  return o.c || geo.project(o.center[0], o.center[1], o.zoom);
}

// Get a watch picture. o = { center:[lat,lng] or c:[world x,y], zoom, w, h, heading, style, format, path, markers }
// cb(err, packedImage)
function get(o, cb) {
  var key = styleKey(o);
  var c = centerOf(o);
  var rotated = !!o.heading;
  var t = findTile(key, c, o.w, o.h, rotated);
  if (t) return cb(null, render(t, c, o.w, o.h, o.heading, o.format));
  // o.ahead: [dx, dy] world pixels to center a new picture further along (navigation)
  var fc = c;
  if (o.ahead && !dev.snapTiles) {
    var room = SIZE / 2 - (rotated ? Math.sqrt(o.w * o.w + o.h * o.h) : Math.max(o.w, o.h)) / 2 - 4;
    var len = Math.sqrt(o.ahead[0] * o.ahead[0] + o.ahead[1] * o.ahead[1]) || 1;
    var k = Math.max(0, room * 0.75) / len;
    fc = [c[0] + o.ahead[0] * k, c[1] + o.ahead[1] * k];
  }
  fetchTile(o, key, fc, function (err, tile) {
    if (err) return cb(err);
    if (!covers(tile, c, o.w, o.h, rotated)) {
      // view bigger than one Google picture (very large screens): show what we have
    }
    cb(null, render(tile, c, o.w, o.h, o.heading, o.format));
  });
}

// --- Background prefetch -----------------------------------------------------
// Prefetching waits while the watch is still receiving (decoding a picture blocks the phone's
// only thread, which would stall the Bluetooth transfer)
var isIdle = function () { return true; };
var idleTimer = null;

function pumpPrefetch() {
  if (prefetching || !prefetchQueue.length) return;
  if (!isIdle()) {
    if (!idleTimer) idleTimer = setTimeout(function () { idleTimer = null; pumpPrefetch(); }, 500);
    return;
  }
  var job = prefetchQueue.shift();
  if (findTile(job.key, job.c, job.w, job.h, job.rotated)) return pumpPrefetch();
  prefetching = true;
  fetchTile(job.o, job.key, job.c, function () {
    prefetching = false;
    pumpPrefetch();
  });
}

// w, h: the area the new picture must cover (bigger than the view = "get the next one ready")
function queue(o, zoom, c, w, h) {
  var oo = {};
  Object.keys(o).forEach(function (k) { oo[k] = o[k]; });
  oo.zoom = zoom;
  oo.c = c;
  var key = styleKey(oo);
  prefetchQueue.push({ o: oo, key: key, c: c, w: w || o.w, h: h || o.h, rotated: !!o.heading });
}

// After showing view o: get the next picture ready in the direction the user is
// near the edge, plus the zoomed-in and zoomed-out pictures.
function prefetchAround(o, withZoom) {
  prefetchQueue = [];
  var key = styleKey(o);
  var c = centerOf(o);
  var t = findTile(key, c, o.w, o.h, !!o.heading);
  if (t) {
    // if one more pan step (a third of the view) would leave this picture, prefetch around the view
    var step = Math.max(o.w, o.h) * 0.6;
    if (!covers(t, c, o.w + step, o.h + step, !!o.heading)) {
      // the next picture: centered on the view, or further along when moving (o.ahead)
      var nc = c;
      if (o.ahead && !dev.snapTiles) {
        var len = Math.sqrt(o.ahead[0] * o.ahead[0] + o.ahead[1] * o.ahead[1]) || 1;
        var k = (SIZE / 2 - Math.max(o.w, o.h)) * 0.6 / len;
        if (k > 0) nc = [c[0] + o.ahead[0] * k, c[1] + o.ahead[1] * k];
      }
      queue(o, o.zoom, nc, o.w + step, o.h + step);
    }
  }
  if (withZoom) {
    // zooming out first: its picture also fills the edges when the watch shrinks the view
    if (o.zoom > 3) queue(o, o.zoom - 1, [c[0] / 2, c[1] / 2]);
    if (o.zoom < 20) queue(o, o.zoom + 1, [c[0] * 2, c[1] * 2]);
  }
  pumpPrefetch();
}

function clear() {
  tiles = [];
  prefetchQueue = [];
}

function cancelPrefetch() { prefetchQueue = []; }

module.exports = { get: get, styleKey: styleKey, prefetchAround: prefetchAround, clear: clear, SIZE: SIZE,
  cancelPrefetch: cancelPrefetch, setIdleCheck: function (fn) { isIdle = fn; } };
