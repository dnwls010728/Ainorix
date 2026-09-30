-- On the camera: background layers follow the camera partly, so distant
-- hills and clouds scroll slower than the level (depth illusion).
local Parallax = {}

local FACTORS = { hills = 0.6, clouds = 0.85 }  -- 1 = glued to the camera, 0 = part of the level

function Parallax:onStart()
  local p = self:position()
  self.origin = { x = p.x, y = p.y }
  self.layers = {}
  for layer, factor in pairs(FACTORS) do
    for _, id in ipairs(scene.withTag(layer)) do
      local b = scene.get(id, "Transform").position
      table.insert(self.layers, { id = id, x = b.x, y = b.y, z = b.z, f = factor })
    end
  end
  table.sort(self.layers, function(a, b) return a.id < b.id end)
end

function Parallax:onUpdate(dt)
  local p = self:position()
  local dx, dy = p.x - self.origin.x, p.y - self.origin.y
  for _, l in ipairs(self.layers) do
    if scene.exists(l.id) then
      scene.set(l.id, "Transform", { position = { x = l.x + dx * l.f, y = l.y + dy * l.f, z = l.z } })
    end
  end
end

return Parallax
