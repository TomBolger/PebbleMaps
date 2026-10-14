// Pebble Maps - Google Maps Platform calls (all with the user's own API key)
//
// APIs used (the user enables these in Google Cloud):
//   Maps Static API      - map pictures
//   Places API (New)     - search and nearby places
//   Routes API           - directions and travel times
//   Geocoding API        - addresses <-> coordinates

var P = require('./protocol');

var config = {
  key: '',
  base: null,        // test override: 'http://127.0.0.1:8765'
  language: 'en',
  voice: 'en-US'
};

function setKey(key) {
  key = String(key || '').trim();
  if (key.indexOf('test:') === 0) {
    config.base = key.substr(5);
    config.key = 'TEST';
  } else {
    config.base = null;
    config.key = key;
  }
}

// "de-DE", "pt-BR", "fr" ... -> the code Google's map and place services use ("de", "pt-BR", "fr"),
// plus a region for the voice ("de-DE")
var VOICE_REGION = { en: 'en-US', es: 'es-ES', fr: 'fr-FR', de: 'de-DE', it: 'it-IT', pt: 'pt-BR', nl: 'nl-NL',
  sv: 'sv-SE', da: 'da-DK', nb: 'nb-NO', no: 'nb-NO', fi: 'fi-FI', pl: 'pl-PL', cs: 'cs-CZ', tr: 'tr-TR' };
function setLanguage(lang) {
  lang = String(lang || 'en').replace('_', '-');
  var base = lang.split('-')[0].toLowerCase();
  var region = lang.split('-')[1];
  config.language = (base === 'pt' || base === 'zh') && region ? base + '-' + region.toUpperCase() : base;
  config.voice = region ? base + '-' + region.toUpperCase() : (VOICE_REGION[base] || base);
  if (base === 'no') config.language = 'nb';
}
function language() { return config.language; }

function hasKey() { return !!config.key; }
function isTest() { return !!config.base; }

function url(service, path) {
  if (config.base) return config.base + '/' + service + path;
  var hosts = {
    places: 'https://places.googleapis.com',
    routes: 'https://routes.googleapis.com',
    tts: 'https://texttospeech.googleapis.com',
    maps: 'https://maps.googleapis.com'
  };
  return hosts[service] + path;
}

// Turn a Google error into plain language a non-technical user can act on
function friendlyError(status, body, apiName) {
  var msg = '';
  var reason = '';
  try {
    var j = typeof body === 'string' ? JSON.parse(body) : body;
    if (j && j.error) {
      msg = j.error.message || '';
      reason = j.error.status || '';
      (j.error.details || []).forEach(function (d) { if (d.reason) reason += ' ' + d.reason; });
    } else if (j && j.error_message) {
      msg = j.error_message;
      reason = j.status || '';
    }
  } catch (e) {
    msg = String(body || '').substr(0, 300);
  }
  var all = (msg + ' ' + reason).toLowerCase();
  if (status === 0) {
    return { code: P.ERR.NETWORK, title: 'No connection', text: 'Your phone couldn\'t reach Google. Check its internet connection and try again.' };
  }
  if (all.indexOf('api key not valid') >= 0 || all.indexOf('api_key_invalid') >= 0 || all.indexOf('provided api key is invalid') >= 0) {
    return { code: P.ERR.NO_KEY, title: 'Key not valid', text: 'Google says your Maps key isn\'t valid. Open Pebble Maps Settings on your phone and paste the key again.' };
  }
  if (all.indexOf('billing') >= 0) {
    return { code: P.ERR.API, title: 'Billing needed', text: 'Google needs billing turned on for your project. Usage beyond the free allowances can incur charges. See step 3 in Settings on your phone.' };
  }
  if (all.indexOf('api_key_service_blocked') >= 0 || all.indexOf('are blocked') >= 0) {
    return { code: P.ERR.API, title: 'Key is limited', blocked: true, text: 'Your key is set to only work with some services. In Google Cloud, open your key (Settings step 5) and tick ' + apiName + ' under API restrictions.' };
  }
  if (all.indexOf('not been used') >= 0 || all.indexOf('is disabled') >= 0 || all.indexOf('service_disabled') >= 0 ||
      all.indexOf('not authorized to use this api') >= 0 ||
      all.indexOf('not activated') >= 0 || all.indexOf('this api project is not authorized') >= 0) {
    return { code: P.ERR.API, title: apiName + ' is off', text: 'Your key can\'t use the ' + apiName + ' yet. In Settings on your phone, do step 4 again (turn on the map services) and check step 5.' };
  }
  if (all.indexOf('referer') >= 0 || all.indexOf('referrer') >= 0 || all.indexOf('api_key_android_app_blocked') >= 0 ||
      all.indexOf('api_key_ios_app_blocked') >= 0 || all.indexOf('ip address') >= 0) {
    return { code: P.ERR.API, title: 'Key is restricted', text: 'Your key only works on certain websites or apps. In Google Cloud, set "Application restrictions" to None (Settings step 5).' };
  }
  if (status === 429 || all.indexOf('quota') >= 0 || all.indexOf('rate') >= 0) {
    return { code: P.ERR.API, title: 'Too many requests', text: 'Google\'s daily limit for your key was reached. Try again later.' };
  }
  return { code: P.ERR.API, title: 'Google error', text: (apiName ? apiName + ': ' : '') + (msg || ('error ' + status)).substr(0, 180) };
}

