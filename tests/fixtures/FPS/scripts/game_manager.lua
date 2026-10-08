-- Round logic: counts targets, runs the clock, shows the results screen.
-- UI entities it drives: TargetsText, TimeText, ResultPanel, ResultText, PlayAgain, Crosshair.
local Game = {}

function Game:onStart()
  self.total = #scene.withTag("target")
  self.left = self.total
  self.time = 0
  self.over = false
  game.set("shots", 0)
  game.set("hits", 0)
  self:hud()
end

function Game:onUpdate(dt)
  if self.over then return end
  self.time = self.time + dt
  scene.set(scene.find("TimeText"), "UIText", { text = string.format("TIME  %.1f", self.time) })

  -- Fell off the map: back to the start.
  local player = scene.find("Player")
  if player and scene.get(player, "Transform").position.y < -10 then
    scene.set(player, "Transform", { position = { x = 0, y = 1.2, z = 12 } })
    scene.set(player, "CharacterBody", { velocity = { x = 0, y = 0, z = 0 } })
  end
end

function Game:onTargetDown(id)
  self.left = self.left - 1
  self:hud()
  if self.left > 0 or self.over then return end

  self.over = true
  audio.play("sounds/clear.wav")
  scene.broadcast("onRoundOver")
  local shots, hits = game.get("shots") or 0, game.get("hits") or 0
  local accuracy = shots > 0 and math.floor(hits / shots * 100 + 0.5) or 0
  scene.set(scene.find("ResultText"), "UIText", {
    text = string.format("RANGE CLEAR!  %.1f s   ACCURACY %d%%", self.time, accuracy),
    visible = true,
  })
  scene.set(scene.find("ResultPanel"), "UIPanel", { visible = true })
  scene.set(scene.find("PlayAgain"), "UIButton", { visible = true })
  scene.set(scene.find("Crosshair"), "UIText", { visible = false })
end

function Game:hud()
  scene.set(scene.find("TargetsText"), "UIText", { text = string.format("TARGETS  %d / %d", self.total - self.left, self.total) })
end

return Game
