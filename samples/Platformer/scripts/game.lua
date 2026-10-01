-- Lives, coins, clock, respawning and the end screens.
local Game = {}

function Game:onStart()
  self.player = scene.find("Player")
  self.coins, self.lives, self.time = 0, 3, 0
  self.over = false
  game.set("coins", 0)
  self:respawn()
  self:hud()
end

function Game:respawn()
  local spawn = game.get("spawn")
  if spawn and self.player then scene.send(self.player, "respawn", spawn) end
end

function Game:onUpdate(dt)
  if input.pressed("R") or input.pressed("GamepadStart") then return game.loadScene(game.scene()) end
  if self.over then return end
  self.time = self.time + dt
  scene.set(scene.find("TimeText"), "UIText", { text = string.format("%.1f", self.time) })
end

-- From coins (pos = nil) and "?" blocks (pos = where the coin pops out).
function Game:onCoin(pos)
  self.coins = self.coins + 1
  game.set("coins", self.coins)
  if pos then
    scene.create("Coin Pop", {
      Transform = { position = { pos.x, pos.y, 0.1 } },
      Sprite = { texture = "assets/sprites/coin.png", columns = 4, order = 4 },
      SpriteAnimation = { clip = "spin", clips = { spin = { frames = { 0, 1, 2, 3 }, fps = 16 } } },
      Script = { path = "scripts/pop.lua" },
    })
  end
  self:hud()
end

function Game:onPlayerDied()
  if self.over then return end
  self.lives = self.lives - 1
  self:hud()
  if self.lives > 0 then
    timer.after(1.0, function() self:respawn() end)
  else
    self:finish("GAME OVER", false)
  end
end

function Game:onGoal()
  if self.over then return end
  audio.play("sounds/clear.wav")
  self:finish(string.format("LEVEL CLEAR!\n<color=#ffffff>%d COINS  %.1f s</color>", self.coins, self.time), true)
end

function Game:finish(text, won)
  self.over = true
  if self.player then scene.send(self.player, "freeze") end
  scene.set(scene.find("Message"), "UIText", { text = text, visible = true, color = won and { 1, 0.9, 0.3 } or { 1, 0.45, 0.4 } })
  scene.set(scene.find("MessagePanel"), "UIPanel", { visible = true })
  scene.set(scene.find("PlayAgain"), "UIButton", { visible = true })
end

function Game:hud()
  scene.set(scene.find("CoinsText"), "UIText", { text = "COINS " .. self.coins })
  scene.set(scene.find("LivesText"), "UIText", { text = "LIVES " .. self.lives })
end

return Game