function request(opts, cb) {
  var xhr = new XMLHttpRequest();
  var done = false;
  var timer = setTimeout(function () {
    if (done) return;
    done = true;
    try { xhr.abort(); } catch (e) { /* ignore */ }
    cb({ status: 0, body: 'timeout' });
  }, opts.timeout || 20000);
  xhr.open(opts.method || 'GET', opts.url, true);
  var headers = opts.headers || {};
  Object.keys(headers).forEach(function (h) { xhr.setRequestHeader(h, headers[h]); });
  if (opts.binary) {
    if (opts.binary === 'text') xhr.overrideMimeType('text/plain; charset=x-user-defined');
    else xhr.responseType = 'arraybuffer';
  }
  xhr.onload = function () {
    if (done) return;
    done = true;
    clearTimeout(timer);
    var body;
    if (opts.binary === 'arraybuffer' || (opts.binary && opts.binary !== 'text')) body = xhr.response;
    else body = xhr.responseText;
    if (xhr.status >= 200 && xhr.status < 300) cb(null, body, xhr);
    else {
      var text = body;
      if (opts.binary && typeof body !== 'string') {
        try { text = String.fromCharCode.apply(null, new Uint8Array(body).subarray(0, 600)); } catch (e) { text = ''; }
      }
      cb({ status: xhr.status, body: text });
    }
  };
  xhr.onerror = function () {
    if (done) return;
    done = true;
    clearTimeout(timer);
    cb({ status: 0, body: 'network error' });
  };
  xhr.send(opts.body || null);
}

function postJSON(service, path, fieldMask, body, apiName, cb) {
  request({
    method: 'POST',
    url: url(service, path),
    headers: {
      'Content-Type': 'application/json',
      'X-Goog-Api-Key': config.key,
      'X-Goog-FieldMask': fieldMask
    },
    body: JSON.stringify(body)
  }, function (err, text) {
    if (err) return cb(friendlyError(err.status, err.body, apiName));
    var j;
    try { j = JSON.parse(text); } catch (e) { return cb(friendlyError(500, 'Bad response from Google', apiName)); }
    cb(null, j);
  });
}

function getJSON(service, path, apiName, cb) {
  request({ url: url(service, path) }, function (err, text) {
    if (err) return cb(friendlyError(err.status, err.body, apiName));
    var j;
    try { j = JSON.parse(text); } catch (e) { return cb(friendlyError(500, 'Bad response', apiName)); }
    if (j.status && j.status !== 'OK' && j.status !== 'ZERO_RESULTS') {
      return cb(friendlyError(403, j, apiName));
    }
    cb(null, j);
  });
}

// Binary GET with a fallback for phone apps without arraybuffer support
function getBinary(u, cb) {
  request({ url: u, binary: 'arraybuffer', timeout: 25000 }, function (err, body) {
    if (!err && body && typeof body !== 'string' && body.byteLength !== undefined) {
      return cb(null, new Uint8Array(body));
    }
    if (err && err.status !== 200) return cb(err);
    request({ url: u, binary: 'text', timeout: 25000 }, function (err2, text) {
      if (err2) return cb(err2);
      var out = new Uint8Array(text.length);
      for (var i = 0; i < text.length; i++) out[i] = text.charCodeAt(i) & 0xff;
      cb(null, out);
    });
  });
}

