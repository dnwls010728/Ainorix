-- Patrolling slime: walks until a wall or a ledge, then turns around.
-- Stomp it from above; touching it from the side hurts the player.
-- Needs CharacterBody (sphere, plane2D) + Sprite + SpriteAnimation (walk, squash).
local Slime = {}

function Slime:onStart()
  self.dir = -1
  self.speed = self.params.speed or 1.6
  self.radius = self:get("CharacterBody").radius
  self.player = scene.find("Player")
  self.dead = false
end

-- True if a ray from `origin` hits level geometry (not ourselves or the player).
function Slime:blocked(origin, dir, dist)
  local hit = physics.raycast(origin, dir, dist)
  return hit ~= nil and hit.entity ~= self.id and hit.entity ~= self.player
end

function Slime:onUpdate(dt)
  if self.dead then return end
  local p = self:position()
  local ahead = { x = p.x + self.dir * (self.radius + 0.02), y = p.y, z = 0 }
  local wall = self:blocked(ahead, { x = self.dir, y = 0, z = 0 }, 0.15)
  local ground = self:blocked({ x = p.x + self.dir * 0.45, y = p.y, z = 0 }, { x = 0, y = -1, z = 0 }, self.radius + 0.4)
  if self:grounded() and (wall or not ground) then self.dir = -self.dir end
  self:setVelocity(self.dir * self.speed, self:velocity().y, 0)
  self:set("Sprite", { flipX = self.dir > 0 })

  -- Player contact (a simple box test is enough and fully deterministic).
  if not self.player or not scene.exists(self.player) then return end
  local pp = scene.get(self.player, "Transform").position
  local dx, dy = pp.x - p.x, pp.y - p.y
  if math.abs(dx) < 0.62 and math.abs(dy) < 0.8 then
    local pv = scene.get(self.player, "CharacterBody").velocity
    if pv.y < 0 and dy > 0.3 then
      self:squash()
      scene.send(self.player, "bounce")
    else
      scene.send(self.player, "die")
    end
  end
end

function Slime:squash()
  if self.dead then return end
  self.dead = true
  require("scripts.effects").spawn(self:position(), "hit")
  audio.play("sounds/stomp.wav", { volume = 0.7 })
  self:remove("CharacterBody")
  self:set("SpriteAnimation", { clip = "squash" })
  timer.after(0.4, function() self:destroy() end)
end

return Slime
