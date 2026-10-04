-- Mock platform provider, like the web build's: demo overlays, no real ads or payments.
-- The "Ad" overlay entities exist in both scenes; the owning script forwards its
-- adSkip / adBuy / adCancel actions and calls Ads.update() every frame.
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")

local Ads = {}

local current = nil
local lastInterstitial = -1e9

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

-- done(true) only if the "ad" was watched to the end.
function Ads.rewarded(done)
  if Meta.data.noAds then
    done(true)
    return
  end
  open({ kind = "rewarded", title = I.t("ad.rewarded"), sub = I.t("ad.rewardedSub"), frames = 180, done = function(ok)
    if not ok then UI.toast(I.t("toast.noAd")) end
    done(ok)
  end })
end

-- Interstitial at natural breaks: at most one per 3 minutes, never with No Ads.
function Ads.interstitial(next)
  local now = time.frame()
  if Meta.data.noAds or now - lastInterstitial < 180 * 60 then
    next()
    return
  end
  lastInterstitial = now
  open({ kind = "interstitial", title = I.t("ad.break"), frames = 120, done = next })
end

function Ads.purchase(name, price, done)
  open({ kind = "purchase", title = I.t("ad.purchase"), sub = I.t("ad.purchaseSub", { name = name, price = price }), done = done })
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
