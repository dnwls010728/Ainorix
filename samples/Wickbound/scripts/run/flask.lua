-- A thrown oil flask: arcs to its target and leaves a burning pool.
-- params { tx, ty = target, dmg, area = pool radius, dur = pool seconds, evolved }
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")

local Flask = {}

local FLIGHT = 0.55

function Flask:onStart()
  local p = self:position()
  self.sx, self.sy = p.x, p.y
  self.tx, self.ty = self.params.tx or p.x, self.params.ty or p.y
  self.body = scene.child(self.id, "Body")
  self.t = 0
end

function Flask:onUpdate(dt)
  if dt == 0 then return end
  self.t = self.t + dt / FLIGHT
  if self.t >= 1 then
    local q = self.params
    W.spawn("pool", self.tx, self.ty, { dmg = q.dmg or 5, r = q.area or 1.5, dur = q.dur or 3.5, evolved = q.evolved or false })
    W.burst(self.tx, self.ty, "#c8ff5a", 10, 140 * B.U)
    W.sfx("flask")
    self:destroy()
    return
  end
  self:setPosition(self.sx + (self.tx - self.sx) * self.t, self.sy + (self.ty - self.sy) * self.t, 0)
  scene.set(self.body, "Transform", { position = { y = math.sin(self.t * math.pi) * 50 * B.U } })
end

return Flask
