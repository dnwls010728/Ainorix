-- Lantern flame: the PointLight's intensity wavers around its scene value.
-- params: { "phase": 0 }
local Flicker = {}

function Flicker:onStart()
  self.phase = self.params.phase or 0 -- offset so lanterns do not flicker in step
  self.base = self:get("PointLight").intensity
end

function Flicker:onUpdate(dt)
  local t = time.now() + self.phase
  local wave = 0.06 * math.sin(t * 9.1) + 0.04 * math.sin(t * 17.3) + 0.03 * math.sin(t * 3.7)
  self:set("PointLight", { intensity = self.base * (1 + wave) })
end

-- Sent by game.lua when the quest is done.
function Flicker:brighten()
  self.base = self.base * 1.4
end

return Flicker
