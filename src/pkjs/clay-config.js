// Pebble Maps - settings page layout (Clay)

function config() {
  return [
    { type: 'pmtheme' },
    { type: 'pmhero' },
    {
      type: 'section',
      items: [
        { type: 'heading', defaultValue: '★ Favorites', size: 4 },
        {
          type: 'text',
          defaultValue: 'Places you go often. Each one can have its own way of getting there. ' +
            'On the watch, press the red pin button (<strong>Down</strong>) on the map, then Favorites.'
        },
        { type: 'pmfavorites', messageKey: 'favorites', defaultValue: '[]' }
      ]
    },
    {
      type: 'section',
      items: [
        { type: 'heading', defaultValue: 'Getting around', size: 4 },
        {
          type: 'select',
          messageKey: 'defaultMode',
          label: 'Default travel mode',
          defaultValue: '0',
          description: 'Used for new places. You can still pick a different way each time.',
          options: [
            { label: 'Drive', value: '0' },
            { label: 'Walk', value: '1' },
            { label: 'Bike', value: '2' },
            { label: 'Transit', value: '3' }
          ]
        },
        {
          type: 'select',
          messageKey: 'language',
          label: 'Language for places and directions',
          description: 'Place names, addresses, turn instructions and spoken directions. To speak searches in another language, set the dictation language in the Pebble app on your phone.',
          defaultValue: 'auto',
          options: [
            { label: 'Automatic (phone language)', value: 'auto' },
            { label: 'English', value: 'en' }, { label: 'Español', value: 'es' }, { label: 'Français', value: 'fr' },
            { label: 'Deutsch', value: 'de' }, { label: 'Italiano', value: 'it' }, { label: 'Português', value: 'pt' },
            { label: 'Nederlands', value: 'nl' }, { label: 'Svenska', value: 'sv' }, { label: 'Dansk', value: 'da' },
            { label: 'Norsk', value: 'nb' }, { label: 'Suomi', value: 'fi' }, { label: 'Polski', value: 'pl' },
            { label: 'Čeština', value: 'cs' }, { label: 'Türkçe', value: 'tr' }
          ]
        },
        {
          type: 'select',
          messageKey: 'units',
          label: 'Distances in',
          defaultValue: 'auto',
          options: [
            { label: 'Automatic', value: 'auto' },
            { label: 'Miles & feet', value: 'imperial' },
            { label: 'Kilometers & meters', value: 'metric' }
          ]
        },
        {
          type: 'toggle',
          messageKey: 'allModeTimes',
          label: 'Show times for every travel mode',
          description: 'When you pick a place, see how long it takes to drive, walk, bike or take transit.',
          defaultValue: true
        },
        {
          type: 'toggle',
          messageKey: 'vibrate',
          label: 'Buzz before turns',
          defaultValue: true
        },
        {
          type: 'toggle',
          messageKey: 'avoidTolls',
          label: 'Avoid tolls (driving)',
          defaultValue: false
        },
        {
          type: 'toggle',
          messageKey: 'avoidHighways',
          label: 'Avoid highways (driving)',
          defaultValue: false
        },
        {
          type: 'toggle',
          messageKey: 'avoidFerries',
          label: 'Avoid ferries (driving)',
          defaultValue: false
        },
        { type: 'heading', defaultValue: 'Map', size: 4 },
        {
          type: 'toggle',
          messageKey: 'economical',
          label: 'Economical mode',
          description: 'Reduce Google requests: skip map prefetching, automatic places on the map, and estimates for all travel modes. Searches and directions still work. This does not cap your bill.',
          defaultValue: false
        },
        {
          type: 'toggle',
          messageKey: 'liveLocation',
          label: 'Live location',
          description: 'Your blue dot moves with you on the map (it turns into an arrow while you\'re moving). ' +
            'Turn off to save battery: the map then shows where you were when you opened it.',
          defaultValue: true
        },
        { type: 'heading', id: 'touchHeading', defaultValue: 'Touch screen', size: 4 },
        {
          type: 'toggle',
          messageKey: 'disableTouch',
          id: 'disableTouch',
          label: 'Disable touch',
          description: 'Turns off touch in Pebble Maps completely; use the buttons only. This also switches on the button layout below.',
          defaultValue: false
        },
        {
          type: 'toggle',
          messageKey: 'nonTouchUI',
          id: 'nonTouchUI',
          label: 'Non-touch UI',
          description: 'Gives this watch the layout made for watches without touch (no search bar; the gear holds the options). ' +
            'Touch still works. Every button is available in both layouts.',
          defaultValue: false
        },
        { type: 'heading', defaultValue: 'Display', size: 4 },
        {
          type: 'select',
          messageKey: 'textSize',
          label: 'Text size on the watch',
          defaultValue: 'auto',
          description: 'Automatic uses Large on Pebble Time 2 and Round 2.',
          options: [
            { label: 'Automatic', value: 'auto' },
            { label: 'Standard', value: '0' },
            { label: 'Large', value: '1' },
            { label: 'Extra large', value: '2' }
          ]
        },
        {
          type: 'select',
          messageKey: 'mapStyle',
          id: 'mapStyle',
          label: 'Map colors',
          defaultValue: 'light',
          description: 'For color watches.',
          options: [
            { label: 'Light (like Google Maps)', value: 'light' },
            { label: 'Dark (night)', value: 'dark' }
          ]
        }
      ]
    },
    {
      type: 'section',
      items: [
        { type: 'heading', defaultValue: 'Using Pebble Maps', size: 4 },
        {
          type: 'text',
          defaultValue:
            '<strong>Map screen (buttons):</strong> Up = move the map (zoom, then Select ⋯ for up/down and left/right) · ' +
            'Select = search by voice · Down (gear) = options.<br><br>' +
            '<strong>Map screen (Pebble Time 2 / Round 2):</strong> Up (gear) = options · Select = search by voice · ' +
            'Down (red pin) = Places. Tap the search bar to type. Drag the map, double-tap to zoom in, press and hold to zoom out.<br><br>' +
            '<strong>Options (gear):</strong> the top button switches between heading up (the map turns with you), ' +
            'north up, and a plain map that stays put; each press also brings the map back to you. ' +
            'The middle button opens Places (buttons layout) or the map controls (touch layout). ' +
            'The bottom button opens Places on map: turn places and their names on or off, and pick which kinds ' +
            '(restaurants, coffee, ATMs...). Tap a place or its name for photos, hours and reviews. ' +
            '"Open now" (on at first, shared with the Places menu) hides places that are closed right now ' +
            'on the map, in Places lists and in searches. Places without opening hours (parks, addresses, ' +
            'landmarks) still show.<br><br>' +
            '<strong>Heading up:</strong> standing still for 30 seconds turns the map back to north up. ' +
            'If you move the map yourself, it stays there while you stand still and follows you again ' +
            'a few seconds after you start moving.<br><br>' +
            '<strong>Place info:</strong> on a place, press Down (<strong>i</strong>) for photos, hours and reviews. ' +
            'Select shows the next photo; hold Select for directions.<br><br>' +
            '<strong>Results:</strong> Up/Down moves between pins, Select opens the place. ' +
            'Hold Select for map controls and the list view.<br><br>' +
            '<strong>Place and route screens:</strong> hold Select to move the map.<br><br>' +
            '<strong>Transit:</strong> in the directions list, rides have a blue <strong>i</strong>: press Select for the ' +
            'next departures.<br><br>' +
            '<strong>Navigating:</strong> press Select to open the options bar: move the map, alerts ' +
            '(buzz → buzz + voice on Pebble Time 2 / Round 2 → off), voice volume, ' +
            'hide the direction cards (they come back before each turn), see all steps, overview, or end. ' +
            'Tap the compass to switch between north-up and the map turning with you. ' +
            'Press Back twice to stop.'
        }
      ]
    },
    {
      type: 'section',
      items: [
        { type: 'heading', defaultValue: 'Your Google Maps key', size: 4 },
        { type: 'pmstatus' },
        {
          type: 'input',
          messageKey: 'apiKey',
          defaultValue: '',
          label: 'Google Maps key',
          description: 'Stored only on your phone and sent only to Google.',
          attributes: {
            placeholder: 'Paste here — starts with AIza',
            type: 'text',
            autocapitalize: 'off',
            autocorrect: 'off',
            autocomplete: 'off',
            spellcheck: 'false'
          }
        },
        { type: 'pmkeytest' }
      ]
    },
    {
      type: 'section',
      items: [
        { type: 'pmguide' }
      ]
    },
    {
      type: 'text',
      defaultValue: '<div class="pm-footer">Map data and directions by Google. Pebble Maps is not made by or affiliated with Google.</div>'
    },
    { type: 'submit', defaultValue: 'Save settings' }
  ];
}

// Runs inside the settings page
function customFn() {
  var clayConfig = this;
  clayConfig.on(clayConfig.EVENTS.AFTER_BUILD, function () {
    var ud = (clayConfig.meta && clayConfig.meta.userData) || {};
    var style = clayConfig.getItemById('mapStyle');
    if (style && ud.color === false) style.hide();
    // Touch settings: only for watches with a touch screen
    var info = clayConfig.meta && clayConfig.meta.activeWatchInfo;
    var plat = info && info.platform;
    var off = clayConfig.getItemById('disableTouch'), ui = clayConfig.getItemById('nonTouchUI');
    var head = clayConfig.getItemById('touchHeading');
    if (plat && plat !== 'emery' && plat !== 'gabbro') {
      [off, ui, head].forEach(function (it) { if (it) it.hide(); });
    } else if (off && ui) {
      // no touch means the button layout
      var sync = function () {
        if (off.get()) { ui.set(true); ui.disable(); } else ui.enable();
      };
      off.on('change', sync);
      sync();
    }
  });
}

module.exports = { config: config, customFn: customFn };