// --- Places API (New) -----------------------------------------------------
var PLACE_FIELDS = 'places.id,places.displayName,places.formattedAddress,places.shortFormattedAddress,' +
  'places.location,places.primaryTypeDisplayName';

function placeFrom(p) {
  return {
    name: (p.displayName && p.displayName.text) || 'Unnamed place',
    address: p.shortFormattedAddress || p.formattedAddress || '',
    fullAddress: p.formattedAddress || '',
    lat: p.location && p.location.latitude,
    lng: p.location && p.location.longitude,
    placeId: p.id,
    type: (p.primaryTypeDisplayName && p.primaryTypeDisplayName.text) || ''
  };
}

// "Hide closed places": ask for the open-now flag and drop only places Google says are closed
// right now; places without opening hours (parks, addresses, landmarks) stay. Asking for hours
// puts the lookup in Google's Enterprise tier (1,000 free a month), so only searches and the
// Places lists do this; the map dots use cheaper lookups (see index.js).
var OPEN_FIELD = ',places.currentOpeningHours.openNow';

function notClosed(raw) {
  return !(raw.currentOpeningHours && raw.currentOpeningHours.openNow === false);
}

function searchText(query, loc, cb, hideClosed) {
  var n = hideClosed ? 20 : 10;   // room for closed ones we drop
  var body = { textQuery: query, pageSize: n, languageCode: config.language };
  if (loc) {
    body.locationBias = { circle: { center: { latitude: loc[0], longitude: loc[1] }, radius: 40000 } };
  }
  var mask = PLACE_FIELDS + (hideClosed ? OPEN_FIELD : '');
  function done(err, j) {
    if (err) return cb(err);
    cb(null, (j.places || []).filter(hideClosed ? notClosed : function () { return true; })
      .map(placeFrom).filter(function (p) { return p.lat !== undefined; }).slice(0, 10));
  }
  postJSON('places', '/v1/places:searchText', mask, body, 'Places API', function (err, j) {
    // Older/newer API versions name the result limit differently; retry once without it.
    if (err && /pageSize|unknown name|invalid json payload/i.test(err.text || '')) {
      delete body.pageSize;
      body.maxResultCount = n;
      return postJSON('places', '/v1/places:searchText', mask, body, 'Places API', done);
    }
    done(err, j);
  });
}

function searchNearby(category, loc, cb, hideClosed) {
  var body = {
    includedTypes: category.types,
    maxResultCount: hideClosed ? 20 : 10,   // room for closed ones we drop
    rankPreference: category.rank,
    languageCode: config.language,
    locationRestriction: { circle: { center: { latitude: loc[0], longitude: loc[1] }, radius: category.radius } }
  };
  postJSON('places', '/v1/places:searchNearby', PLACE_FIELDS + (hideClosed ? OPEN_FIELD : ''), body, 'Places API', function (err, j) {
    if (err) return cb(err);
    cb(null, (j.places || []).filter(hideClosed ? notClosed : function () { return true; })
      .map(placeFrom).filter(function (p) { return p.lat !== undefined; }).slice(0, 10));
  });
}

// Suggestions while typing (Places Autocomplete), nearest first.
// Uses a session token so the typing plus the final pick are billed as one lookup.
function autocomplete(input, loc, token, cb) {
  var body = { input: input, languageCode: config.language, includeQueryPredictions: false };
  if (token) body.sessionToken = token;
  if (loc) {
    body.locationBias = { circle: { center: { latitude: loc[0], longitude: loc[1] }, radius: 30000 } };
    body.origin = { latitude: loc[0], longitude: loc[1] };
  }
  var mask = 'suggestions.placePrediction.placeId,suggestions.placePrediction.structuredFormat,' +
             'suggestions.placePrediction.text,suggestions.placePrediction.distanceMeters';
  postJSON('places', '/v1/places:autocomplete', mask, body, 'Places API', function (err, j) {
    if (err) return cb(err);
    var out = [];
    (j.suggestions || []).forEach(function (sg) {
      var p = sg.placePrediction;
      if (!p || !p.placeId) return;
      var sf = p.structuredFormat || {};
      out.push({
        placeId: p.placeId,
        name: (sf.mainText && sf.mainText.text) || (p.text && p.text.text) || '',
        address: (sf.secondaryText && sf.secondaryText.text) || '',
        distance: p.distanceMeters
      });
    });
    cb(null, out);
  });
}

