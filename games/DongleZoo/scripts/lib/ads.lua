-- Ads and purchases behind one interface, so the game never knows which platform pays.
-- This build ships the demo provider: overlays that stand in for a rewarded ad, a break ad
-- and the store sheet (nothing is shown from an ad network and nothing is charged). A real
-- provider replaces the three `open` calls below with the platform SDK and calls
-- Ads.close(true/false) when the SDK reports the result; see docs/DONGLEZOO.md.
-- The "Ad" overlay entities live in the scene; the App script forwards the overlay's
-- adSkip / adBuy / adCancel actions and calls Ads.update() every frame.
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")
local D = require("scripts.lib.defs")

local Ads = {}

local current = nil
local lastBreak = -1e9 -- frame of the last break ad
local gamesSinceBreak = 0

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
function Ads.rewarded(done)
  if Meta.data.noAds then
    done(true)
    return
  end
  open({ kind = "rewarded", title = I.t("ad.rewarded"), sub = I.t("ad.rewardedSub"), frames = 150, done = function(ok)
    if not ok then UI.toast(I.t("toast.noad")) end
    done(ok)
  end })
end

-- Break ad between games: never in the first games of a session, never more often than
-- D.INTERSTITIAL_GAP seconds, never with Remove Ads.
function Ads.gameFinished()
  gamesSinceBreak = gamesSinceBreak + 1
end

function Ads.interstitial(next)
  local now = time.frame()
  if Meta.data.noAds or gamesSinceBreak < D.INTERSTITIAL_GAMES or now - lastBreak < D.INTERSTITIAL_GAP * 60 then
    next()
    return
  end
  lastBreak, gamesSinceBreak = now, 0
  open({ kind = "interstitial", title = I.t("ad.break"), frames = 100, done = next })
end

function Ads.purchase(product, done)
  open({ kind = "purchase", title = I.t("ad.purchase"),
         sub = I.t("ad.purchaseSub", { name = I.L(product.name), price = product.price }), done = done })
end

-- Counts real frames, so it also runs while the game is paused.
function Ads.update()
  local ad = current
  if not ad or not ad.frames then return end
  ad.left = ad.left - 1
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
