-- Spins the entity around Z at a constant speed. params { speed = degrees per second }
local Spin = {}

function Spin:onStart()
  self.speed = self.params.speed or 45
end

function Spin:onUpdate(dt)
  self:rotate(0, 0, self.speed * dt)
end

return Spin
