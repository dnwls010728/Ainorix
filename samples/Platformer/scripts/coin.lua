-- Collectible coin (trigger). The spin comes from its SpriteAnimation.
local Coin = {}

function Coin:onTriggerEnter(other)
  local tag = scene.get(other, "Tag")
  if tag and string.find(tag.tags, "player", 1, true) then
    audio.play("sounds/coin.wav", { volume = 0.6 })
    require("scripts.effects").spawn(self:position(), "coin")
    scene.broadcast("onCoin")
    self:destroy()
  end
end

return Coin
