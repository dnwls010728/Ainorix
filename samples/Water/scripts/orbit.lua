-- Orbit camera: A / D (or arrows) turn around the scene, W / S zoom, Q / E change the height.
-- It writes CameraFollow.offset; the component keeps the camera looking at its target.
local Orbit = {}

function Orbit:onStart()
  local offset = self:get("CameraFollow").offset
  self.angle = math.atan(offset.x, offset.z)
  self.distance = math.sqrt(offset.x * offset.x + offset.z * offset.z)
  self.height = offset.y
end

local function axis(positive, negative, positiveAlt, negativeAlt)
  local value = 0
  if input.down(positive) or (positiveAlt and input.down(positiveAlt)) then value = value + 1 end
  if input.down(negative) or (negativeAlt and input.down(negativeAlt)) then value = value - 1 end
  return value
end

function Orbit:onUpdate(dt)
  self.angle = self.angle + axis("D", "A", "Right", "Left") * dt * 0.9
  self.distance = math.max(6, math.min(20, self.distance - axis("W", "S", "Up", "Down") * dt * 6))
  self.height = math.max(1.2, math.min(12, self.height + axis("E", "Q") * dt * 4))
  self:set("CameraFollow", { offset = {
    math.sin(self.angle) * self.distance, self.height, math.cos(self.angle) * self.distance } })
end

return Orbit
