-- Slime (CharacterBody2D, topdown): wanders, chases the hero when it can see
-- them, takes two bolts. params: { speed, hp, sight }
local Slime = {}

function Slime:onStart()
  self.speed = self.params.speed or 1.8
  self.hp = self.params.hp or 2
  self.sight = self.params.sight or 6
  self.player = scene.find("Player")
  self.dir = { x = 1, y = 0 }
  self.wander = 0
  self.knock = nil
  self.hitTimer = 0
end

function Slime:canSee(p, pp)
  local dx, dy = pp.x - p.x, pp.y - p.y
  local dist = math.sqrt(dx * dx + dy * dy)
  if dist > self.sight or dist < 0.01 then return false, 0, 0 end
  local hit = physics.raycast({ x = p.x + dx / dist * 0.45, y = p.y + dy / dist * 0.45, z = 0 }, { x = dx, y = dy, z = 0 }, dist)
  return hit ~= nil and hit.entity == self.player, dx / dist, dy / dist
end

function Slime:onUpdate(dt)
  if self.dead then return end
  self.hitTimer = math.max(0, self.hitTimer - dt)
  if self.hitTimer <= 0 and self.hurtClip then
    self.hurtClip = false
    self:set("SpriteAnimation", { clip = "idle" })
  end
  if self.knock then
    self:setVelocity(self.knock.x, self.knock.y)
    self.knock.x, self.knock.y = self.knock.x * 0.85, self.knock.y * 0.85
    if math.abs(self.knock.x) + math.abs(self.knock.y) < 0.3 then self.knock = nil end
    return
  end
  local p = self:position()
  local seen, cx, cy = false, 0, 0
  if self.player and scene.exists(self.player) then
    seen, cx, cy = self:canSee(p, scene.get(self.player, "Transform").position)
  end
  if seen then
    self:setVelocity(cx * self.speed * 1.4, cy * self.speed * 1.4)
  else
    self.wander = self.wander - dt
    if self.wander <= 0 then
      local a = math.random() * math.pi * 2
      self.dir = { x = math.cos(a), y = math.sin(a) }
      self.wander = 1 + math.random() * 1.5
    end
    self:setVelocity(self.dir.x * self.speed, self.dir.y * self.speed)
  end
  local c = self:get("CharacterBody2D")
  if c.onWall then self.wander = 0 end
  self:set("Sprite", { flipX = c.velocity.x > 0 })
end

-- From a bolt: `dir` is the bolt's velocity.
function Slime:hit(dir)
  if self.dead then return end
  self.hp = self.hp - 1
  local len = math.max(0.01, math.sqrt(dir.x * dir.x + dir.y * dir.y))
  self.knock = { x = dir.x / len * 7, y = dir.y / len * 7 }
  self.hitTimer = 0.25
  self.hurtClip = true
  self:set("SpriteAnimation", { clip = "hurt" })
  if self.hp <= 0 then
    self.dead = true
    audio.play("sounds/stomp.wav", { volume = 0.7 })
    self:remove("CharacterBody2D")
    scene.broadcast("onSlimeDefeated")
    timer.after(0.3, function() self:destroy() end)
  else
    audio.play("sounds/bump.wav", { volume = 0.5 })
  end
end

return Slime
