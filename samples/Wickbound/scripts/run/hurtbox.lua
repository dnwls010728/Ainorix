-- Sensor child of the player: tells the player script which enemies are touching it.
-- Pickups and enemy bullets react to this sensor in their own scripts.
local W = require("scripts.lib.world")

local Hurtbox = {}

function Hurtbox:onTriggerEnter(other)
  if W.enemies[other] and W.player then W.player:touch(other, true) end
end

function Hurtbox:onTriggerExit(other)
  if W.player then W.player:touch(other, false) end
end

return Hurtbox
