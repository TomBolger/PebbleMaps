const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../src/pkjs');
const P = require(root + '/protocol');
const geo = require(root + '/geo');
const nav = require(root + '/nav');

function timers() {
  let id = 0;
  const pending = new Map();
  return {
    pending,
    setTimeout(fn, ms) { pending.set(++id, { fn, ms }); return id; },
    clearTimeout(id) { pending.delete(id); },
    setInterval() { return ++id; },
    clearInterval() {},
    run(ms) { const entries = [...pending.entries()].filter(([, t]) => t.ms === ms); for (const [id, t] of entries) { pending.delete(id); t.fn(); } }
  };
}

function phone() {
  const clock = timers(), messages = [], routes = [], autocomplete = [], details = [], fixes = [], watches = [], stopped = [], pois = [];
  const dev = { simLocation: [0, 0] };
  const S = { apiKey: 'dummy', defaultMode: 0, allModeTimes: true, liveLocation: false };
  const favs = [];
  function Clay() { this.registerComponent = () => {}; }
  const google = {
    hasKey: () => true,
    computeRoute: (...args) => routes.push({ mode: args[2], cb: args.at(-1) }),
    autocomplete: (...args) => autocomplete.push(args.at(-1)),
    placeDetails: (id, token, cb) => details.push({ id, token, cb }),
    searchPois: (loc, radius, cat, cb) => pois.push({ loc, radius, cat, cb }),
    searchOpen: (loc, radius, cat, cb) => cb(null, [])
  };
  const mocks = {
    './image': {}, './google': google, './settings': { get: () => S, findFavorite: () => -1, favorites: () => favs,
      favMode: (f, st) => (f.mode >= 0 && f.mode <= 3 ? f.mode : st.defaultMode) },
    './vendor/clay': Clay, './clay-components': [], './clay-config': { config: () => [] }, './dev': dev,
    'jpeg-js/lib/decoder': () => {}, './msg': { send: d => messages.push(d), drop() {}, pending: () => 0 },
    './tiles': { get() {}, styleKey: () => '', setIdleCheck() {}, cancelPrefetch() {} }, './voice': { onProblem() {}, cancel() {} }
  };
  const context = {
    module: { exports: {} }, require: n => mocks[n] || require(path.resolve(root, n)),
    Pebble: { addEventListener() {} }, console, Date, Math, Uint8Array,
    setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    setInterval: clock.setInterval, clearInterval: clock.clearInterval,
    navigator: { geolocation: {
      getCurrentPosition: (ok, err, opts) => fixes.push({ ok, err, opts }),
      watchPosition: (ok, err) => { watches.push({ ok, err }); return watches.length; },
      clearWatch: id => stopped.push(id)
    } }
  };
  vm.runInNewContext(fs.readFileSync(root + '/index.js', 'utf8'), context);
  context.dest = { lat: 0, lng: 0.01, name: 'Destination' };
  return { context, google, pois, clock, messages, routes, autocomplete, details, fixes, watches, stopped, dev, S, favs,
    send: p => context.module.exports._test.onMessage({ payload: p }) };
}

function apiRoute() {
  return { distanceMeters: 1112, duration: '100s', legs: [{ steps: [{
    polyline: { encodedPolyline: geo.encodePolyline([[0, 0], [0, 0.01]]) },
    distanceMeters: 1112, staticDuration: '100s'
  }] }] };
}
function start(p, mode = 0) { p.send({ cmd: P.CMD.NAV_START, seq: 2 + mode, mode, width: 144, height: 168 }); }

test('late route success after stop cannot start GPS or navigation', () => {
  const p = phone(); start(p); p.send({ cmd: P.CMD.NAV_STOP }); p.dev.simLocation = null;
  p.routes[0].cb(null, apiRoute());
  assert.equal(p.context.navigator_, null); assert.equal(p.context.navState, null); assert.equal(p.watches.length, 0);
});

test('late route error after stop does not show an error on the home screen', () => {
  const p = phone(); start(p); p.send({ cmd: P.CMD.NAV_STOP }); p.routes[0].cb({ text: 'Network failed' });
  assert.equal(p.messages.filter(m => m.cmd === P.CMD.ERROR).length, 0);
});

