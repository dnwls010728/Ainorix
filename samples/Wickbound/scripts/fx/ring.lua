-- Expanding ring that fades out. params { r = final radius, color = "#rrggbb", fill = bool }
local Ring = {}

local LIFE = 0.35

local function rgb(hex)
  return { tonumber(hex:sub(2, 3), 16) / 255, tonumber(hex:sub(4, 5), 16) / 255, tonumber(hex:sub(6, 7), 16) / 255 }
end

function Ring:onStart()
  self.r = self.params.r or 2
  self.fill = self.params.fill and scene.child(self.id, "Fill") or nil
  local color = rgb(self.params.color or "#ffffff")
  self:set("Sprite", { color = color })
  if self.fill then scene.set(self.fill, "Sprite", { color = color, visible = true }) end
  self.life = LIFE
  self:apply()
end

function Ring:apply()
  local f = 1 - self.life / LIFE
  local s = self.r * (0.3 + 0.7 * f) * 2
  self:set("Transform", { scale = { x = s, y = s, z = 1 } })
  self:set("Sprite", { opacity = 1 - f })
  if self.fill then scene.set(self.fill, "Sprite", { opacity = (1 - f) * 0.35 }) end
end

function Ring:onUpdate(dt)
  self.life = self.life - dt
  if self.life <= 0 then
    self:destroy()
    return
  end
  self:apply()
end

return Ring
