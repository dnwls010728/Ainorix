-- Flies a figure-eight around the start position, so the entity's Trail has a path to draw.
-- params: { "width": 1.5, "height": 0.9, "speed": 2.4 }
local Orbit = {}

function Orbit:onStart()
  self.base = self:position()
  self.angle = 0
end

function Orbit:onUpdate(dt)
  self.angle = self.angle + (self.params.speed or 2) * dt
  local width, height = self.params.width or 1, self.params.height or 1
  self:setPosition(self.base.x + math.sin(self.angle) * width, self.base.y + math.sin(self.angle * 2) * height,
    self.base.z + math.cos(self.angle) * width * 0.4)
end

return Orbit