test('restarting navigation keeps only the new session and its GPS watch', () => {
  const p = phone(); start(p, 0); start(p, 1); p.dev.simLocation = null;
  p.routes[1].cb(null, apiRoute()); const current = p.context.navigator_;
  p.routes[0].cb(null, apiRoute());
  assert.equal(p.context.navigator_, current); assert.equal(current.route.mode, 1); assert.equal(p.watches.length, 1);
});

test('old arrival timer cannot stop a new navigation session', () => {
  const p = phone(); start(p); p.dev.simLocation = null; p.routes[0].cb(null, apiRoute());
  p.context.navigator_.onArrive(); start(p, 1); p.routes[1].cb(null, apiRoute());
  const current = p.context.navigator_; p.clock.run(1000); assert.equal(p.context.navigator_, current);
});

test('late reroute after stop cannot replace the cached route', () => {
  const p = phone(); start(p); p.dev.simLocation = null; p.routes[0].cb(null, apiRoute());
  const previous = p.context.route; p.context.navigator_.reroute([0.001, 0], () => { throw Error('stale callback'); });
  p.send({ cmd: P.CMD.NAV_STOP }); p.routes[1].cb(null, apiRoute()); assert.equal(p.context.route, previous);
});

test('out-of-order autocomplete response cannot change the selected visible place', () => {
  const p = phone(); p.send({ cmd: P.CMD.AUTOCOMPLETE, text: 'co', idx: 10 }); p.send({ cmd: P.CMD.AUTOCOMPLETE, text: 'coffee', idx: 11 });
  p.autocomplete[1](null, [{ name: 'Coffee', placeId: 'new' }]); p.autocomplete[0](null, [{ name: 'Old', placeId: 'old' }]);
  p.send({ cmd: P.CMD.SELECT, mode: P.SRC.SUGGEST, idx: 0, num: 11 }); assert.equal(p.details[0].id, 'new');
  assert.deepEqual(p.messages.filter(m => m.num === P.LIST.SUGGEST).map(m => m.idx), [11]);
});

test('selection identifies the displayed suggestion snapshot while the next list is in transit', () => {
  const p = phone(); p.send({ cmd: P.CMD.AUTOCOMPLETE, text: 'co', idx: 10 }); p.autocomplete[0](null, [{ name: 'Old', placeId: 'old' }]);
  p.send({ cmd: P.CMD.AUTOCOMPLETE, text: 'coffee', idx: 11 }); p.autocomplete[1](null, [{ name: 'Coffee', placeId: 'new' }]);
  p.send({ cmd: P.CMD.SELECT, mode: P.SRC.SUGGEST, idx: 0, num: 10 }); assert.equal(p.details[0].id, 'old');
});

test('a recent cached fix preserves its timestamp, rather than becoming new', () => {
  const p = phone(); p.dev.simLocation = null; const time = Date.now() - 10000; let location;
  p.context.getLocation(15000, (err, loc) => { assert.equal(err, null); location = loc; });
  p.fixes[0].ok({ timestamp: time, coords: { latitude: 1, longitude: 2, accuracy: 10 } });
  assert.equal(p.context.meTime, time); assert.equal(location[0], 1);
});

test('a fifteen-minute-old fix and stale fallback cannot start route planning', () => {
  const p = phone(); p.dev.simLocation = null; p.context.me = [1, 2]; p.context.meTime = Date.now() - 900000;
  let error; p.context.getLocation(15000, err => { error = err; });
  p.fixes[0].ok({ timestamp: Date.now() - 900000, coords: { latitude: 1, longitude: 2, accuracy: 10 } });
  assert.equal(error, undefined); p.clock.run(50000); assert.equal(error.code, P.ERR.NO_LOCATION);
});

test('completed location requests ignore late fixes from another retry', () => {
  const p = phone(); p.dev.simLocation = null;
  p.context.getLocation(15000, () => {}); p.clock.run(4000);
  p.fixes[1].ok({ timestamp: Date.now(), coords: { latitude: 1, longitude: 2, accuracy: 10 } });
  p.fixes[0].ok({ timestamp: Date.now(), coords: { latitude: 3, longitude: 4, accuracy: 10 } });
  assert.equal(p.context.me[0], 1);
});

