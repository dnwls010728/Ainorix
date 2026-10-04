-- Menu scene: title, play setup, upgrades, achievements, shop, settings, result and the
-- daily gift. The screens are entity trees in scenes/menu.scene.json ("Screen:<name>");
-- this script shows one at a time and fills in what depends on the save data.
local D = require("scripts.lib.defs")
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")
local Ads = require("scripts.lib.ads")

local Menu = {}

local ICON = "assets/icons/"
local SCREENS = { "title", "play", "upgrades", "ach", "shop", "settings", "result" }
local STATS = { "runs", "wins", "kills", "bossKills", "bestTime", "braziersLit", "glimsEarned", "maxLevel", "evolutions" }
local t, L = I.t, I.L
local fmt = UI.fmt

function Menu:onStart()
  if not Meta.data then Meta.load() end
  local s = Meta.data.settings
  I.setLocale(s.locale)
  Audio.setVolumes(s.music, s.sfx)
  Audio.sceneChanged()
  Audio.music("menu")
  UI.clearToasts()
  UI.localize()
  self.dialog = nil
  self.dirty = false
  -- coming back from a run: show its result
  self.result = game.get("result")
  self.newAch = game.get("newAch") or {}
  self.doubled = false
  game.set("result", nil)
  game.set("newAch", nil)
  self:show(self.result and "result" or "title")
end

function Menu:show(name)
  self.screen = name
  for _, sname in ipairs(SCREENS) do UI.show("Screen:" .. sname, sname == name) end
  self:refresh()
end

function Menu:openDialog(name)
  self:closeDialog()
  self.dialog = name
  UI.show("Dlg:" .. name, true)
end

function Menu:closeDialog()
  if self.dialog then UI.show("Dlg:" .. self.dialog, false) end
  self.dialog = nil
end

-- ---------------------------------------------------------------- screens

local function setCard(name, selected, unlocked, lockColor)
  UI.set(name, {
    color = UI.rgb(selected and "#fff3c4" or unlocked and "#fff8ec" or "#e6e0f3"),
    borderColor = UI.rgb(selected and "#ffcf5c" or unlocked and "#ffe9c0" or (lockColor or "#ffe9c0")),
  })
end

function Menu:refreshPlay()
  local d = Meta.data
  for _, k in ipairs(D.KEEPERS) do
    local name = "Keeper:" .. k.id
    local unlocked = Meta.keeperUnlocked(k.id)
    local sel = unlocked and d.selectedKeeper == k.id
    setCard(name, sel, unlocked, k.color)
    UI.set(name .. ":img", { opacity = unlocked and 1 or 0.55 })
    UI.text(name .. ":name", L(k.name))
    UI.text(name .. ":desc", L(k.desc))
    UI.show(name .. ":tag", sel)
    UI.show(name .. ":unlock", not unlocked)
    UI.text(name .. ":unlock", t("play.unlock") .. " · " .. fmt(k.cost))
  end
  for _, s in ipairs(D.STAGES) do
    local name = "Stage:" .. s.id
    local unlocked = Meta.stageUnlocked(s.id)
    local sel = unlocked and d.selectedStage == s.id
    setCard(name, sel, unlocked)
    UI.set(name .. ":img", { opacity = unlocked and 1 or 0.6 })
    UI.text(name .. ":name", L(s.name))
    UI.text(name .. ":desc", L(s.desc))
    local tag = ""
    if sel then tag = t("play.selected") elseif not unlocked then tag = t("play.winReq", { stage = L(D.stageById(s.requires).name) }) end
    UI.text(name .. ":tag", tag)
  end
  UI.set("Start", { interactable = Meta.keeperUnlocked(d.selectedKeeper) and Meta.stageUnlocked(d.selectedStage) })
end

function Menu:refreshUpgrades()
  for _, m in ipairs(D.META) do
    local name = "Meta:" .. m.id
    local cost = Meta.metaCost(m.id)
    UI.set(name, { color = UI.rgb(cost and "#fff8ec" or "#fff3c4"), borderColor = UI.rgb(cost and "#ffe9c0" or "#ffcf5c") })
    UI.text(name .. ":name", L(m.name))
    UI.text(name .. ":desc", L(m.desc))
    UI.pips(name .. ":pip", Meta.level(m.id), m.max)
    UI.show(name .. ":buy", cost ~= nil)
    UI.show(name .. ":max", cost == nil)
    if cost then
      local can = Meta.data.glims >= cost
      UI.set(name .. ":buy", { text = t("upgrades.buy") .. " · " .. fmt(cost), color = UI.rgb(can and "#ffb59c" or "#e9e2d2"),
        textColor = UI.rgb(can and "#5a2a1a" or "#8d7a66") })
    end
  end
end

