-- Ads and purchases behind one interface, so the game never knows which platform pays.
--
-- Hosted: when the app that embeds the player handles the request (host.call returns
-- true - on the web a page script defines window.OEHost, see web/host.js), the real ad or
-- store sheet is shown by that app and its answer arrives through host.poll():
--   host.call("ads.rewarded", {placement})     -> message "ads.result" {ok}
--   host.call("ads.interstitial", {placement}) -> message "ads.result" {ok}
--   host.call("iap.purchase", {product})       -> message "iap.result" {ok, product}
-- Demo: with no host the same calls open overlays that stand in for the ad and the store
-- (nothing comes from an ad network and nothing is charged), so every flow stays playable.
--
-- The "Ad" overlay entities live in the scene; the App script forwards the overlay's
-- adSkip / adBuy / adCancel actions and calls Ads.update() every frame.
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")
local D = require("scripts.lib.defs")

local Ads = {}

local HOST_TIMEOUT = 90 * 60 -- frames to wait for the host's answer before giving up

local current = nil
local lastBreak = -1e9 -- frame of the last break ad
local gamesSinceBreak = 0
local bridge = rawget(_G, "host") -- nil on players older than the host bridge

-- Asks the host; true when it took the request (its answer will come as `expect`).
local function hosted(name, data, expect, done)
  if not bridge or not bridge.call(name, data) then return false end
  current = { kind = "host", expect = expect, left = HOST_TIMEOUT, done = done }
  Audio.setMuted(true)
  return true
end

local function open(ad)
  current = ad
  UI.show("Ad", true)
  UI.text("Ad:title", ad.title)
  UI.text("Ad:sub", ad.sub or "")
  UI.show("Ad:bar", ad.frames ~= nil)
  UI.show("Ad:skip", ad.kind == "rewarded")
  UI.show("Ad:buy", ad.kind == "purchase")
  UI.show("Ad:cancel", ad.kind == "purchase")
  ad.left = ad.frames
  Audio.setMuted(true)
end

function Ads.close(result)
  local ad = current
  if not ad then return end
  current = nil
  UI.show("Ad", false)
  Audio.setMuted(false)
  if ad.done then ad.done(result) end
end

function Ads.busy()
  return current ~= nil
end

-- done(true) only if the ad was watched to the end. Rewarded ads stay available after
-- "Remove Ads" (the player chooses them), but are then granted at once.
function Ads.rewarded(placement, done)
  if Meta.data.noAds then
    done(true)
    return
  end
  local finish = function(ok)
    if not ok then UI.toast(I.t("toast.noad")) end
    done(ok)
  end
  if hosted("ads.rewarded", { placement = placement }, "ads.result", finish) then return end
  open({ kind = "rewarded", title = I.t("ad.rewarded"), sub = I.t("ad.rewardedSub"), frames = 150, done = finish })
end

function Ads.gameFinished()
  gamesSinceBreak = gamesSinceBreak + 1
end

-- Break ad between games: never in the first games of a session, never more often than
-- D.INTERSTITIAL_GAP seconds, never with Remove Ads. `next` runs when it is over.
function Ads.interstitial(next)
  local now = time.frame()
  if Meta.data.noAds or gamesSinceBreak < D.INTERSTITIAL_GAMES or now - lastBreak < D.INTERSTITIAL_GAP * 60 then
    next()
    return
  end
  lastBreak, gamesSinceBreak = now, 0
  if hosted("ads.interstitial", { placement = "game_over" }, "ads.result", next) then return end
  open({ kind = "interstitial", title = I.t("ad.break"), frames = 100, done = next })
end

-- done(true) when the purchase went through.
function Ads.purchase(product, done)
  if hosted("iap.purchase", { product = product.id }, "iap.result", done) then return end
  if not D.DEMO_STORE then
    UI.toast(I.t("toast.nostore"))
    done(false)
    return
  end
  open({ kind = "purchase", title = I.t("ad.purchase"),
         sub = I.t("ad.purchaseSub", { name = I.L(product.name), price = product.price }), done = done })
end

-- Tells the host the game finished loading (portals count this; ad SDKs preload on it).
function Ads.ready()
  if bridge then bridge.call("game.ready", { version = 1 }) end
end

-- Counts real frames, so it also runs while the game is paused.
function Ads.update()
  if bridge then
    for _, m in ipairs(bridge.poll()) do
      if current and current.kind == "host" and m.name == current.expect then
        Ads.close(type(m.data) == "table" and m.data.ok == true)
      end
    end
  end
  local ad = current
  if not ad or not ad.left then return end
  ad.left = ad.left - 1
  if ad.kind == "host" then
    if ad.left <= 0 then Ads.close(false) end
    return
  end
  UI.set("Ad:bar", { value = 1 - ad.left / ad.frames })
  if ad.left <= 0 then Ads.close(true) end
end

-- Returns true when the action belonged to the overlay.
function Ads.action(a)
  if a == "adSkip" or a == "adCancel" then
    Ads.close(false)
    return true
  elseif a == "adBuy" then
    Ads.close(true)
    return true
  end
  return false
end

return Ads
