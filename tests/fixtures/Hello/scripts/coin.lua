-- Collectible coin: spins; when something tagged "player" touches it, plays a
-- sound, tells every script "onCoinCollected" and removes itself.
-- Needs a Collider with isTrigger = true (see prefabs/coin.prefab.json).
local Coin = {}

function Coin:onUpdate(dt)
  self:rotate(0, 180 * dt, 0)
end

function Coin:onTriggerEnter(other)
  local tag = scene.get(other, "Tag")
  if tag and string.find(tag.tags, "player", 1, true) then
    audio.play("sounds/coin.wav")
    scene.broadcast("onCoinCollected", self.id)
    self:destroy()
  end
end

return Coin
