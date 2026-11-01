// Pebble Maps - the watch's "overview": a small, wide, low-resolution picture of the map around
// the view. The watch shows it, blown up, wherever its sharp picture doesn't reach yet (panning
// past the edge, zooming out, waiting for a new picture), so the screen is never blank.
// One overview pixel is 4 x 4 screen pixels (it is drawn two zoom levels further out).

var SHIFT = 2;

// What overview should follow the view (main picture centered on world point c at `zoom`, turned
// by `heading`)? Returns null when the one the watch holds still does the job, else
// { zoom, c (world point at the overview's zoom), w, h }.
// sent: what the watch holds ({ zoom, heading, c }) or null; ow, oh: its size in pixels.
function next(sent, zoom, c, heading, ow, oh) {
  var oz = zoom - SHIFT;
  if (!ow || !oh || oz < 0) return null;
  var oc = [Math.round(c[0] / (1 << SHIFT)), Math.round(c[1] / (1 << SHIFT))];
  if (sent && sent.zoom === oz && sent.heading === heading) {
    // still centered well enough? The screen takes half the overview's width, so a sixth off
    // centre still leaves a screen's worth around the view
    if (Math.abs(sent.c[0] - oc[0]) <= ow / 6 && Math.abs(sent.c[1] - oc[1]) <= oh / 6) return null;
  }
  return { zoom: oz, c: oc, w: ow, h: oh };
}

module.exports = { next: next, SHIFT: SHIFT };
