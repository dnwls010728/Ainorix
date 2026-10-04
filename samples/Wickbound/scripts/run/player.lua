-- The lantern keeper (CharacterBody2D, top-down): movement, health, oil and the light it
-- gives, contact damage from enemies touching the Hurtbox child. Weapons are child
-- entities with their own script (scripts/run/weapon.lua).
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")

local Player = {}

local STICK_RADIUS = 56 -- reference pixels
local sqrt, min, max, sin = math.sqrt, math.min, math.max, math.sin

function Player:onStart()
  W.player = self
  W.hurtbox = scene.child(self.id, "Hurtbox")
  W.magnet = scene.child(self.id, "Magnet")
  self.body = scene.child(self.id, "Body")
  self.lamp = scene.child(self.body, "Lamp")
  self.lantern = scene.child(self.id, "Lantern")
  self.warmth = scene.child(self.id, "Warmth")
  local p = self:position()
  self.x, self.y = p.x, p.y
  self.fx, self.fy, self.face = 1, 0, 1 -- facing
  self.moving = false
  self.iframes, self.hurtFlash = 0, 0
  self.lightR = B.BASE_LIGHT
  self.dread = false
  self.touching = {} -- enemy ids inside the hurtbox
  self.hint = {}
  self.stick = { active = false, ox = 0, oy = 0, x = 0, y = 0 }
  self.t = 0
  W.game:initPlayer(self)
end

-- From the Hurtbox child: an enemy started or stopped touching the keeper.
function Player:touch(other, entering)
  self.touching[other] = entering or nil
end

-- Keyboard, or a drag-anywhere virtual joystick (mouse / first finger).
function Player:readMove()
  local x, y = 0, 0
  if input.down("A") or input.down("Left") then x = x - 1 end
  if input.down("D") or input.down("Right") then x = x + 1 end
  if input.down("W") or input.down("Up") then y = y + 1 end
  if input.down("S") or input.down("Down") then y = y - 1 end

  local s = self.stick
  local mx, my = input.mouse()
  mx, my = mx * 1280, my * 720
  if input.pressed("MouseLeft") then
    s.active, s.ox, s.oy, s.x, s.y = true, mx, my, mx, my
  elseif s.active and input.down("MouseLeft") then
    local dx, dy = mx - s.ox, my - s.oy
    local d = sqrt(dx * dx + dy * dy)
    if d > STICK_RADIUS then
      -- drag the origin along so direction changes stay responsive
      s.ox = s.ox + (dx / d) * (d - STICK_RADIUS)
      s.oy = s.oy + (dy / d) * (d - STICK_RADIUS)
    end
    s.x, s.y = mx, my
  else
    s.active = false
  end

  if x == 0 and y == 0 and s.active then
    local dx, dy = s.x - s.ox, s.oy - s.y -- screen y grows downward
    local d = sqrt(dx * dx + dy * dy)
    if d > 8 then
      local m = min(1, d / STICK_RADIUS)
      if m > 0.6 then m = 1 end -- snap to full speed so thumbs do not have to stretch
      x, y = (dx / d) * m, (dy / d) * m
    end
  elseif x ~= 0 or y ~= 0 then
    local d = sqrt(x * x + y * y)
    x, y = x / d, y / d
  end
  return x, y
end

function Player:damage(raw)
  if self.iframes > 0 then return end
  self.hp = self.hp - max(1, raw - self.armor)
  self.iframes = 0.55
  self.hurtFlash = 0.25
  W.sfx("hurt")
  W.shake(7)
  W.burst(self.x, self.y, "#ff5a5a", 8, 160 * B.U)
end

