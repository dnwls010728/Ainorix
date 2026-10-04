-- Spins the entity at a constant angular speed.
-- params: { "degreesPerSecond": [x, y, z] }   (default: 45 deg/s around Y)
local Rotator = {}

function Rotator:onStart()
  local s = self.params.degreesPerSecond or { 0, 45, 0 }
  self.speed = { x = s[1] or s.x or 0, y = s[2] or s.y or 0, z = s[3] or s.z or 0 }
end

function Rotator:onUpdate(dt)
  local r = self:get("Transform").rotation
  self:set("Transform", { rotation = {
    x = (r.x + self.speed.x * dt) % 360,
    y = (r.y + self.speed.y * dt) % 360,
    z = (r.z + self.speed.z * dt) % 360,
  } })
end

return Rotator
