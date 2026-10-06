-- One attached script owns the run; stateless modules implement each responsibility.
local D = require("scripts.lib.defs")
local I = require("scripts.lib.i18n")
local UI = require("scripts.lib.ui")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")
local Ads = require("scripts.lib.ads")
local Board = require("scripts.lib.board")
local Scoring = require("scripts.app.scoring")
local App = {}

-- Fixed method installation order; modules have no service imports or mutable state.
for _, methods in ipairs({
  require("scripts.app.screens"),
  require("scripts.app.gameplay"),
  require("scripts.app.effects"),
  require("scripts.app.rewards"),
}) do
  for _, name in ipairs(methods.methods) do App[name] = methods[name] end
end

function App:onStart()
  self.services = { D = D, I = I, UI = UI, Meta = Meta, Audio = Audio, Ads = Ads, Board = Board, Scoring = Scoring }
  Meta.load()
  if Meta.data.settings.locale == "" then
    -- first launch: follow the device language (players older than the host bridge start in English)
    local bridge = rawget(_G, "host")
    local lang = self.params.locale or (bridge and bridge.language()) or "en"
    Meta.data.settings.locale = lang == "ko" and "ko" or "en"
  end
  I.setLocale(Meta.data.settings.locale)
  Audio.configure(Meta.data.settings.sfx, Meta.data.settings.music)

  self.camera = scene.find("Camera")
  self.canvas = scene.find("Canvas")
  self.background = scene.find("Background")
  self.holder = scene.find("Holder")
  self.guide = scene.find("Guide")
  self.line = scene.find("Line")
  self.mode = "home" -- home | play | over
  self.dialog = nil
  self.score, self.combo, self.comboLeft = 0, 0, 0
  self.fx = {} -- { id, frames left }
  self.pop = 0
  self.frame = 0
  self.vw, self.vh = 0, 0
  Board.clear()

  UI.localize()
  self:layout()
  self:applyTheme()
  self:showHome()
  Ads.ready()
end

function App:onAction(a, b)
  if Ads.action(a) then return end
  if Ads.busy() then return end
  Audio.sfx("click")
  if self:handleGameplayAction(a, b) then return end
  if self:handleScreensAction(a, b) then return end
  self:handleRewardsAction(a, b)
end

function App:onUpdate(dt)
  self.frame = self.frame + 1
  self:layout()
  Ads.update()
  UI.updateToasts()
  self:updateFx()

  if input.pressed("Escape") and not Ads.busy() then
    if self.dialog and self.dialog ~= "over" then
      self:close()
    elseif self.mode == "play" then
      self:open("pause")
    end
  end

  if self.mode == "play" then
    if not self.dialog and not Ads.busy() and dt > 0 then self:updatePlay(dt) end
  elseif self.mode == "over" and self.overFrames then
    Board.step(dt, function(tier, x, y, newId) self:onMerge(tier, x, y, newId) end)
    self.overFrames = self.overFrames - 1
    if self.overFrames <= 0 then
      self.overFrames = nil
      self:showOver()
    end
  end
end

return App
