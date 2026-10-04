-- Screen shake on top of CameraFollow: the follow offset jitters and settles.
local W = require("scripts.lib.world")
local Meta = require("scripts.lib.meta")

local Camera = {}

function Camera:onStart()
  W.camera = self
  self.amount = 0
end

-- amount in reference pixels, like the original
function Camera:shake(amount)
  if Meta.data.settings.shake then self.amount = math.min(18, math.max(self.amount, amount)) end
end

function Camera:onUpdate(dt)
  if dt == 0 then return end
  if self.amount > 0.2 then
    local k = self.amount * W.U / 1.24
    self:set("CameraFollow", { offset = { x = (math.random() - 0.5) * k, y = (math.random() - 0.5) * k, z = 20 } })
    self.amount = self.amount * math.max(0, 1 - dt * 9)
    self.shaking = true
  elseif self.shaking then
    self.shaking = false
    self:set("CameraFollow", { offset = { x = 0, y = 0, z = 20 } })
  end
end

return Camera