test('poor or stale GPS cannot advance navigation, reroute or declare arrival', () => {
  let arrived = false, reroutes = 0; const messages = [];
  const navigator = new nav.Navigator({ route: nav.buildRoute(apiRoute(), 0, { lat: 0, lng: 0.01 }), settings: {},
    send: d => messages.push(d), reroute: () => reroutes++, onArrive: () => { arrived = true; } });
  navigator.update([0, 0.0099], 500); navigator.update([0, 0.0099], 5, Date.now() - 60000);
  for (let i = 0; i < 5; i++) navigator.update([1, 1], 500);
  assert.equal(arrived, false); assert.equal(navigator.lastIdx, 0); assert.equal(reroutes, 0);
  assert.ok(messages.every(m => m.flags === P.NAV_FLAG.NO_GPS));
  navigator.update([0, 0.0099], 5, Date.now()); assert.equal(arrived, true);
});

test('economical mode skips automatic POIs and four travel-mode requests', () => {
  const p = phone(); p.S.economical = true; assert.equal(p.context.placesShown(), 0);
  p.send({ cmd: P.CMD.MODE_TIMES }); assert.equal(p.routes.length, 0);
  assert.ok(p.messages.some(m => m.cmd === P.CMD.LIST && m.num === P.LIST.MODES));
});

test('the 21st favorite fails explicitly and existing favorites remain intact', () => {
  const store = { 'clay-settings': JSON.stringify({ favorites: JSON.stringify(Array.from({ length: 20 }, (_, i) => ({ name: 'Place ' + i, placeId: 'p' + i }))) }) };
  const c = { module: { exports: {} }, require: () => P, localStorage: { getItem: k => store[k], setItem: (k, v) => { store[k] = v; } } };
  vm.runInNewContext(fs.readFileSync(root + '/settings.js', 'utf8'), c); const settings = c.module.exports;
  assert.equal(settings.addFavorite({ name: 'extra', placeId: 'extra' }), false); assert.equal(settings.favorites().length, 20);
  settings.removeFavorite(0); assert.equal(settings.addFavorite({ name: 'extra', placeId: 'extra' }), true);
  assert.equal(settings.favorites().at(-1).placeId, 'extra');
});

test('a fix without a usable timestamp or with poor accuracy still shows where you are', () => {
  const p = phone(); p.dev.simLocation = null;
  let loc; p.context.getLocation(20000, (err, l) => { loc = l; });
  p.fixes[0].ok({ coords: { latitude: 47.6, longitude: -122.3, accuracy: 1500 } });
  assert.deepEqual(Array.from(loc), [47.6, -122.3]);
  const q = phone(); q.dev.simLocation = null;
  let loc2; q.context.getLocation(20000, (err, l) => { loc2 = l; });
  q.fixes[0].ok({ timestamp: Math.floor(Date.now() / 1000), coords: { latitude: 1, longitude: 2, accuracy: 20 } });
  assert.deepEqual(Array.from(loc2), [1, 2]);
});

test('map pieces are PackBits-packed rows that unpack exactly, within the message size', () => {
  const p = phone();
  function unpack(data, stride, rows) {
    const out = []; let s = 0;
    while (s < data.length) {
      const n = data[s++]; const run = n > 128; let cnt = run ? 257 - n : n + 1;
      if (run) { const v = data[s++]; while (cnt--) out.push(v); } else { while (cnt--) out.push(data[s++]); }
    }
    return out;
  }
  let seed = 7; const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  for (const flat of [0.0, 0.5, 0.95]) {
    const w = 200, h = 60, stride = 100, data = [];
    let v = 0;
    for (let i = 0; i < stride * h; i++) { if (rnd() > flat) v = Math.floor(rnd() * 256); data.push(v); }
    const img = { format: 1, stride, width: w, height: h, data };
    const chunks = p.context.chunksFor(img, [{ x0: 20, x1: 180, y0: 3, y1: 57 }]);
    const room = Math.max(64, p.context.watch.inbox - 160);
    for (const ch of chunks) {
      assert.ok(ch.data.length <= room);
      const rows = unpack(ch.data);
      assert.equal(rows.length % ch.stride, 0);
      for (let i = 0; i < rows.length; i++) {
        const y = ch.y + Math.floor(i / ch.stride), x = ch.xb + (i % ch.stride);
        assert.equal(rows[i], data[y * stride + x]);
      }
    }
    const total = chunks.reduce((a, c) => a + c.data.length / c.stride, 0);
    assert.ok(total > 0);
  }
});