// Full details (location, address) for one place
function placeDetails(placeId, token, cb) {
  var path = '/v1/places/' + encodeURIComponent(placeId) + (token ? '?sessionToken=' + encodeURIComponent(token) : '');
  request({
    url: url('places', path),
    headers: {
      'X-Goog-Api-Key': config.key,
      'X-Goog-FieldMask': 'id,displayName,formattedAddress,shortFormattedAddress,location,primaryTypeDisplayName'
    }
  }, function (err, text) {
    if (err) return cb(friendlyError(err.status, err.body, 'Places API'));
    var j;
    try { j = JSON.parse(text); } catch (e) { return cb(friendlyError(500, 'Bad response', 'Places API')); }
    cb(null, placeFrom(j));
  });
}

// --- Places shown on the map, and the "more info" page ---------------------------
// Places of one kind around a spot (for the map). cb(err, [place + primaryType])
function searchPois(loc, radius, cat, cb) {
  var body = {
    includedTypes: cat.types,
    maxResultCount: 20,
    rankPreference: cat.rank || 'POPULARITY',
    languageCode: config.language,
    locationRestriction: { circle: { center: { latitude: loc[0], longitude: loc[1] }, radius: radius } }
  };
  postJSON('places', '/v1/places:searchNearby', PLACE_FIELDS + ',places.primaryType', body, 'Places API', function (err, j) {
    if (err) return cb(err);
    cb(null, (j.places || []).map(function (p) {
      var o = placeFrom(p);
      o.primaryType = p.primaryType || '';
      return o;
    }).filter(function (p) { return p.lat !== undefined; }));
  });
}

// Places of one kind that are open right now (Text Search does the "open now" filtering,
// so we don't pay for opening-hours fields). cb(err, [place + primaryType])
function searchOpen(loc, radius, cat, cb) {
  var dLat = radius / 111320, dLng = radius / (111320 * Math.cos(loc[0] * Math.PI / 180));
  var body = {
    textQuery: cat.name, includedType: cat.types[0], openNow: true, pageSize: 20, languageCode: config.language,
    rankPreference: cat.rank === 'DISTANCE' ? 'DISTANCE' : 'RELEVANCE',
    locationRestriction: { rectangle: {
      low: { latitude: loc[0] - dLat, longitude: loc[1] - dLng },
      high: { latitude: loc[0] + dLat, longitude: loc[1] + dLng } } }
  };
  postJSON('places', '/v1/places:searchText', PLACE_FIELDS + ',places.primaryType', body, 'Places API', function (err, j) {
    if (err) return cb(err);
    cb(null, (j.places || []).map(function (p) {
      var o = placeFrom(p);
      o.primaryType = p.primaryType || '';
      return o;
    }).filter(function (p) { return p.lat !== undefined; }));
  });
}

var PRICE = { PRICE_LEVEL_FREE: 'Free', PRICE_LEVEL_INEXPENSIVE: '$', PRICE_LEVEL_MODERATE: '$$',
  PRICE_LEVEL_EXPENSIVE: '$$$', PRICE_LEVEL_VERY_EXPENSIVE: '$$$$' };
var INFO_FIELDS = 'id,displayName,formattedAddress,shortFormattedAddress,location,primaryType,primaryTypeDisplayName,' +
  'rating,userRatingCount,priceLevel,currentOpeningHours,regularOpeningHours,nationalPhoneNumber,websiteUri,' +
  'editorialSummary,reviews,photos,businessStatus';

function txt(o) { return (o && (o.text || '')) || ''; }

