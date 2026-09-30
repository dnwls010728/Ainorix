-- Hearts, coins, clock and the end screens. R restarts.
local Game = {}

function Game:onStart()
  self.player = scene.find("Player")
  self.coins, self.hp, self.time = 0, 3, 0
  self.over = false
  self:hud()
end

function Game:onUpdate(dt)
  if input.pressed("R") then return game.loadScene(game.scene()) end
  if self.over then return end
  self.time = self.time + dt
  if self.noteTimer then
    self.noteTimer = self.noteTimer - dt
    if self.noteTimer <= 0 then
      self.noteTimer = nil
      scene.set(scene.find("Note"), "UIText", { visible = false })
    end
  end
end

function Game:onCoin()
  self.coins = self.coins + 1
  self:hud()
end

function Game:onPlayerHurt()
  if self.over then return end
  self.hp = self.hp - 1
  self:hud()
  if self.hp <= 0 then self:finish("GAME OVER", false) end
end

function Game:onExit()
  if self.over then return end
  local total = game.get("coinsTotal") or 0
  if self.coins < total then
    scene.set(scene.find("Note"), "UIText", { text = string.format("%d COINS LEFT", total - self.coins), visible = true })
    self.noteTimer = 2
    return
  end
  audio.play("sounds/clear.wav")
  self:finish(string.format("ESCAPED!\n<color=#ffffff>%d COINS  %.1f s</color>", self.coins, self.time), true)
end

function Game:finish(text, won)
  self.over = true
  if self.player then scene.send(self.player, "freeze") end
  scene.set(scene.find("Message"), "UIText", { text = text, visible = true, color = won and { 1, 0.9, 0.3 } or { 1, 0.45, 0.4 } })
  scene.set(scene.find("MessagePanel"), "UIPanel", { visible = true })
  scene.set(scene.find("PlayAgain"), "UIButton", { visible = true })
end

function Game:hud()
  local total = game.get("coinsTotal") or 0
  scene.set(scene.find("CoinsText"), "UIText", { text = string.format("COINS %d/%d", self.coins, total) })
  for i = 1, 3 do
    scene.set(scene.find("Heart" .. i), "UIImage", { frame = i <= self.hp and 0 or 1 })
  end
end

return Game
