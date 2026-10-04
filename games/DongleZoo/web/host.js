// Host bridge adapter for the web build (copied next to index.html by `oe package --web`).
// The game asks for ads through host.call (scripts/lib/ads.lua); this file answers with
// Google's H5 Games Ads (the AdSense "Ad Placement API": adBreak / adConfig).
//
// Set AD_CLIENT to your AdSense publisher id to turn real ads on. While it is empty every
// call returns false and the game shows its built-in demo overlays instead.
// TEST_ADS keeps Google's test creatives on; switch it off only for the released game.
// Purchases are not handled here (the web has no store): "iap.purchase" returns false.
(function () {
  "use strict";
  var AD_CLIENT = "";      // e.g. "ca-pub-0000000000000000"
  var TEST_ADS = true;

  function send(name, data) {
    if (window.Module && Module.oeHostSend) Module.oeHostSend(name, data);
  }

  var ready = false;
  if (AD_CLIENT) {
    var s = document.createElement("script");
    s.async = true;
    s.src = "https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js";
    s.setAttribute("data-ad-client", AD_CLIENT);
    s.setAttribute("data-ad-frequency-hint", "150s");
    if (TEST_ADS) s.setAttribute("data-adbreak-test", "on");
    s.onerror = function () { ready = false; };  // blocked by an ad blocker: the game falls back to its demo flow
    document.head.appendChild(s);
    window.adsbygoogle = window.adsbygoogle || [];
    window.adBreak = window.adConfig = function (o) { window.adsbygoogle.push(o); };
    window.adConfig({ preloadAdBreaks: "on", sound: "on", onReady: function () { ready = true; } });
  }

  window.OEHost = {
    // Returns true when the request was taken; the answer follows through Module.oeHostSend.
    call: function (name, data) {
      if (name === "game.ready") return true;
      if (!ready) return false;
      if (name === "ads.rewarded") {
        var viewed = false, offered = false;
        window.adBreak({
          type: "reward",
          name: String(data.placement || "reward"),
          beforeReward: function (showAd) { offered = true; showAd(); },
          adViewed: function () { viewed = true; },
          adDismissed: function () { viewed = false; },
          adBreakDone: function () { send("ads.result", { ok: offered && viewed }); }
        });
        return true;
      }
      if (name === "ads.interstitial") {
        window.adBreak({
          type: "next",
          name: String(data.placement || "break"),
          adBreakDone: function () { send("ads.result", { ok: true }); }
        });
        return true;
      }
      return false;
    }
  };
})();