// Everything Google knows about a place: ratings, hours, reviews, photos
function placeInfo(placeId, cb) {
  request({
    url: url('places', '/v1/places/' + encodeURIComponent(placeId) + '?languageCode=' + config.language),
    headers: { 'X-Goog-Api-Key': config.key, 'X-Goog-FieldMask': INFO_FIELDS }
  }, function (err, text) {
    if (err) return cb(friendlyError(err.status, err.body, 'Places API'));
    var j;
    try { j = JSON.parse(text); } catch (e) { return cb(friendlyError(500, 'Bad response', 'Places API')); }
    var info = placeFrom(j);
    var cur = j.currentOpeningHours || {};
    var reg = j.regularOpeningHours || {};
    info.primaryType = j.primaryType || '';
    info.rating = j.rating || 0;
    info.ratingCount = j.userRatingCount || 0;
    info.price = PRICE[j.priceLevel] || '';
    info.openNow = cur.openNow === undefined ? (reg.openNow === undefined ? null : reg.openNow) : cur.openNow;
    info.nextOpen = cur.nextOpenTime || reg.nextOpenTime || '';
    info.nextClose = cur.nextCloseTime || reg.nextCloseTime || '';
    info.hours = cur.weekdayDescriptions || reg.weekdayDescriptions || [];
    info.always = !!((cur.periods || reg.periods || []).length === 1 && !(cur.periods || reg.periods)[0].close);
    info.phone = j.nationalPhoneNumber || '';
    info.website = j.websiteUri || '';
    info.summary = txt(j.editorialSummary);
    info.status = j.businessStatus || '';
    info.reviews = (j.reviews || []).map(function (r) {
      return {
        author: (r.authorAttribution && r.authorAttribution.displayName) || 'Someone',
        rating: r.rating || 0,
        when: r.relativePublishTimeDescription || '',
        text: txt(r.text) || txt(r.originalText)
      };
    });
    info.photos = (j.photos || []).slice(0, 10).map(function (ph) {
      return { name: ph.name, w: ph.widthPx || 0, h: ph.heightPx || 0 };
    });
    cb(null, info);
  });
}

// A place photo (JPEG bytes), sized to fit within maxW x maxH
function placePhoto(name, maxW, maxH, cb) {
  var path = '/v1/' + name + '/media?skipHttpRedirect=true&maxWidthPx=' + Math.round(maxW) + '&maxHeightPx=' + Math.round(maxH);
  request({ url: url('places', path), headers: { 'X-Goog-Api-Key': config.key } }, function (err, text) {
    if (err) return cb(friendlyError(err.status, err.body, 'Places API'));
    var j;
    try { j = JSON.parse(text); } catch (e) { return cb(friendlyError(500, 'Bad response', 'Places API')); }
    if (!j.photoUri) return cb({ code: P.ERR.API, title: 'No photo', text: 'Google sent no photo.' });
    getBinary(j.photoUri, function (e2, bytes) {
      if (e2) return cb(friendlyError(e2.status, e2.body, 'Places API'));
      cb(null, bytes);
    });
  });
}

// --- Cloud Text-to-Speech (spoken directions) ----------------------------------
// cb(err, base64 WAV at 8 kHz)
function tts(text, cb) {
  var lang = config.voice || 'en-US';
  request({
    method: 'POST',
    url: url('tts', '/v1/text:synthesize'),
    headers: { 'Content-Type': 'application/json', 'X-Goog-Api-Key': config.key },
    body: JSON.stringify({
      input: { text: text },
      voice: { languageCode: lang, ssmlGender: 'FEMALE' },
      audioConfig: { audioEncoding: 'LINEAR16', sampleRateHertz: 8000, speakingRate: 1.1 }
    })
  }, function (err, body) {
    if (err) return cb(friendlyError(err.status, err.body, 'Text-to-Speech API'));
    var j;
    try { j = JSON.parse(body); } catch (e) { return cb(friendlyError(500, 'Bad response', 'Text-to-Speech API')); }
    if (!j.audioContent) return cb(friendlyError(500, 'No audio', 'Text-to-Speech API'));
    cb(null, j.audioContent);
  });
}

