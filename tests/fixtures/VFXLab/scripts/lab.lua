-- Space fires the fireworks by hand, on top of their own two-second cycle.
local Lab = {}

function Lab:onUpdate()
  if not input.pressed("Space") then return end
  vfx.burst("Burst", 70)
  vfx.burst("Flash", 1)
  vfx.burst("Puff", 7)
end

return Lab
