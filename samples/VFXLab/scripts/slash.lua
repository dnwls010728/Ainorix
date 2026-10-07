-- Repeated sword sweeps. Presentation remains project code, not a new engine behavior.
local Slash = {}
function Slash:onStart()
  self.elapsed = 0
  self.cycle = -1
end
function Slash:onUpdate(dt)
  self.elapsed = self.elapsed + dt
  local cycle = math.floor(self.elapsed / 0.8)
  local t = self.elapsed - cycle * 0.8
  if cycle ~= self.cycle then
    self.cycle = cycle
    vfx.play(self.id)
  end
  self:set("Transform", {
    position = { -0.7 + math.min(t / 0.2, 1) * 1.4, -0.5, 0 },
    rotation = { 0, 0, -65 + math.min(t / 0.2, 1) * 130 }
  })
end
return Slash
