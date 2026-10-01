-- Level logic: counts the coins (entities tagged "coin"), shows the score,
-- and when all are collected either loads params.nextScene or shows the win
-- screen with the "Play Again" button.
-- params: { "nextScene": "scenes/level2.scene.json" }   ("" = last level)
local Game = {}

function Game:onStart()
  self.total = #scene.withTag("coin")
  self.collected = 0
  self.best = save.get("highScore", 0)
  self.score = scene.find("Score")
  self.message = scene.find("Message")
  self.playAgain = scene.find("Play Again")
  self.player = scene.find("Player")
  if self.player then self.spawn = scene.get(self.player, "Transform").position end
  self:refresh()
end

-- Falling off the level puts the player back at the start.
function Game:onUpdate(dt)
  if not self.player or not scene.exists(self.player) then return end
  local p = scene.get(self.player, "Transform").position
  if p.y < -10 then
    scene.set(self.player, "Transform", { position = self.spawn })
    scene.set(self.player, "CharacterBody", { velocity = { x = 0, y = 0, z = 0 } })
    log.info("player fell off the level: respawned")
  end
end

function Game:refresh()
  if not self.score then return end
  local text = string.format("Coins: %d/%d", self.collected, self.total)
  local total = game.get("total") or 0
  if total > 0 then text = text .. string.format("\nTotal: %d", total) end
  if self.best > 0 then text = text .. string.format("\nBest: %d", self.best) end
  scene.set(self.score, "UIText", { text = text })
end

function Game:onCoinCollected(coin)
  self.collected = self.collected + 1
  game.set("total", (game.get("total") or 0) + 1)
  local total = game.get("total")
  if total > self.best then
    self.best = total
    save.set("highScore", self.best)
    local ok, error = pcall(save.flush)
    if not ok then log.warn("Could not save the high score:", error) end
  end
  self:refresh()
  if self.collected < self.total then return end

  audio.play("sounds/success.wav")
  local nextScene = self.params.nextScene or ""
  if nextScene ~= "" then
    scene.set(self.message, "UIText", { text = "Level clear!", visible = true })
    timer.after(1.5, function() game.loadScene(nextScene) end)
  else
    scene.set(self.message, "UIText", { text = "You win!", visible = true })
    if self.playAgain then scene.set(self.playAgain, "UIButton", { visible = true }) end
  end
end

return Game
