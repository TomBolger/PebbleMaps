// Pebble Maps - ordered, retrying AppMessage queue
// Two lanes: small replies (lists, place cards, pages, toasts) go ahead of bulk traffic
// (map pieces, markers, voice), so screens fill in while a map is still streaming.
// Order is kept within each lane.
var queue = [];
var busy = false;
var BULK = /^(map:|mk|voice|nav)/;   // nav updates stay in step with their map pictures

function isBulk(item) { return !!(item.tag && BULK.test(item.tag)); }

function pump() {
  if (busy || !queue.length) return;
  busy = true;
  var item = queue[0];
  Pebble.sendAppMessage(item.dict, function () {
    queue.shift();
    busy = false;
    if (item.done) item.done(true);
    pump();
  }, function (e) {
    busy = false;
    item.tries = (item.tries || 0) + 1;
    // replies the user is waiting for get more patience than map pieces (the watch may be busy drawing)
    if (item.tries >= (isBulk(item) ? 4 : 10)) {
      queue.shift();
      if (item.done) item.done(false);
    }
    setTimeout(pump, Math.min(2000, 150 * item.tries));
  });
}

// tag: optional string so stale messages can be dropped (e.g. old map images)
function send(dict, tag, done) {
  var item = { dict: dict, tag: tag || null, done: done };
  if (!isBulk(item)) {
    // after the message in flight and any other replies, but before bulk traffic
    var i = busy ? 1 : 0;
    while (i < queue.length && !isBulk(queue[i])) i++;
    queue.splice(i, 0, item);
  } else {
    queue.push(item);
  }
  pump();
}

function drop(tagPrefix) {
  // never drop the in-flight message (index 0 while busy)
  var start = busy ? 1 : 0;
  for (var i = queue.length - 1; i >= start; i--) {
    if (queue[i].tag && queue[i].tag.indexOf(tagPrefix) === 0) queue.splice(i, 1);
  }
}

function pending() { return queue.length; }

module.exports = { send: send, drop: drop, pending: pending };
