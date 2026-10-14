// Pebble Maps - turn-by-turn navigation engine
var P = require('./protocol');
var geo = require('./geo');
var fmt = require('./format');
var brief = require('./brief');

var MAN_MAP = {
  TURN_SLIGHT_LEFT: P.MAN.SLIGHT_LEFT, TURN_SHARP_LEFT: P.MAN.SHARP_LEFT, UTURN_LEFT: P.MAN.UTURN_LEFT,
  TURN_LEFT: P.MAN.LEFT, TURN_SLIGHT_RIGHT: P.MAN.SLIGHT_RIGHT, TURN_SHARP_RIGHT: P.MAN.SHARP_RIGHT,
  UTURN_RIGHT: P.MAN.UTURN_RIGHT, TURN_RIGHT: P.MAN.RIGHT, STRAIGHT: P.MAN.STRAIGHT,
  RAMP_LEFT: P.MAN.RAMP_LEFT, RAMP_RIGHT: P.MAN.RAMP_RIGHT, MERGE: P.MAN.MERGE,
  FORK_LEFT: P.MAN.FORK_LEFT, FORK_RIGHT: P.MAN.FORK_RIGHT, FERRY: P.MAN.FERRY,
  FERRY_TRAIN: P.MAN.FERRY, ROUNDABOUT_LEFT: P.MAN.ROUNDABOUT_LEFT,
  ROUNDABOUT_RIGHT: P.MAN.ROUNDABOUT_RIGHT, DEPART: P.MAN.DEPART, NAME_CHANGE: P.MAN.STRAIGHT
};

function vehicleIcon(type) {
  type = type || '';
  if (/BUS|TROLLEY|SHARE_TAXI/.test(type)) return P.MAN.BUS;
  if (/SUBWAY|METRO/.test(type)) return P.MAN.SUBWAY;
  if (/TRAM|LIGHT_RAIL|MONORAIL|CABLE|FUNICULAR|GONDOLA/.test(type)) return P.MAN.TRAM;
  if (/FERRY/.test(type)) return P.MAN.FERRY;
  if (/RAIL|TRAIN/.test(type)) return P.MAN.TRAIN;
  return P.MAN.BUS;
}

function latLngOf(stop) {
  var l = stop && stop.location && stop.location.latLng;
  return l ? [l.latitude, l.longitude] : null;
}

// Google writes times as "2:38\u202fPM"; the watch fonts only have plain spaces
function spaces(t) { return t.replace(/[\u00a0\u2007\u2009\u202f]/g, ' '); }

function transitInfo(td) {
  if (!td) return null;
  var line = td.transitLine || {};
  var veh = line.vehicle || {};
  var stops = td.stopDetails || {};
  var lv = td.localizedValues || {};
  var lineName = line.nameShort || line.name || (veh.name && veh.name.text) || 'transit';
  var vehName = (veh.name && veh.name.text) || '';
  return {
    line: lineName,
    vehicle: vehName,
    icon: vehicleIcon(veh.type),
    headsign: td.headsign || '',
    stops: td.stopCount || 0,
    from: (stops.departureStop && stops.departureStop.name) || '',
    to: (stops.arrivalStop && stops.arrivalStop.name) || '',
    departs: spaces((lv.departureTime && lv.departureTime.time && lv.departureTime.time.text) || ''),
    // for the schedule page: where this ride starts and ends, and when
    fromLoc: latLngOf(stops.departureStop), toLoc: latLngOf(stops.arrivalStop),
    depTime: stops.departureTime || '', vehicleType: veh.type || '', agency: ((line.agencies || [])[0] || {}).name || '',
    arrives: spaces((lv.arrivalTime && lv.arrivalTime.time && lv.arrivalTime.time.text) || '')
  };
}