// --- Geocoding API --------------------------------------------------------
function geocode(address, cb) {
  getJSON('maps', '/maps/api/geocode/json?address=' + encodeURIComponent(address) +
    '&key=' + encodeURIComponent(config.key), 'Geocoding API', function (err, j) {
    if (err) return cb(err);
    var r = (j.results || [])[0];
    if (!r) return cb(null, null);
    cb(null, {
      lat: r.geometry.location.lat, lng: r.geometry.location.lng,
      address: r.formatted_address, placeId: r.place_id
    });
  });
}

function reverseGeocode(loc, cb) {
  getJSON('maps', '/maps/api/geocode/json?latlng=' + loc[0] + ',' + loc[1] +
    '&key=' + encodeURIComponent(config.key), 'Geocoding API', function (err, j) {
    if (err) return cb(err);
    var r = (j.results || [])[0];
    cb(null, r ? { address: r.formatted_address, placeId: r.place_id } : null);
  });
}

// --- Routes API -----------------------------------------------------------
var ROUTE_FIELDS_FULL = [
  'routes.duration', 'routes.staticDuration', 'routes.distanceMeters', 'routes.description',
  'routes.polyline.encodedPolyline', 'routes.travelAdvisory.tollInfo', 'routes.warnings',
  'routes.legs.steps.distanceMeters', 'routes.legs.steps.staticDuration',
  'routes.legs.steps.polyline.encodedPolyline', 'routes.legs.steps.navigationInstruction',
  'routes.legs.steps.travelMode', 'routes.legs.steps.transitDetails',
  'routes.legs.steps.startLocation', 'routes.legs.steps.endLocation'
].join(',');
var ROUTE_FIELDS_SUMMARY = 'routes.duration,routes.distanceMeters';

function waypoint(p) {
  if (p.placeId && p.placeId.indexOf('ChIJ') === 0 && !p.lat) return { placeId: p.placeId };
  return { location: { latLng: { latitude: p.lat, longitude: p.lng } } };
}

function computeRoute(origin, dest, mode, prefs, full, cb) {
  var body = {
    origin: waypoint({ lat: origin[0], lng: origin[1] }),
    destination: waypoint(dest),
    travelMode: P.ROUTES_MODE[mode],
    languageCode: config.language,
    units: prefs.imperial ? 'IMPERIAL' : 'METRIC'
  };
  if (mode === P.MODE.DRIVE) {
    body.routingPreference = 'TRAFFIC_AWARE';
    body.routeModifiers = {
      avoidTolls: !!prefs.avoidTolls,
      avoidHighways: !!prefs.avoidHighways,
      avoidFerries: !!prefs.avoidFerries
    };
  }
  if (mode === P.MODE.TRANSIT) {
    body.departureTime = new Date(Date.now() + 60000).toISOString();
  }
  postJSON('routes', '/directions/v2:computeRoutes', full ? ROUTE_FIELDS_FULL : ROUTE_FIELDS_SUMMARY, body,
    'Routes API', function (err, j) {
      if (err) return cb(err);
      var r = (j.routes || [])[0];
      if (!r) return cb({ code: P.ERR.NO_RESULTS, title: 'No route', text: 'Google couldn\'t find a ' + P.MODE_NAMES[mode].toLowerCase() + ' route there.' });
      cb(null, r);
    });
}

// Upcoming departures of one transit line between two stops (the schedule page).
// Google's routes include live times where the agency shares them.
// cb(err, { deps: [{ time: ISO, line, headsign }], warnings: [] })
var TRANSIT_MODE = { BUS: 'BUS', INTERCITY_BUS: 'BUS', TROLLEYBUS: 'BUS', SHARE_TAXI: 'BUS', SUBWAY: 'SUBWAY', METRO_RAIL: 'SUBWAY',
  TRAM: 'LIGHT_RAIL', LIGHT_RAIL: 'LIGHT_RAIL', MONORAIL: 'LIGHT_RAIL', HEAVY_RAIL: 'TRAIN', COMMUTER_TRAIN: 'TRAIN',
  HIGH_SPEED_TRAIN: 'TRAIN', LONG_DISTANCE_TRAIN: 'TRAIN', RAIL: 'RAIL' };

