-- One-shot star puff: tints the ParticleEmitter, bursts once and removes itself.
-- params { color = "#rrggbb", n = particles, speed = units per second }
local Burst = {}

local function rgb(hex)
  return { tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255 }
end

function Burst:onStart()
  local color = rgb(self.params.color or "#ffffff") -- tint
  local speed = self.params.speed or 4 -- fastest particle, units per second
  self:set("ParticleEmitter", { startColor = color, endColor = color, speed = speed })
  self:burst(self.params.n or 8)
  self.life = 1.2
end

function Burst:onUpdate(dt)
  self.life = self.life - dt
  if self.life <= 0 then self:destroy() end
end

return Burst