// Turn a Routes API route into our navigation model
function buildRoute(r, mode, dest) {
  var points = [];
  var steps = [];
  var legs = r.legs || [];
  legs.forEach(function (leg) {
    (leg.steps || []).forEach(function (st) {
      var pts = geo.decodePolyline(st.polyline && st.polyline.encodedPolyline);
      if (!pts.length && st.startLocation && st.endLocation) {
        pts = [[st.startLocation.latLng.latitude, st.startLocation.latLng.longitude],
               [st.endLocation.latLng.latitude, st.endLocation.latLng.longitude]];
      }
      var startIdx = points.length ? points.length - 1 : 0;
      for (var i = 0; i < pts.length; i++) {
        if (points.length && i === 0) {
          var last = points[points.length - 1];
          if (Math.abs(last[0] - pts[0][0]) < 1e-6 && Math.abs(last[1] - pts[0][1]) < 1e-6) continue;
        }
        points.push(pts[i]);
      }
      var ni = st.navigationInstruction || {};
      var tr = transitInfo(st.transitDetails);
      var instr = ni.instructions || '';
      var man = MAN_MAP[ni.maneuver] !== undefined ? MAN_MAP[ni.maneuver] : P.MAN.STRAIGHT;
      if (tr) {
        man = tr.icon;
        instr = 'Take ' + tr.line + (tr.headsign ? ' toward ' + tr.headsign : '');
      }
      steps.push({
        instruction: instr || 'Continue',
        short: tr ? instr : brief.brief(instr, ni.maneuver),          // for the watch banner
        spoken: tr ? instr : brief.brief(instr, ni.maneuver, true),   // for the voice
        man: man,
        startIdx: startIdx,
        endIdx: points.length - 1,
        meters: st.distanceMeters || 0,
        seconds: fmt.seconds(st.staticDuration),
        travelMode: st.travelMode || '',
        transit: tr
      });
    });
  });
  if (!points.length && r.polyline) points = geo.decodePolyline(r.polyline.encodedPolyline);
  // cumulative distances along the line
  var cum = [0];
  for (var i = 1; i < points.length; i++) cum.push(cum[i - 1] + geo.haversine(points[i - 1], points[i]));
  steps.forEach(function (s) {
    s.startDist = cum[s.startIdx] || 0;
    s.endDist = cum[s.endIdx] || 0;
  });
  var staticSum = steps.reduce(function (a, s) { return a + s.seconds; }, 0);
  var dur = fmt.seconds(r.duration);
  return {
    mode: mode,
    dest: dest,
    points: points,
    cum: cum,
    steps: steps,
    total: cum[cum.length - 1] || r.distanceMeters || 0,
    duration: dur,
    staticDuration: fmt.seconds(r.staticDuration) || staticSum,
    factor: staticSum > 0 && dur > 0 ? dur / staticSum : 1,
    description: r.description || '',
    hasTolls: !!(r.travelAdvisory && r.travelAdvisory.tollInfo),
    warnings: r.warnings || [],
    distanceMeters: r.distanceMeters || 0
  };
}

function stepList(route, imperial) {
  return route.steps.map(function (s, i) {
    var sub = fmt.distance(s.meters || (s.endDist - s.startDist), imperial);
    if (s.transit) {
      sub = (s.transit.departs ? s.transit.departs + ' · ' : '') + s.transit.stops + (s.transit.stops === 1 ? ' stop' : ' stops');
      return { title: fmt.clip(s.instruction + (s.transit.from ? ' from ' + s.transit.from : ''), 120), sub: sub, icon: s.man, extra: 1 };
    }
    return { title: fmt.clip(s.instruction.split('\n')[0], 120), sub: sub, icon: i === 0 && s.man === P.MAN.STRAIGHT ? P.MAN.DEPART : s.man };
  });
}

// Spoken words of our own (Google's instructions already come in the chosen language)
var SAY = {
  es: { reroute: 'Recalculando', arrived: 'Has llegado', at: ' a ', ahead: 'Tu destino está delante', getOff: 'Bájate en ', board: 'Sube al ', toward: ' hacia ' },
  fr: { reroute: 'Nouvel itinéraire', arrived: 'Vous êtes arrivé', at: ' à ', ahead: 'Votre destination est devant vous', getOff: 'Descendez à ', board: 'Prenez le ', toward: ' direction ' },
  de: { reroute: 'Route wird neu berechnet', arrived: 'Sie haben Ihr Ziel erreicht', at: ': ', ahead: 'Ihr Ziel liegt vor Ihnen', getOff: 'Aussteigen bei ', board: 'Einsteigen in ', toward: ' Richtung ' },
  it: { reroute: 'Ricalcolo del percorso', arrived: 'Sei arrivato', at: ' a ', ahead: 'La destinazione è davanti a te', getOff: 'Scendi a ', board: 'Prendi il ', toward: ' verso ' },
  pt: { reroute: 'Recalculando', arrived: 'Você chegou', at: ' a ', ahead: 'Seu destino está à frente', getOff: 'Desça em ', board: 'Pegue o ', toward: ' sentido ' },
  pl: { reroute: 'Wyznaczanie nowej trasy', arrived: 'Jesteś na miejscu', at: ': ', ahead: 'Cel jest przed tobą', getOff: 'Wysiądź na przystanku ', board: 'Wsiądź do ', toward: ' w kierunku ' },
  nl: { reroute: 'Route wordt herberekend', arrived: 'U bent aangekomen', at: ' bij ', ahead: 'Uw bestemming ligt voor u', getOff: 'Stap uit bij ', board: 'Neem ', toward: ' richting ' }
};
var SAY_EN = { reroute: 'Rerouting', arrived: 'You have arrived', at: ' at ', ahead: 'Your destination is ahead', getOff: 'Get off at ', board: 'Board ', toward: ' toward ' };