function transitDepartures(from, to, vehicleType, line, cb) {
  var deps = [], warnings = [], seen = {}, rounds = 0;
  var start = new Date(Date.now() - 60000);
  function ask() {
    var body = {
      origin: waypoint({ lat: from[0], lng: from[1] }),
      destination: waypoint({ lat: to[0], lng: to[1] }),
      travelMode: 'TRANSIT', computeAlternativeRoutes: true, languageCode: config.language,
      departureTime: start.toISOString()
    };
    if (TRANSIT_MODE[vehicleType]) body.transitPreferences = { allowedTravelModes: [TRANSIT_MODE[vehicleType]] };
    postJSON('routes', '/directions/v2:computeRoutes', 'routes.legs.steps.transitDetails,routes.warnings', body, 'Routes API',
      function (err, j) {
        if (err) return deps.length ? done() : cb(err);
        var latest = null;
        (j.routes || []).forEach(function (r) {
          (r.warnings || []).forEach(function (w) { if (warnings.indexOf(w) < 0) warnings.push(w); });
          var steps = [];
          (r.legs || []).forEach(function (l) { (l.steps || []).forEach(function (st) { if (st.transitDetails) steps.push(st.transitDetails); }); });
          var td = steps[0];
          if (!td) return;
          var tl = td.transitLine || {};
          var name = tl.nameShort || tl.name || '';
          var when = td.stopDetails && td.stopDetails.departureTime;
          if (!when) return;
          if (!latest || when > latest) latest = when;
          if (line && name && name !== line) return;   // another line
          if (seen[when]) return;
          seen[when] = true;
          deps.push({ time: when, line: name, headsign: td.headsign || '' });
        });
        rounds++;
        if (latest && deps.length < 6 && rounds < 3) {
          start = new Date(new Date(latest).getTime() + 60000);
          return ask();
        }
        done();
      });
  }
  function done() {
    deps.sort(function (a, b) { return a.time < b.time ? -1 : 1; });
    cb(null, { deps: deps.slice(0, 8), warnings: warnings });
  }
  ask();
}

// --- Maps Static API ------------------------------------------------------
var STYLES = {
  light: [
    'element:geometry|color:0xffffff',
    'element:labels.icon|visibility:off',
    'element:labels.text.fill|color:0x555555',
    'element:labels.text.stroke|color:0xffffff|weight:2',
    'feature:poi|element:labels|visibility:off',
    'feature:poi.park|element:geometry|color:0xaaffaa',
    'feature:poi.park|element:labels.text|visibility:on',
    'feature:poi.park|element:labels.text.fill|color:0x005500',
    'feature:water|element:geometry|color:0x55aaff',
    'feature:water|element:labels.text.fill|color:0x0055aa',
    'feature:road|element:geometry.stroke|visibility:off',
    'feature:road.local|element:geometry.fill|color:0xaaaaaa',
    'feature:road.arterial|element:geometry.fill|color:0xaaaaaa',
    'feature:road.highway|element:geometry.fill|color:0xffaa00',
    'feature:road.highway|element:labels.text.fill|color:0x555555',
    'feature:transit|visibility:off',
    'feature:administrative|element:geometry|visibility:off',
    'feature:administrative.locality|element:labels.text.fill|color:0x000000'
  ],
  dark: [
    'element:geometry|color:0x000055',
    'element:labels.icon|visibility:off',
    'element:labels.text.fill|color:0xaaaaaa',
    'element:labels.text.stroke|color:0x000055|weight:2',
    'feature:poi|element:labels|visibility:off',
    'feature:poi.park|element:geometry|color:0x005500',
    'feature:water|element:geometry|color:0x0000aa',
    'feature:road|element:geometry.stroke|visibility:off',
    'feature:road.local|element:geometry.fill|color:0x555555',
    'feature:road.arterial|element:geometry.fill|color:0x555555',
    'feature:road.highway|element:geometry.fill|color:0xaa5500',
    'feature:transit|visibility:off',
    'feature:administrative|element:geometry|visibility:off',
    'feature:administrative.locality|element:labels.text.fill|color:0xffffff'
  ],
  bw: [
    'element:geometry|color:0xffffff',
    'element:labels.icon|visibility:off',
    'element:labels.text.fill|color:0x000000',
    'element:labels.text.stroke|color:0xffffff|weight:3',
    'feature:poi|element:labels|visibility:off',
    'feature:poi.park|element:geometry|color:0xd8d8d8',
    'feature:water|element:geometry|color:0x909090',
    'feature:road|element:geometry.stroke|visibility:off',
    'feature:road.local|element:geometry.fill|color:0x000000|weight:1',
    'feature:road.arterial|element:geometry.fill|color:0x000000',
    'feature:road.highway|element:geometry.fill|color:0x000000',
    'feature:transit|visibility:off',
    'feature:administrative|element:geometry|visibility:off'
  ]
};