function Player:onUpdate(dt)
  if dt == 0 then
    self.stick.active = false
    return
  end
  local g = W.game
  self.t = self.t + dt
  local pos = self:position()
  self.x, self.y = pos.x, pos.y

  -- movement
  local mx, my = self:readMove()
  self.moving = mx ~= 0 or my ~= 0
  if self.moving then
    local d = sqrt(mx * mx + my * my)
    self.fx, self.fy = mx / d, my / d
  end
  self:setVelocity(mx * self.speed, my * self.speed, 0)
  local face = self.fx >= 0 and 1 or -1
  if face ~= self.face then
    self.face = face
    scene.set(self.body, "Sprite", { flipX = face < 0 })
    scene.set(self.lamp, "Transform", { position = { x = 22 * B.U * face } })
  end

  if self.iframes > 0 then self.iframes = self.iframes - dt end
  if self.hurtFlash > 0 then self.hurtFlash = self.hurtFlash - dt end
  if self.hp > 0 then self.hp = min(self.maxHp, self.hp + 0.25 * dt) end
  local blink = self.iframes > 0 and math.floor(self.t * 20) % 2 == 0
  if blink ~= self.blink then
    self.blink = blink
    scene.set(self.body, "Sprite", { opacity = blink and 0.4 or 1 })
  end
  local clip = self.moving and "move" or "idle"
  if self.bodyAnimated and clip ~= self.bodyClip then
    self.bodyClip = clip
    scene.set(self.body, "SpriteAnimation", { clip = clip })
  end

  -- oil and light
  local bossDrain = 1
  for _, e in ipairs(W.enemyList) do
    if e.kind == "eclipse" then bossDrain = 1.5 end
  end
  self.oil = max(0, self.oil - B.OIL_DRAIN * g.drainMul * bossDrain * dt)
  local target = B.BASE_LIGHT * (0.45 + 0.55 * (self.oil / self.maxOil)) * g.lightMul
  self.lightR = self.lightR + (target - self.lightR) * min(1, dt * 4)
  local flick = 1 + sin(self.t * 9) * 0.012 + sin(self.t * 23) * 0.008
  scene.set(self.lantern, "Light2D", { radius = self.lightR * 1.18 * flick })
  local warm = self.lightR * 2
  scene.set(self.warmth, "Transform", { scale = { x = warm / (2 * B.BASE_LIGHT), y = warm / (2 * B.BASE_LIGHT), z = 1 } })
  local lampR = (14 + 18 * self.oil / self.maxOil) * (1 + sin(self.t * 13) * 0.1) / 32
  scene.set(self.lamp, "Transform", { scale = { x = lampR, y = lampR, z = 1 } })

  local wasDread = self.dread
  self.dread = self.oil <= 0
  if self.dread ~= wasDread then
    scene.set(scene.find("Camera"), "Darkness2D", self.dread and { color = { 46 / 255, 26 / 255, 84 / 255 }, opacity = 0.97 }
      or { color = { 27 / 255, 26 / 255, 78 / 255 }, opacity = 0.94 })
    if self.dread then W.toast({ en = "The lantern is almost out!", ko = "등불이 꺼질 것 같아요!" }) end
  end
  local hint = self.hint
  if not hint.oil and self.oil < self.maxOil * 0.3 then
    hint.oil = true
    W.toast({ en = "Oil is low — grab oil drops or light a campfire", ko = "기름이 부족해요 — 기름을 줍거나 모닥불을 밝혀요" })
  end
  if not hint.move and g.time > 1.5 then
    hint.move = true
    W.toast({ en = "Just keep moving — your lantern does the rest!", ko = "움직이기만 하면 돼요. 공격은 등불이 알아서 해요!" })
  end
  if not hint.brazier and g.time > 14 then
    hint.brazier = true
    W.toast({ en = "Follow the arrow to a campfire and stand in its ring", ko = "화살표를 따라 모닥불로 가서 원 안에 서 봐요" })
  end

  -- contact damage: the lowest-id enemy touching the hurtbox hits when the keeper is vulnerable
  if self.iframes <= 0 then
    local hit = nil
    for id in pairs(self.touching) do
      local e = W.enemies[id]
      if not e or e.dead then
        self.touching[id] = nil
      elseif not hit or id < hit.id then
        hit = e
      end
    end
    if hit then
      local dscale = B.enemyDmgScale(g.time / 60)
      self:damage(hit.dmg * dscale * (hit.lit and 1 or B.DARK_DAMAGE))
      if hit.kind == "leech" then
        self.oil = max(0, self.oil - 12)
        W.number(self.x, self.y + 30 * B.U, "-oil", "#c8ff5a", true)
      end
    end
  end

  if self.hp <= 0 then
    self.hp = 0
    g:playerDied()
  end
end

return Player