test('spoken extras follow the chosen language; English keeps "In ..., ..."', () => {
  const mk = lang => new nav.Navigator({ route: nav.buildRoute(apiRoute(), 0, { lat: 0, lng: 0.01, name: 'Cafe' }), settings: { language: lang, imperial: false }, send() {} });
  const de = mk('de-DE'), en = mk('en-US');
  assert.equal(de.inDistance(300, 'Links abbiegen'), 'Links abbiegen');
  assert.ok(/^In .*, turn left$/.test(en.inDistance(300, 'Turn left')));
  assert.equal(de.words().w.reroute, 'Route wird neu berechnet');
  assert.equal(mk('ja').words().w.reroute, 'Rerouting');
  assert.equal(mk('pl-PL').words().w.reroute, 'Wyznaczanie nowej trasy');
});

test('zooming out keeps the most popular places, thinned so they do not crowd', () => {
  const p = phone(), c = p.context;
  c.poiOpen = false; c.poiMask = 1; c.poiMode = 2;
  const lat0 = 47.66, lng0 = -117.42;
  const view = z => ({ center: [lat0, lng0], zoom: z, heading: 0, w: 144, h: 168 });
  c.maybeFetchPois(view(16));
  assert.equal(p.pois.length, 1);
  const first = p.pois[0];
  // 20 places in popularity order, spread over the fetched circle
  const list = [];
  for (let i = 0; i < 20; i++) {
    const a = i * 2.4, r = first.radius * 0.1 * (1 + i % 5) * 0.9;
    list.push({ placeId: 'p' + i, name: 'Place ' + i, lat: lat0 + r * Math.cos(a) / 111320,
      lng: lng0 + r * Math.sin(a) / (111320 * Math.cos(lat0 * Math.PI / 180)), primaryType: 'restaurant' });
  }
  first.cb(null, list);
  const at16 = c.poiMarkers(view(16));
  assert.ok(at16.length > 0);
  // zoom out: no new data, still pins, best-ranked first, none crowding
  for (const z of [15, 14, 13]) {
    const ms = c.poiMarkers(view(z));
    assert.ok(ms.length > 0, 'pins at zoom ' + z);
    for (let i = 0; i < ms.length; i++) for (let j = i + 1; j < ms.length; j++) {
      assert.ok(Math.abs(ms[i].x - ms[j].x) >= 70 || Math.abs(ms[i].y - ms[j].y) >= 16, 'no overlap at zoom ' + z);
    }
    assert.ok(ms.length <= 16);
    const ranks = Array.from(c.pois).map(pl => pl.rank);
    assert.equal(ranks[0], 0, 'top-ranked place is kept');
    assert.deepEqual(ranks, ranks.slice().sort((a, b) => a - b));
  }
  assert.equal(c.poiMarkers(view(12)).length, 0);
  // a wider view is not covered by the small lookup: one new request, with a bigger radius; zoomed back in is covered
  c.maybeFetchPois(view(14));
  assert.equal(p.pois.length, 2); assert.ok(p.pois[1].radius > first.radius);
  p.pois[1].cb(null, []);
  c.maybeFetchPois(view(16)); c.maybeFetchPois(view(14));
  assert.equal(p.pois.length, 2);
});

test('overview: resent only when the view leaves its middle, changes zoom or turns', () => {
  const ov = require('../src/pkjs/overview');
  const c = [1000 * 4, 2000 * 4];
  const first = ov.next(null, 15, c, 0, 100, 114);
  assert.deepEqual(first, { zoom: 13, c: [1000, 2000], w: 100, h: 114 });
  const sent = { zoom: 13, c: [1000, 2000], heading: 0 };
  assert.equal(ov.next(sent, 15, [c[0] + 4 * 10, c[1]], 0, 100, 114), null);
  assert.notEqual(ov.next(sent, 15, [c[0] + 4 * 30, c[1]], 0, 100, 114), null);
  assert.notEqual(ov.next(sent, 16, [c[0] * 2, c[1] * 2], 0, 100, 114), null);
  assert.notEqual(ov.next(sent, 15, c, 90, 100, 114), null);
  assert.equal(ov.next(null, 15, c, 0, 0, 0), null);
  assert.equal(ov.next(null, 1, c, 0, 100, 114), null);
});