// opts: { center:[lat,lng], zoom, w, h, style:'light'|'dark'|'bw', path:[[lat,lng]...] }
function staticMapUrl(opts) {
  var q = [
    'center=' + opts.center[0].toFixed(6) + ',' + opts.center[1].toFixed(6),
    'zoom=' + opts.zoom,
    'size=' + opts.w + 'x' + opts.h,
    'scale=1',
    'format=png',
    'maptype=roadmap',
    'language=' + config.language
  ];
  (STYLES[opts.style] || STYLES.light).forEach(function (s) { q.push('style=' + encodeURIComponent(s)); });
  if (opts.path && opts.path.length > 1) {
    var color = opts.style === 'bw' ? '0x000000ff' : (opts.style === 'dark' ? '0x55aaffff' : '0x0055ffff');
    var geo = require('./geo');
    var pts = opts.path;
    var enc = geo.encodePolyline(pts);
    var maxPts = 400;
    while (enc.length > 6000 && maxPts > 20) {
      enc = geo.encodePolyline(geo.thin(pts, maxPts));
      maxPts = Math.floor(maxPts * 0.7);
    }
    q.push('path=' + encodeURIComponent('color:' + color + '|weight:' + (opts.style === 'bw' ? 4 : 5) + '|enc:' + enc));
  }
  if (opts.markers) opts.markers.forEach(function (m) { q.push('markers=' + encodeURIComponent(m)); });
  q.push('key=' + encodeURIComponent(config.key));
  return url('maps', '/maps/api/staticmap?' + q.join('&'));
}

function staticMap(opts, cb) {
  getBinary(staticMapUrl(opts), function (err, bytes) {
    if (err) return cb(friendlyError(err.status, err.body, 'Maps Static API'));
    cb(null, bytes);
  });
}

// Check every service the key needs. cb(results) with {name, ok, error}
function checkKey(loc, cb) {
  loc = loc || [40.7580, -73.9855];
  var results = [];
  var pending = 4;
  function done(name, err) {
    results.push({ name: name, ok: !err, error: err });
    if (--pending === 0) cb(results);
  }
  staticMap({ center: loc, zoom: 14, w: 64, h: 64, style: 'light' }, function (err) { done('Maps Static API', err); });
  searchText('coffee', loc, function (err) { done('Places API', err); });
  computeRoute(loc, { lat: loc[0] + 0.01, lng: loc[1] + 0.01 }, P.MODE.WALK, {}, false, function (err) {
    // "no route" still means the API answered
    done('Routes API', err && err.code !== P.ERR.NO_RESULTS ? err : null);
  });
  reverseGeocode(loc, function (err) { done('Geocoding API', err); });
}

module.exports = {
  setLanguage: setLanguage,
  language: language,
  _internal: { STYLES: STYLES, PLACE_FIELDS: PLACE_FIELDS, ROUTE_FIELDS_FULL: ROUTE_FIELDS_FULL, ROUTE_FIELDS_SUMMARY: ROUTE_FIELDS_SUMMARY, INFO_FIELDS: INFO_FIELDS },
  tts: tts,
  autocomplete: autocomplete,
  placeDetails: placeDetails,
  searchPois: searchPois,
  searchOpen: searchOpen,
  placeInfo: placeInfo,
  placePhoto: placePhoto,
  transitDepartures: transitDepartures,
  setKey: setKey,
  hasKey: hasKey,
  isTest: isTest,
  searchText: searchText,
  searchNearby: searchNearby,
  geocode: geocode,
  reverseGeocode: reverseGeocode,
  computeRoute: computeRoute,
  staticMap: staticMap,
  staticMapUrl: staticMapUrl,
  checkKey: checkKey,
  friendlyError: friendlyError,
  config: config
};
