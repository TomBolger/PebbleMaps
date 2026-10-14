// Pebble Maps - settings, favorites and recents (stored on the phone)
//
// Clay keeps its values in localStorage['clay-settings']. Favorites live
// there too (as a JSON string) so the settings page and the watch always
// see the same list.

var P = require('./protocol');

var CLAY_KEY = 'clay-settings';
var RECENTS_KEY = 'pm-recents';
var MAX_FAVS = 20;
var MAX_RECENTS = 10;

function readClay() {
  try {
    return JSON.parse(localStorage.getItem(CLAY_KEY)) || {};
  } catch (e) {
    return {};
  }
}

function writeClay(obj) {
  localStorage.setItem(CLAY_KEY, JSON.stringify(obj));
}

function bool(v, def) {
  if (v === undefined || v === null || v === '') return def;
  return v === true || v === 1 || v === '1' || v === 'true';
}

function get() {
  var c = readClay();
  var units = c.units || 'auto';
  var imperial;
  if (units === 'imperial') imperial = true;
  else if (units === 'metric') imperial = false;
  else {
    var lang = (typeof navigator !== 'undefined' && navigator.language) || 'en-US';
    imperial = /-(US|LR|MM|GB)$/i.test(lang) || lang === 'en';
  }
  // language for Google's answers: chosen in Settings, or the phone's
  var language = c.language && c.language !== 'auto' ? String(c.language) :
    String((typeof navigator !== 'undefined' && navigator.language) || 'en-US');
  var dm = parseInt(c.defaultMode, 10);
  return {
    apiKey: String(c.apiKey || '').trim(),
    defaultMode: isNaN(dm) ? P.MODE.DRIVE : dm,
    imperial: imperial,
    language: language,
    avoidTolls: bool(c.avoidTolls, false),
    avoidHighways: bool(c.avoidHighways, false),
    avoidFerries: bool(c.avoidFerries, false),
    vibrate: bool(c.vibrate, true),
    allModeTimes: bool(c.allModeTimes, true),
    economical: bool(c.economical, false),
    mapStyle: c.mapStyle || 'light',
    liveLocation: bool(c.liveLocation, true),
    disableTouch: bool(c.disableTouch, false),
    nonTouchUI: bool(c.nonTouchUI, false),
    textSize: (c.textSize === undefined || c.textSize === '' || c.textSize === 'auto') ? 3 : (parseInt(c.textSize, 10) || 0)
  };
}

// --- Favorites ------------------------------------------------------------
// { name, address, lat, lng, placeId, mode: 0-3 or 'default' }
function favorites() {
  var c = readClay();
  var list;
  try {
    list = JSON.parse(c.favorites || '[]');
  } catch (e) {
    list = [];
  }
  if (!Array.isArray(list)) list = [];
  return list.filter(function (f) { return f && (f.name || f.address); }).slice(0, MAX_FAVS);
}

function saveFavorites(list) {
  var c = readClay();
  c.favorites = JSON.stringify(list.slice(0, MAX_FAVS));
  writeClay(c);
}

function favMode(f, settings) {
  var m = parseInt(f && f.mode, 10);
  return isNaN(m) || m < 0 || m > 3 ? settings.defaultMode : m;
}

function sameplace(a, b) {
  if (!a || !b) return false;
  if (a.placeId && b.placeId && a.placeId === b.placeId) return true;
  if (a.lat !== undefined && b.lat !== undefined && a.lat !== null && b.lat !== null) {
    var dLat = Math.abs(a.lat - b.lat), dLng = Math.abs(a.lng - b.lng);
    if (dLat < 0.0003 && dLng < 0.0004 && (a.name === b.name || !a.name || !b.name)) return true;
  }
  return false;
}

function findFavorite(place) {
  var list = favorites();
  for (var i = 0; i < list.length; i++) if (sameplace(list[i], place)) return i;
  return -1;
}

function addFavorite(place) {
  var list = favorites();
  if (findFavorite(place) >= 0) return false;
  if (list.length >= MAX_FAVS) return false;
  list.push({
    name: place.name, address: place.address || place.fullAddress || '',
    lat: place.lat, lng: place.lng, placeId: place.placeId || '', mode: 'default'
  });
  saveFavorites(list);
  return true;
}

function removeFavorite(index) {
  var list = favorites();
  if (index < 0 || index >= list.length) return;
  list.splice(index, 1);
  saveFavorites(list);
}

function setFavoriteMode(index, mode) {
  var list = favorites();
  if (index < 0 || index >= list.length) return;
  list[index].mode = (mode === P.MODE_USE_DEFAULT) ? 'default' : mode;
  saveFavorites(list);
}

function updateFavoriteLocation(index, loc) {
  var list = favorites();
  if (!list[index]) return;
  list[index].lat = loc.lat;
  list[index].lng = loc.lng;
  if (loc.placeId) list[index].placeId = loc.placeId;
  list[index].geocodedFrom = list[index].address;
  saveFavorites(list);
}

// --- Recents --------------------------------------------------------------
function recents() {
  try {
    var r = JSON.parse(localStorage.getItem(RECENTS_KEY) || '[]');
    return Array.isArray(r) ? r : [];
  } catch (e) {
    return [];
  }
}

function addRecent(place) {
  var list = recents().filter(function (r) { return !sameplace(r, place); });
  list.unshift({
    name: place.name, address: place.address || '', lat: place.lat, lng: place.lng,
    placeId: place.placeId || '', type: place.type || ''
  });
  localStorage.setItem(RECENTS_KEY, JSON.stringify(list.slice(0, MAX_RECENTS)));
}

module.exports = {
  get: get,
  readClay: readClay,
  writeClay: writeClay,
  favorites: favorites,
  saveFavorites: saveFavorites,
  favMode: favMode,
  findFavorite: findFavorite,
  addFavorite: addFavorite,
  removeFavorite: removeFavorite,
  setFavoriteMode: setFavoriteMode,
  updateFavoriteLocation: updateFavoriteLocation,
  recents: recents,
  addRecent: addRecent,
  sameplace: sameplace
};
