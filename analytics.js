/*
  Privacy-friendly portfolio analytics with GoatCounter.

  Setup:
  GoatCounter account: erik-odal

  Tracking only runs
  on the production GitHub Pages hostname so local previews are not counted.
*/
(function () {
  'use strict';

  var GOATCOUNTER_CODE = 'erik-odal';
  var PRODUCTION_HOST = 'erik-odal.github.io';

  if (
    window.location.hostname !== PRODUCTION_HOST
  ) {
    return;
  }

  window.goatcounter = {
    // A generic ref tag can be added to links, e.g. ?ref=kth-application.
    // UTM source is also accepted. Otherwise the normal browser referrer is used.
    referrer: function () {
      var params = new URLSearchParams(window.location.search);
      return (
        params.get('ref') ||
        params.get('utm_source') ||
        document.referrer ||
        undefined
      );
    },

    // Keep page names tidy in the analytics dashboard.
    path: function () {
      var path = window.location.pathname;
      if (path === '/index.html' || path === '/') return '/';
      return path.replace(/\.html$/, '');
    }
  };

  var script = document.createElement('script');
  script.async = true;
  script.src = 'https://gc.zgo.at/count.js';
  script.setAttribute(
    'data-goatcounter',
    'https://' + GOATCOUNTER_CODE + '.goatcounter.com/count'
  );
  document.head.appendChild(script);
})();
