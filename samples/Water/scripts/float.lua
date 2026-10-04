-- Rides the sea: follows the wave height at its spot and leans with the wave slope.
-- params: { "lift": 0.1, "tilt": 1, "yaw": 0 }
local Waves = require("scripts.waves")
local Float = {}

function Float:onStart()
  self.base = self:position()
  self.lift = self.params.lift or 0.1 -- height of the origin above the surface
  self.tilt = self.params.tilt or 1   -- 0 = stay upright, 1 = lie on the surface
  self.yaw = self.params.yaw or 0
end

function Float:onUpdate(dt)
  -- The frame is drawn with the time after this step, so sample the waves there.
  local height, slopeX, slopeZ = Waves.sample(self.base.x, self.base.z, time.now() + dt)
  self:set("Transform", {
    position = { self.base.x, self.base.y + height + self.lift, self.base.z },
    -- The surface normal is (-slopeX, 1, -slopeZ): lean around Z for the X slope and around X for the Z slope.
    rotation = { -math.deg(math.atan(slopeZ)) * self.tilt, self.yaw, math.deg(math.atan(slopeX)) * self.tilt },
  })
end

return Float
