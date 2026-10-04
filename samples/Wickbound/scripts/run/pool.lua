-- Burning oil pool: damages enemies standing in it every 0.4 s, then fades.
-- params { dmg, r = radius, dur = seconds, evolved = also tops up the keeper's oil }
local W = require("scripts.lib.world")

local Pool = {}

function Pool:onStart()
  self.dmg = self.params.dmg or 5
  self.r = self.params.r or 1.5
  self.life = self.params.dur or 3.5
  self.evolved = self.params.evolved or false
  self.tick = 0
  local p = self:position()
  self.x, self.y = p.x, p.y
  self:set("Transform", { scale = { x = self.r * 2, y = self.r * 2, z = 1 } })
end

function Pool:onUpdate(dt)
  if dt == 0 then return end
  self.life = self.life - dt
  if self.life <= 0 then
    self:destroy()
    return
  end
  self.tick = self.tick - dt
  if self.tick <= 0 then
    self.tick = 0.4
    local hits = W.areaDamage(self.x, self.y, self.r, self.dmg, "oil_flask")
    if self.evolved and hits > 0 then
      local p = W.player
      p.oil = math.min(p.maxOil, p.oil + 0.5)
    end
  end
  if self.life < 0.5 then self:set("Sprite", { opacity = self.life / 0.5 * 0.55 }) end
end

return Pool