Navigator.prototype.words = function () {
  var lang = String((this.settings && this.settings.language) || 'en').split('-')[0].toLowerCase();
  return { lang: lang, w: SAY[lang] || SAY_EN };
};

// "In 0.3 miles, turn right" in English; other languages say Google's instruction on its own
Navigator.prototype.inDistance = function (meters, say) {
  if (this.words().lang !== 'en') return say;
  return 'In ' + brief.spokenDistance(fmt.distance(meters, this.settings.imperial, true)) + ', ' + lowerFirst(say);
};

// "Turn right" -> "turn right" (after "In 0.3 miles, ")
function lowerFirst(t) {
  t = String(t || '');
  return /^[A-Z][a-z]/.test(t) ? t.charAt(0).toLowerCase() + t.slice(1) : t;
}

// --- Navigator --------------------------------------------------------------
var THRESH = {
  0: { soon: 450, now: 80, off: 60 },   // drive
  1: { soon: 60, now: 15, off: 35 },    // walk
  2: { soon: 150, now: 30, off: 40 },   // bike
  3: { soon: 60, now: 15, off: 45 }     // transit (walking parts)
};

function Navigator(opts) {
  this.route = opts.route;
  this.view = opts.view;             // {w, h} of the watch drawing area
  this.settings = opts.settings;
  this.send = opts.send;             // function(dict)
  this.reroute = opts.reroute;       // function(position, cb(err, route))
  this.onArrive = opts.onArrive;
  this.speak = opts.speak || function () {};      // speak(text) when voice is on
  this.prepare = opts.prepare || function () {};  // prepare([texts]) ahead of time
  this.preparedKey = '';
  this.spokeStart = false;
  this.lastIdx = 0;
  this.offCount = 0;
  this.alerts = {};
  this.lastSent = 0;
  this.lastKey = '';
  this.rerouting = false;
  this.lastReroute = 0;
  this.arrived = false;
  this.lastFix = 0;
  this.heading = null;
}

Navigator.prototype.setRoute = function (route) {
  this.route = route;
  this.lastIdx = 0;
  this.offCount = 0;
  this.alerts = {};
  this.rerouting = false;
  this.preparedKey = '';
};

// Snap position (lat,lng) to the route. Returns {idx, t, s, off}
Navigator.prototype.snap = function (pos) {
  var pts = this.route.points;
  if (pts.length < 2) return { idx: 0, t: 0, s: 0, off: 0 };
  var best = null;
  var self = this;
  function scan(from, to) {
    for (var i = Math.max(0, from); i < Math.min(pts.length - 1, to); i++) {
      var a = geo.toLocal(pos, pts[i]);
      var b = geo.toLocal(pos, pts[i + 1]);
      var dx = b[0] - a[0], dy = b[1] - a[1];
      var len2 = dx * dx + dy * dy;
      var t = len2 > 0 ? Math.max(0, Math.min(1, -(a[0] * dx + a[1] * dy) / len2)) : 0;
      var px = a[0] + t * dx, py = a[1] + t * dy;
      var d = Math.sqrt(px * px + py * py);
      // slight preference for staying near the last position
      var penalty = i < self.lastIdx ? (self.lastIdx - i) * 2 : 0;
      if (!best || d + penalty < best.score) {
        best = { idx: i, t: t, off: d, score: d + penalty };
      }
    }
  }
  scan(this.lastIdx - 8, this.lastIdx + 80);
  if (!best || best.off > 80) scan(0, pts.length);
  var segLen = this.route.cum[best.idx + 1] - this.route.cum[best.idx];
  best.s = this.route.cum[best.idx] + best.t * segLen;
  return best;
};

Navigator.prototype.stepAt = function (s) {
  var steps = this.route.steps;
  for (var k = 0; k < steps.length; k++) {
    if (steps[k].endDist > s + 3) return k;
  }
  return steps.length - 1;
};

Navigator.prototype.remainingSeconds = function (k, s) {
  var steps = this.route.steps;
  var sec = 0;
  for (var i = k + 1; i < steps.length; i++) sec += steps[i].seconds;
  var st = steps[k];
  if (st) {
    var len = Math.max(1, st.endDist - st.startDist);
    sec += st.seconds * Math.max(0, Math.min(1, (st.endDist - s) / len));
  }
  return sec * (this.route.factor || 1);
};

