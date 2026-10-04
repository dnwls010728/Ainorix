-- In-run dialogs: level-up, chest, pause (with settings and quit confirmation), revive
-- prompt, victory. The game is paused (game.pause) while one is open; this script keeps
-- running because scripts and UI stay alive during a pause.
local D = require("scripts.lib.defs")
local W = require("scripts.lib.world")
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")
local Ads = require("scripts.lib.ads")

local Dialogs = {}

local ICON = "assets/icons/"
local t, L = I.t, I.L

function Dialogs:onStart()
  self.stack = {} -- open dialog names, top last
  self.revivedByAd = false
  self.dirty = false
end

-- ---------------------------------------------------------------- stack

function Dialogs:top()
  return self.stack[#self.stack]
end

function Dialogs:open(name, replace)
  if replace then self:closeAll() end
  local below = self:top()
  if below then UI.show("Dlg:" .. below, false) end
  self.stack[#self.stack + 1] = name
  UI.show("Dlg:" .. name, true)
end

function Dialogs:closeTop()
  local name = table.remove(self.stack)
  if name then UI.show("Dlg:" .. name, false) end
  local below = self:top()
  if below then UI.show("Dlg:" .. below, true) end
end

function Dialogs:closeAll()
  for _, name in ipairs(self.stack) do UI.show("Dlg:" .. name, false) end
  self.stack = {}
end

-- ---------------------------------------------------------------- content

local function choiceIcon(c)
  if c.kind == "heal" then return "2764" end
  if c.kind == "glims" then return "1f4b0" end
  local info = D.weaponById(c.id) or D.passiveById(c.id)
  return info and info.icon or "2728"
end

local function badge(prefix, c)
  local text, bg, fg
  if c.kind == "evolve" then
    text, bg, fg = t("levelup.evolve"), "#ffcf5c", "#5a3a12"
  elseif c.kind == "heal" or c.kind == "glims" then
    text, bg, fg = t("kind." .. c.kind), "#c9b6ff", "#3e2f78"
  elseif c.isNew then
    text, bg, fg = t("levelup.new"), "#8fe3c4", "#1f5a46"
  else
    text, bg, fg = t("levelup.lv", { n = c.level }), "#c9b6ff", "#3e2f78"
  end
  UI.set(prefix .. ":badge", { color = UI.rgb(bg), width = UI.textWidth(text, 14) + 22 })
  UI.set(prefix .. ":badge:text", { text = text, color = UI.rgb(fg) })
end

local function fillCard(prefix, c)
  local evo = c.kind == "evolve"
  UI.set(prefix, { color = UI.rgb(evo and "#fff3c4" or "#fff8ec"), borderColor = UI.rgb(evo and "#ffcf5c" or "#ffe9c0") })
  UI.set(prefix .. ":icon", { texture = ICON .. choiceIcon(c) .. ".png" })
  UI.text(prefix .. ":name", L(c.name))
  UI.text(prefix .. ":desc", L(c.desc))
end

function Dialogs:showLevelup(choices)
  self.choices = choices
  for i = 1, 3 do
    local c = choices[i]
    UI.show("Choice" .. i, c ~= nil)
    if c then
      fillCard("Choice" .. i, c)
      badge("Choice" .. i, c)
    end
  end
  local rerolls = W.game.rerolls
  UI.set("Reroll", { text = rerolls > 0 and t("levelup.reroll", { n = rerolls }) or t("levelup.rerollAd"),
    color = UI.rgb(rerolls > 0 and "#ece4ff" or "#ffb59c") })
  self:open("levelup", true)
end

function Dialogs:showChest(rewards)
  for i = 1, 4 do
    local c = rewards[i]
    UI.show("Reward" .. i, c ~= nil)
    if c then
      fillCard("Reward" .. i, c)
      local tagged = c.kind == "evolve" or c.kind == "weapon" or c.kind == "passive"
      UI.show("Reward" .. i .. ":badge", tagged)
      if tagged then badge("Reward" .. i, c) end
    end
  end
  self:open("chest", true)
end

function Dialogs:showDying(canRevive)
  UI.show("Dying:revive", canRevive)
  UI.show("Dying:reviveAd", not canRevive and not self.revivedByAd)
  self:open("dying", true)
end

function Dialogs:showVictory()
  self:open("victory", true)
end

local function fillBuild(column, items)
  UI.show("Build:" .. column .. ":empty", #items == 0)
  for i = 1, 6 do
    local it = items[i]
    local name = "Build:" .. column .. i
    UI.show(name, it ~= nil)
    if it then
      local info = D.weaponById(it.id) or D.passiveById(it.id)
      UI.set(name, { color = UI.rgb(it.evolved and "#fff3c4" or "#ffefd2") })
      UI.set(name .. ":icon", { texture = ICON .. info.icon .. ".png" })
      UI.text(name .. ":name", L(it.evolved and info.evoName or info.name))
      UI.pips(name .. ":pip", it.level, 5)
    end
  end
end

function Dialogs:openPause()
  if not W.game:pause() then return end
  fillBuild("Weapons", W.game.weapons)
  fillBuild("Passives", W.game.passives)
  self:open("pause", true)
end

function Dialogs:fillSettings()
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

-- ---------------------------------------------------------------- actions (from scripts/ui/button.lua)

function Dialogs:onAction(a, b)
  if Ads.action(a) then return end
  local g = W.game
  local s = Meta.data.settings
  if a ~= "setMusic" and a ~= "setSfx" then Audio.sfx("click") end

  if a == "pick" then
    if self:top() == "levelup" and self.choices[b] then
      self:closeAll()
      g:choose(b)
    end
  elseif a == "reroll" then
    if g.rerolls > 0 then
      g:reroll(false)
    elseif not Ads.busy() then
      Ads.rewarded(function(ok)
        if ok then g:reroll(true) end
      end)
    end
  elseif a == "chestClose" then
    self:closeAll()
    g:resumePlay()
  elseif a == "pause" then
    self:openPause()
  elseif a == "resume" then
    self:closeAll()
    g:resumePlay()
  elseif a == "pauseSettings" then
    self:fillSettings()
    self:open("settings")
  elseif a == "quitAsk" then
    self:open("quit")
  elseif a == "quitYes" or a == "giveUp" then
    self:closeAll()
    g:giveUp()
  elseif a == "closeTop" then
    self:closeTop()
  elseif a == "revive" then
    self:closeAll()
    g:revive()
  elseif a == "reviveAd" then
    if not Ads.busy() then
      Ads.rewarded(function(ok)
        if ok then
          self.revivedByAd = true
          self:closeAll()
          g:revive()
        end
      end)
    end
  elseif a == "endless" then
    self:closeAll()
    g:continueEndless()
  elseif a == "finish" then
    self:closeAll()
    g:finish(true)
  elseif a == "setMusic" or a == "setSfx" then
    if a == "setMusic" then s.music = b else s.sfx = b end
    Audio.setVolumes(s.music, s.sfx)
    self.dirty = true
    self:fillSettings()
  elseif a == "toggleShake" then
    s.shake = not s.shake
    Meta.save()
    self:fillSettings()
  elseif a == "toggleDamage" then
    s.damageNumbers = not s.damageNumbers
    Meta.save()
    self:fillSettings()
  elseif a == "lang" then
    s.locale = b
    I.setLocale(b)
    Meta.save()
    UI.localize()
    self:fillSettings()
    W.hud.cache = {}
  elseif a == "resetAsk" then
    self:open("reset")
  elseif a == "resetYes" then
    Meta.reset()
    self:closeTop()
  end
end

function Dialogs:onUpdate()
  Ads.update()
  if self.dirty and not input.down("MouseLeft") then -- a volume slider was released
    self.dirty = false
    Meta.save()
    Audio.sfx("click")
  end
  if Ads.busy() then return end
  local top = self:top()
  if top == "levelup" then
    for i = 1, 3 do
      if input.pressed(tostring(i)) then
        self:onAction("pick", i)
        return
      end
    end
  end
  if input.pressed("Escape") or input.pressed("P") then
    if top == "pause" then
      self:onAction("resume")
    elseif top == "settings" or top == "quit" or top == "reset" then
      self:closeTop()
    elseif not top then
      self:openPause()
    end
  end
end

return Dialogs
