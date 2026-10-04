-- A beam of light between two points that thins out and fades.
-- params { dx, dy = end minus start, w = width, color = "#rrggbb", life = seconds }
local Beam = {}

local function rgb(hex)
  return { tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255 }
end

function Beam:onStart()
  local dx, dy = self.params.dx or 1, self.params.dy or 0
  self.len = math.sqrt(dx * dx + dy * dy)
  self.w = self.params.w or 0.5
  self.max = self.params.life or 0.22
  self.life = self.max
  self:set("Transform", { rotation = { x = 0, y = 0, z = math.deg(math.atan(dy, dx)) } })
  self:set("Sprite", { color = rgb(self.params.color or "#ffffff") })
  self:apply()
end

function Beam:apply()
  local f = self.life / self.max
  self:set("Transform", { scale = { x = self.len + self.w * 0.5, y = self.w * (0.6 + 0.4 * f), z = 1 } })
  self:set("Sprite", { opacity = f * 0.45 })
  scene.set(scene.child(self.id, "Core"), "Sprite", { opacity = f })
end

function Beam:onUpdate(dt)
  self.life = self.life - dt
  if self.life <= 0 then
    self:destroy()
    return
  end
  self:apply()
end

return Beam