function Menu:refreshAch()
  for i, a in ipairs(D.ACHIEVEMENTS) do
    local name = "Ach" .. i
    local got = Meta.data.achievements[a.id] == true
    UI.set(name, { color = UI.rgb(got and "#fff3c4" or "#fff8ec"), borderColor = UI.rgb(got and "#ffcf5c" or "#ffe9c0") })
    UI.set(name .. ":icon", { texture = ICON .. (got and "1f3c6" or "1f512") .. ".png" })
    UI.text(name .. ":name", L(a.name))
    UI.text(name .. ":desc", L(a.desc))
    UI.text(name .. ":reward", t("ach.reward", { n = fmt(a.reward) }))
  end
  local st = Meta.data.stats
  for _, key in ipairs(STATS) do
    UI.text("Stat:" .. key, key == "bestTime" and UI.fmtTime(st[key]) or fmt(st[key]))
  end
end

function Menu:refreshShop()
  for _, p in ipairs(D.IAP_PRODUCTS) do
    local name = "Iap:" .. p.id
    local owned = p.id == "no_ads" and Meta.data.noAds
    UI.text(name .. ":name", L(p.name))
    UI.text(name .. ":desc", L(p.desc))
    UI.text(name .. ":buy", p.priceLabel)
    UI.show(name .. ":buy", not owned)
    UI.show(name .. ":owned", owned)
  end
end

function Menu:refreshSettings()
  local s = Meta.data.settings
  UI.set("Set:Music", { value = s.music })
  UI.set("Set:Sfx", { value = s.sfx })
  UI.text("Set:Music:label", math.floor(s.music * 100 + 0.5) .. "%")
  UI.text("Set:Sfx:label", math.floor(s.sfx * 100 + 0.5) .. "%")
  UI.set("Set:Shake", { text = t(s.shake and "common.on" or "common.off"), color = UI.rgb(s.shake and "#8fe3c4" or "#e9e2d2") })
  UI.set("Set:Damage", { text = t(s.damageNumbers and "common.on" or "common.off"), color = UI.rgb(s.damageNumbers and "#8fe3c4" or "#e9e2d2") })
  UI.set("Set:en", { color = UI.rgb(s.locale == "en" and "#ffd56b" or "#e9e2d2") })
  UI.set("Set:ko", { color = UI.rgb(s.locale == "ko" and "#ffd56b" or "#e9e2d2") })
end

