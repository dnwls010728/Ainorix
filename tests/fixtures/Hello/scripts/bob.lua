-- Bobs the entity up and down while spinning it.
-- params: { "height": 0.4, "speed": 2 }
local Bob = {}

function Bob:onStart()
  self.baseY = self:position().y
end

function Bob:onUpdate(dt)
  local height = self.params.height or 0.4
  local speed = self.params.speed or 2
  local p = self:position()
  self:setPosition(p.x, self.baseY + math.sin(time.now() * speed) * height, p.z)
  self:rotate(0, 90 * dt, 0)
end

return Bob
