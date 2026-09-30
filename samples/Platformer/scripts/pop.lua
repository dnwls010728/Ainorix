-- Coin popping out of a "?" block: rises, then disappears.
local Pop = {}

function Pop:onStart()
  self.t = 0
end

function Pop:onUpdate(dt)
  self.t = self.t + dt
  self:translate(0, (3.5 - self.t * 10) * dt, 0)
  if self.t > 0.45 then self:destroy() end
end

return Pop