// Smoothed direction of travel along the route (degrees, 0 = north)
Navigator.prototype.updateHeading = function (snap) {
  var r = this.route;
  var origin = this.snappedPoint(snap);
  var aheadIdx = snap.idx + 1;
  while (aheadIdx < r.points.length - 1 && r.cum[aheadIdx] - snap.s < 15) aheadIdx++;
  var target = r.points[Math.min(aheadIdx, r.points.length - 1)];
  var hdg = geo.bearing(origin, target);
  if (this.heading === null) this.heading = hdg;
  else {
    var diff = ((hdg - this.heading + 540) % 360) - 180;
    this.heading = (this.heading + diff * 0.6 + 360) % 360;
  }
  this.pos = origin;
};

Navigator.prototype.snappedPoint = function (snap) {
  var a = this.route.points[snap.idx], b = this.route.points[Math.min(snap.idx + 1, this.route.points.length - 1)];
  return [a[0] + (b[0] - a[0]) * snap.t, a[1] + (b[1] - a[1]) * snap.t];
};

Navigator.prototype.update = function (pos, accuracy, timestamp) {
  if (this.arrived) return;
  var now = Date.now();
  var r = this.route;
  var fixTime = timestamp === undefined ? now : timestamp;
  // only throw away fixes that are badly off (city GPS is often 30-80 m); arrival needs a better one
  if (!isFinite(accuracy) || accuracy <= 0) accuracy = 50;
  if (accuracy > 250 ||
      now - fixTime > 20000 || fixTime > now + 5000 || fixTime < this.lastFix) {
    this.offCount = 0;
    this.noGps('GPS signal weak…');
    return;
  }
  this.lastFix = fixTime;
  var th = THRESH[r.mode] || THRESH[0];
  var snap = this.snap(pos);
  var k = this.stepAt(snap.s);
  var step = r.steps[k];
  var flags = 0;
  var self = this;

  // --- off route?
  var riding = step && step.transit;
  var offLimit = th.off + Math.min(50, accuracy || 20) * 0.6;
  if (!riding && snap.off > offLimit) this.offCount++;
  else this.offCount = 0;
  if (this.offCount >= 3 && !this.rerouting && now - this.lastReroute > 15000) {
    this.rerouting = true;
    this.lastReroute = now;
    this.speak(this.words().w.reroute);
    this.reroute(pos, function (err, route) {
      self.rerouting = false;
      if (!err && route) {
        self.setRoute(route);
        self.send({ cmd: P.CMD.TOAST, text: 'New route' });
      }
    });
  }
  if (this.rerouting) flags |= P.NAV_FLAG.REROUTING;
  if (snap.off < offLimit) this.lastIdx = snap.idx;

  // --- arrived?
  var toEnd = r.total - snap.s;
  var destDist = r.dest ? geo.haversine(pos, [r.dest.lat, r.dest.lng]) : toEnd;
  var arrivalRadius = r.mode === P.MODE.DRIVE ? 30 : 15;
  if (accuracy <= (r.mode === P.MODE.DRIVE ? 75 : 50) &&
      ((toEnd < (r.mode === P.MODE.DRIVE ? 35 : 18) && snap.off < 60) || destDist < arrivalRadius)) {
    this.arrived = true;
    this.send({
      cmd: P.CMD.NAV, text: (r.dest && r.dest.name) || 'Destination', text2: '', text3: '', text4: '',
      num: P.MAN.ARRIVE, idx: P.MAN.NONE, num2: Math.round(now / 1000), flags: P.NAV_FLAG.ARRIVED
    });
    var wd = this.words().w;
    this.speak(wd.arrived + (r.dest && r.dest.name ? wd.at + r.dest.name : ''));
    if (this.onArrive) this.onArrive();
    return;
  }

  // --- what to show
  var imperial = this.settings.imperial;
  var text, man, distM, detail = '', alertKey, say = '';
  var next = r.steps[k + 1];
  if (step && step.transit) {
    var t = step.transit;
    distM = step.endDist - snap.s;
    man = t.icon;
    text = 'Ride ' + t.line + ' · get off at ' + (t.to || 'your stop');
    say = this.words().w.getOff + (t.to || '');
    detail = t.stops + (t.stops === 1 ? ' stop' : ' stops') + (t.arrives ? ' · arrive ' + t.arrives : '');
    flags |= P.NAV_FLAG.TRANSIT;
    alertKey = 'exit' + k;
  } else if (k === 0 && snap.s < 25 && step) {
    distM = step.endDist - snap.s;
    man = P.MAN.DEPART;
    text = step.short || step.instruction;
    say = step.spoken || step.instruction;
    alertKey = 'start';
  } else if (next) {
    distM = step.endDist - snap.s;
    man = next.man;
    text = next.short || next.instruction;
    say = next.spoken || next.instruction;
    if (next.transit) {
      var nt = next.transit;
      text = 'Board ' + nt.line + (nt.headsign ? ' toward ' + nt.headsign : '') + ' at ' + (nt.from || 'the stop');
      say = this.words().w.board + nt.line + (nt.headsign ? this.words().w.toward + nt.headsign : '');
      detail = nt.departs ? 'Departs ' + nt.departs : '';
      flags |= P.NAV_FLAG.TRANSIT;
    }
    alertKey = 'step' + (k + 1);
  } else {
    distM = toEnd;
    man = P.MAN.ARRIVE;
    text = 'Arrive at ' + ((r.dest && r.dest.name) || 'destination');
    say = this.words().w.ahead;
    alertKey = 'arrive';
  }
  distM = Math.max(0, distM);

  // "Then" chip: next maneuver comes quickly after this one
  var thenMan = P.MAN.NONE;
  if (next && !(step && step.transit)) {
    var after = r.steps[k + 2];
    var nextLen = next.endDist - next.startDist;
    if (nextLen < (r.mode === P.MODE.DRIVE ? 200 : 60)) thenMan = after ? after.man : P.MAN.ARRIVE;
  }

  // alerts
  if (!this.alerts[alertKey]) this.alerts[alertKey] = {};
  var al = this.alerts[alertKey];
  var soonAt = step && step.transit ? 700 : th.soon;
  var nowAt = step && step.transit ? 200 : th.now;
  // voice: the "soon" line uses the alert distance so it can be prepared in advance
  this.lastSay = say;
  this.lastDistM = distM;
  this.lastKeyName = alertKey;
  var soonText = this.inDistance(soonAt, say);
  if (this.preparedKey !== alertKey) {
    this.preparedKey = alertKey;
    this.prepare([say, soonText]);
  }
  if (!this.spokeStart) {
    this.spokeStart = true;
    if (step && k === 0) this.speak(step.spoken || step.instruction);
  }
  if (distM <= nowAt && !al.now) {
    al.now = al.soon = true;
    flags |= P.NAV_FLAG.ALERT_NOW;
    this.speak(say);
  } else if (distM <= soonAt && !al.soon && (step ? step.endDist - step.startDist : 0) > soonAt * 1.3) {
    al.soon = true;
    flags |= P.NAV_FLAG.ALERT_SOON;
    this.speak(distM > soonAt * 0.6 ? soonText : this.inDistance(distM, say));
  }

  if (distM <= soonAt * 1.15) flags |= P.NAV_FLAG.NEAR;
  var remain = this.remainingSeconds(k, snap.s);
  this.updateHeading(snap);

  var dict = {
    cmd: P.CMD.NAV,
    text: fmt.clip(text, 80),
    text2: fmt.distance(distM, imperial, true),
    text3: fmt.duration(remain) + ' · ' + fmt.distance(toEnd, imperial),
    text4: fmt.clip(detail, 60),
    num: man,
    idx: thenMan,
    num2: Math.round((now + remain * 1000) / 1000),
    flags: flags
  };
  this.lastDict = dict;
  var key = [k, man, dict.text2, flags].join('|');
  var important = (flags & (P.NAV_FLAG.ALERT_NOW | P.NAV_FLAG.ALERT_SOON)) || key.split('|')[0] !== this.lastKey.split('|')[0];
  if (important || now - this.lastSent > 1200) {
    this.lastSent = now;
    this.lastKey = key;
    this.send(dict);
  }
};

// What to say right now (when voice is switched on mid-drive)
Navigator.prototype.currentPhrase = function () {
  if (!this.lastSay) return 'Voice directions on';
  var d = this.lastDistM || 0;
  var th = THRESH[this.route.mode] || THRESH[0];
  if (d <= th.now || this.lastKeyName === 'start') return this.lastSay;
  return this.inDistance(d, this.lastSay);
};

Navigator.prototype.noGps = function (text) {
  this.send({
    cmd: P.CMD.NAV, text: text || 'Waiting for GPS…', text2: '', text3: '', text4: '',
    num: P.MAN.DEPART, idx: P.MAN.NONE, num2: 0, flags: P.NAV_FLAG.NO_GPS
  });
};

module.exports = {
  buildRoute: buildRoute,
  stepList: stepList,
  Navigator: Navigator,
  transitInfo: transitInfo
};