test('API navigate: "lat,lng" becomes the destination without a place card, and navigation routes there', () => {
  const p = phone();
  p.send({ cmd: P.CMD.SELECT, mode: P.SRC.API, text: ' 47.6205, -122.3493 ' });
  assert.equal(p.context.dest.lat, 47.6205); assert.equal(p.context.dest.lng, -122.3493);
  assert.equal(p.messages.filter(m => m.cmd === P.CMD.PLACE || m.cmd === P.CMD.ERROR).length, 0);
  start(p);
  p.fixes.forEach(f => f.ok({ coords: { latitude: 47.6, longitude: -122.3, accuracy: 10 }, timestamp: Date.now() }));
  assert.equal(p.routes.length, 1);
});

test('API navigate: an address is looked up like a typed favorite', () => {
  const p = phone();
  p.send({ cmd: P.CMD.SELECT, mode: P.SRC.API, text: '400 Broad St, Seattle' });
  assert.deepEqual({ ...p.context.dest }, { name: '400 Broad St, Seattle', address: '400 Broad St, Seattle' });
  p.send({ cmd: P.CMD.SELECT, mode: P.SRC.API, text: '91, 10' });   // not a coordinate
  assert.equal(p.context.dest.address, '91, 10');
});

test('API navigate: a favorite\'s name goes to that favorite, in its travel mode unless the caller gives one', () => {
  const p = phone();
  p.favs.push({ name: 'Work', address: '1 Main St', lat: 1, lng: 2, mode: 'default' },
              { name: ' Home ', address: '9 Elm St', lat: 3, lng: 4, mode: 1 });
  const go = (text, mode) => {
    p.routes.length = 0;
    p.send({ cmd: P.CMD.SELECT, mode: P.SRC.API, text });
    start(p, mode);
    p.fixes.forEach(f => f.ok({ coords: { latitude: 0, longitude: 0, accuracy: 10 }, timestamp: Date.now() }));
    p.fixes.length = 0;
    return p.routes[0] && p.routes[0].mode;
  };
  assert.equal(go('home', P.MODE_USE_DEFAULT), 1);   // the favorite's own mode
  assert.equal(p.context.dest.favIndex, 1); assert.equal(p.context.dest.lat, 3);
  assert.equal(go('  HOME', 2), 2);                  // the caller's mode wins
  assert.equal(p.context.dest.favIndex, 1);
  assert.equal(go('work', P.MODE_USE_DEFAULT), 0);   // favorite set to the app default
  assert.equal(p.context.dest.address, '1 Main St');
  p.google.geocode = () => {};
  go('Home Depot', P.MODE_USE_DEFAULT);              // whole names only
  assert.equal(p.context.dest.favIndex, undefined); assert.equal(p.context.dest.address, 'Home Depot');
});

test('API navigate without a destination reports an error', () => {
  const p = phone();
  p.send({ cmd: P.CMD.SELECT, mode: P.SRC.API });
  assert.equal(p.messages.filter(m => m.cmd === P.CMD.ERROR).length, 1);
});

test('API navigate to an address Google can\'t find ends navigation with an error', () => {
  const p = phone();
  p.google.geocode = (q, cb) => cb(null, null);
  p.google.searchText = (q, me, cb) => cb(null, []);
  p.send({ cmd: P.CMD.SELECT, mode: P.SRC.API, text: 'zzzz nowhere' });
  start(p);
  assert.equal(p.context.navState, null);
  const errors = p.messages.filter(m => m.cmd === P.CMD.ERROR);
  assert.equal(errors.length, 1);
  assert.equal(errors[0].text, 'Google couldn\'t find "zzzz nowhere".');   // not a favorite, so no "Settings"
});
