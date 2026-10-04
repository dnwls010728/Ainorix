-- Space fires the fireworks by hand, on top of their own two-second cycle.
local Lab = {}

function Lab:onUpdate()
  if not input.pressed("Space") then return end
  particles.burst("Burst", 70)
  particles.burst("Flash", 1)
  particles.burst("Puff", 7)
end

return Lab