function Menu:refreshResult()
  local r = self.result
  if not r then return end
  UI.set("Result:title", { text = t(r.victory and "result.victory" or "result.defeat"), color = UI.rgb(r.victory and "#ffcf5c" or "#ffa98f") })
  UI.text("Result:sub", t(r.victory and "result.victorySub" or "result.defeatSub"))
  UI.text("Result:time", UI.fmtTime(r.time))
  UI.text("Result:kills", fmt(r.kills))
  UI.text("Result:level", fmt(r.level))
  UI.text("Result:braziers", fmt(r.braziersLit))
  UI.text("Result:glims", fmt(self.doubled and r.glimsEarned * 2 or r.glimsEarned))
  UI.show("Result:double", not self.doubled)
  UI.set("Result:double", { interactable = r.glimsEarned > 0 })
  UI.show("Result:doubled", self.doubled)

  local entries = {}
  for id, v in pairs(r.damageByWeapon or {}) do
    if v > 0 then entries[#entries + 1] = { id, v } end
  end
  table.sort(entries, function(a, b)
    if a[2] ~= b[2] then return a[2] > b[2] end
    return a[1] < b[1]
  end)
  for i = 1, 9 do
    local e = entries[i]
    UI.show("Dmg" .. i, e ~= nil)
    if e then
      local info = D.weaponById(e[1])
      UI.show("Dmg" .. i .. ":icon", info ~= nil)
      if info then UI.set("Dmg" .. i .. ":icon", { texture = ICON .. info.icon .. ".png" }) end
      UI.text("Dmg" .. i .. ":name", info and L(info.name) or e[1])
      UI.set("Dmg" .. i .. ":bar", { value = math.max(0.02, e[2] / entries[1][2]), fillColor = UI.rgb(info and info.color or "#ffb347") })
      UI.text("Dmg" .. i .. ":value", fmt(e[2]))
    end
  end
  UI.show("Result:damage", #entries > 0)
  UI.show("Result:ach", #self.newAch > 0)
  for i = 1, 5 do
    local id = self.newAch[i]
    UI.show("NewAch" .. i, id ~= nil)
    if id then
      for _, a in ipairs(D.ACHIEVEMENTS) do
        if a.id == id then
          UI.text("NewAch" .. i .. ":name", L(a.name))
          UI.text("NewAch" .. i .. ":desc", L(a.desc))
          UI.text("NewAch" .. i .. ":reward", t("ach.reward", { n = fmt(a.reward) }))
        end
      end
    end
  end
end

function Menu:refresh()
  local s = self.screen
  if s == "title" then
    UI.show("Daily badge", Meta.dailyAvailable())
  elseif s == "play" then
    self:refreshPlay()
  elseif s == "upgrades" then
    self:refreshUpgrades()
  elseif s == "ach" then
    self:refreshAch()
  elseif s == "shop" then
    self:refreshShop()
  elseif s == "settings" then
    self:refreshSettings()
  elseif s == "result" then
    self:refreshResult()
  end
  self.glims = nil -- redraw the Glim chips
end

-- ---------------------------------------------------------------- actions (from scripts/ui/button.lua)

function Menu:startRun()
  local d = Meta.data
  game.set("run", { keeperId = d.selectedKeeper, stageId = d.selectedStage })
  game.loadScene("scenes/run.scene.json")
end

function Menu:onAction(a, b)
  if Ads.action(a) then return end
  local s = Meta.data.settings
  if a ~= "setMusic" and a ~= "setSfx" then Audio.sfx("click") end

  if a == "nav" then
    self:show(b)
  elseif a == "selKeeper" then
    Meta.selectKeeper(b)
    self:refresh()
  elseif a == "unlockKeeper" then
    local ok = Meta.unlockKeeper(b)
    Audio.sfx(ok and "buy" or "deny")
    if not ok then UI.toast(t("toast.poor"), 1.8) end
    self:refresh()
  elseif a == "selStage" then
    if Meta.stageUnlocked(b) then Meta.selectStage(b) else Audio.sfx("deny") end
    self:refresh()
  elseif a == "start" then
    self:startRun()
  elseif a == "buyMeta" then
    Audio.sfx(Meta.buyMeta(b) and "buy" or "deny")
    self:refresh()
  elseif a == "buyIap" then
    local product
    for _, p in ipairs(D.IAP_PRODUCTS) do
      if p.id == b then product = p end
    end
    if product and not Ads.busy() then
      Ads.purchase(L(product.name), product.priceLabel, function(ok)
        if ok then
          Meta.purchaseApplied(b)
          Audio.sfx("buy")
          UI.toast(t("shop.thanks"), 1.8)
        else
          UI.toast(t("shop.failed"), 1.8)
        end
        self:refresh()
      end)
    end
  elseif a == "setMusic" or a == "setSfx" then
    if a == "setMusic" then s.music = b else s.sfx = b end
    Audio.setVolumes(s.music, s.sfx)
    self.dirty = true
    self:refreshSettings()
  elseif a == "toggleShake" then
    s.shake = not s.shake
    Meta.save()
    self:refresh()
  elseif a == "toggleDamage" then
    s.damageNumbers = not s.damageNumbers
    Meta.save()
    self:refresh()
  elseif a == "lang" then
    s.locale = b
    I.setLocale(b)
    Meta.save()
    UI.localize()
    self:refresh()
  elseif a == "resetAsk" then
    self:openDialog("reset")
  elseif a == "resetYes" then
    self:closeDialog()
    Meta.reset()
    self:show("title")
  elseif a == "closeTop" then
    self:closeDialog()
  elseif a == "dailyOpen" then
    UI.text("Daily:amount", "+" .. fmt(Meta.dailyAmount()))
    self:openDialog("daily")
  elseif a == "dailyClaim" or a == "dailyClaim2" then
    local claim = function(doubled)
      local n = Meta.claimDaily(doubled)
      self:closeDialog()
      if n > 0 then
        Audio.sfx("buy")
        UI.toast(t("toast.claimed", { n = fmt(n) }), 1.8)
      end
      self:refresh()
    end
    if a == "dailyClaim" then
      claim(false)
    elseif not Ads.busy() then
      Ads.rewarded(function(ok)
        if ok then claim(true) end
      end)
    end
  elseif a == "double" then
    if self.result and not self.doubled and not Ads.busy() then
      Ads.rewarded(function(ok)
        if ok and not self.doubled then
          self.doubled = true
          Meta.grantGlims(self.result.glimsEarned)
          Audio.sfx("buy")
          self:refresh()
        end
      end)
    end
  elseif a == "again" then
    Ads.interstitial(function() self:startRun() end)
  elseif a == "resultUpgrades" then
    Ads.interstitial(function() self:show("upgrades") end)
  elseif a == "resultMenu" then
    Ads.interstitial(function() self:show("title") end)
  end
end

function Menu:onUpdate()
  Ads.update()
  Audio.update(1 / 60)
  UI.updateToasts()
  if self.dirty and not input.down("MouseLeft") then -- a volume slider was released
    self.dirty = false
    Meta.save()
    Audio.sfx("click")
  end
  local glims = fmt(Meta.data.glims)
  if glims ~= self.glims then
    self.glims = glims
    for _, id in ipairs(scene.all("UIText")) do
      if scene.name(id) == "Glims" then scene.set(id, "UIText", { text = glims }) end
    end
  end
end

return Menu
