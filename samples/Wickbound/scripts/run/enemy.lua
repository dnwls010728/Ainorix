-- A critter (RigidBody2D + circle Collider2D): chases the keeper, is faster and hits
-- harder outside the light, where only its eyes show. params { kind, elite, chest }.
-- Kinds with their own behaviour: wisp (keeps its distance and shoots), king (charges,
-- calls skitters), eclipse (bullet rings, calls helpers), gloom (splits), leech (see player.lua).
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")
local Meta = require("scripts.lib.meta")

local Enemy = {}

local U = B.U
local TAU = math.pi * 2
local sqrt, cos, sin, max = math.sqrt, math.cos, math.sin, math.max

function Enemy:onStart()
  local kind = self.params.kind or "shade" -- key of balance.ENEMIES
  local elite = self.params.elite or false -- scaled-up variant with a glow
  self.dropsChest = self.params.chest or false -- leaves a chest when defeated
  local d = B.ENEMIES[kind]
  local g = W.game
  local minutes = g.time / 60
  local hp = d.hp * g.stage.enemyHpMul
  if d.boss then hp = hp * (1 + max(0, (g.bossIndex or 0) - 2) * 0.8) else hp = hp * B.enemyHpScale(minutes) end
  if elite then hp = hp * 9 end
  self.kind, self.elite, self.boss = kind, elite, d.boss or false
  self.hp, self.maxHp = hp, hp
  self.r = d.r * (elite and 1.5 or 1)
  self.speed = d.speed * (elite and 0.9 or 1) * (0.92 + math.random() * 0.16)
  self.dmg = d.dmg * (elite and 1.5 or 1)
  self.xp = d.xp * (elite and 12 or 1)
  self.kx, self.ky = 0, 0 -- knockback velocity
  self.flash, self.stun = 0, 0
  self.lit = true
  self.t, self.t2 = 1.5 + math.random() * 2, 6 -- ability timers
  self.dash, self.dvx, self.dvy, self.tele = 0, 0, 0, 0
  self.face = 1
  self.dead = false
  local p = self:position()
  self.x, self.y = p.x, p.y
  self.body = scene.child(self.id, "Body")
  self.bodyAnimated = scene.has(self.body, "SpriteAnimation")
  self.eyes = scene.child(self.id, "Eyes")
  self.flashGlow = scene.child(self.id, "Flash")
  self.aura = scene.child(self.id, "Aura")
  if elite then
    self:set("Transform", { scale = { x = 1.5, y = 1.5, z = 1 } }) -- the collider scales with it
    scene.set(self.aura, "Sprite", { visible = true })
  end
  W.addEnemy(self)
end

function Enemy:onUpdate(dt)
  if dt == 0 or self.dead then return end
  local player = W.player
  local pos = self:position()
  local x, y = pos.x, pos.y
  self.x, self.y = x, y

  if self.flash > 0 then
    self.flash = self.flash - dt
    if self.flash <= 0 then scene.set(self.flashGlow, "Sprite", { visible = false }) end
  end
  local lit = W.isLit(x, y)
  if lit ~= self.lit then
    self.lit = lit
    scene.set(self.eyes, "Sprite", { visible = not lit })
  end

  local dx, dy = player.x - x, player.y - y
  local d = sqrt(dx * dx + dy * dy)
  if d == 0 then d = 1 end
  dx, dy = dx / d, dy / d
  local face = dx >= 0 and 1 or -1
  if face ~= self.face then
    self.face = face
    scene.set(self.body, "Sprite", { flipX = face < 0 })
  end

  local vx, vy = 0, 0
  if self.stun > 0 then
    self.stun = self.stun - dt
  else
    local sp = self.speed
    local kind = self.kind
    if not lit then
      if kind == "stalker" then sp = sp * 1.9 elseif self.boss then sp = sp * 1.15 else sp = sp * B.DARK_SPEED end
    end
    if kind == "wisp" then
      local want = 250 * U
      local dir = 0
      if d > want + 40 * U then dir = 1 elseif d < want - 40 * U then dir = -1 end
      vx = dx * sp * dir - dy * sp * 0.5
      vy = dy * sp * dir + dx * sp * 0.5
      self.t = self.t - dt
      if self.t <= 0 and d < 420 * U then
        self.t = 3
        local dmg = 10 * B.enemyDmgScale(W.game.time / 60)
        W.spawn("ebullet", x, y, { kind = "ebullet", vx = dx * 150 * U, vy = dy * 150 * U, dmg = dmg })
      end
    elseif kind == "king" then
      self:kingAI(dt, dx, dy, d)
      if self.dash > 0 then
        vx, vy = self.dvx, self.dvy
      elseif self.tele <= 0 then
        vx, vy = dx * sp, dy * sp
      end
    elseif kind == "eclipse" then
      self:eclipseAI(dt)
      vx, vy = dx * sp, dy * sp
    else
      vx, vy = dx * sp, dy * sp
    end
  end
  self:setVelocity(vx + self.kx, vy + self.ky, 0)
  local clip = (vx ~= 0 or vy ~= 0) and "move" or "idle"
  if self.bodyAnimated and clip ~= self.bodyClip then
    self.bodyClip = clip
    scene.set(self.body, "SpriteAnimation", { clip = clip })
  end
  local damp = max(0, 1 - 7 * dt)
  self.kx, self.ky = self.kx * damp, self.ky * damp
end

function Enemy:kingAI(dt, dx, dy, d)
  if self.dash > 0 then
    self.dash = self.dash - dt
    return
  end
  if self.tele > 0 then
    self.tele = self.tele - dt
    if self.tele <= 0 then
      self.dash = 0.75
      self.dvx, self.dvy = dx * 430 * U, dy * 430 * U
      W.shake(5)
    end
    return
  end
  self.t = self.t - dt
  self.t2 = self.t2 - dt
  if self.t <= 0 and d < 520 * U then
    self.t = 7
    self.tele = 0.8
    W.ring(self.x, self.y, self.r * 2.2, "#ff4a4a", false)
    W.sfx("boss")
  end
  if self.t2 <= 0 then
    self.t2 = 6.5
    for i = 0, 5 do
      local a = (i / 6) * TAU
      W.spawn("enemy_skitter", self.x + cos(a) * 70 * U, self.y + sin(a) * 70 * U, { kind = "skitter" })
    end
  end
end

function Enemy:eclipseAI(dt)
  local enraged = self.hp < self.maxHp * 0.5
  self.t = self.t - dt
  self.t2 = self.t2 - dt
  if self.t <= 0 then
    self.t = enraged and 2.6 or 3.8
    local n = enraged and 20 or 14
    local a0 = math.random() * TAU
    local dmg = 14 * B.enemyDmgScale(W.game.time / 60)
    for i = 0, n - 1 do
      local a = a0 + (i / n) * TAU
      W.spawn("ebullet", self.x, self.y, { kind = "ebullet", vx = cos(a) * 140 * U, vy = sin(a) * 140 * U, dmg = dmg })
    end
    W.sfx("pulse")
  end
  if self.t2 <= 0 then
    self.t2 = 9
    for i = 0, 7 do
      local a = (i / 8) * TAU
      local kind = i % 4 == 0 and "stalker" or "shade"
      W.spawn("enemy_" .. kind, self.x + cos(a) * 90 * U, self.y + sin(a) * 90 * U, { kind = kind })
    end
  end
end

-- Damage from a weapon; kx, ky = knockback velocity, stun = seconds. Returns true if it died.
function Enemy:takeDamage(dmg, weapon, kx, ky, stun)
  if self.dead then return false end
  if not self.boss then
    self.kx, self.ky = self.kx + (kx or 0), self.ky + (ky or 0)
    if stun and stun > 0 then self.stun = max(self.stun, stun) end
  end
  if dmg <= 0 then return false end
  W.game:addDamage(weapon, math.min(self.hp, dmg))
  self.hp = self.hp - dmg
  if self.flash <= 0 then scene.set(self.flashGlow, "Sprite", { visible = true }) end
  self.flash = 0.1
  if Meta.data.settings.damageNumbers then
    W.number(self.x, self.y + self.r, tostring(math.floor(dmg + 0.5)), dmg >= 40 and "#ffe27a" or "#ffffff", dmg >= 40)
  end
  if self.hp <= 0 then
    self:die(true)
    return true
  end
  return false
end

-- drops = false when a boss's defeat clears the field (XP only).
function Enemy:die(drops)
  if self.dead then return end
  self.dead = true
  W.removeEnemy(self)
  local g = W.game
  local x, y = self.x, self.y
  local Pickup = require("scripts.run.pickup")
  Pickup.drop("xp", x, y, self.xp)
  if drops then
    g.kills = g.kills + 1
    W.burst(x, y, B.ENEMIES[self.kind].eye, self.boss and 60 or self.elite and 24 or 6, (self.boss and 320 or 140) * U)
    W.sfx("kill")
    if self.kind == "gloom" then
      W.spawn("enemy_gloomlet", x - 10 * U, y, { kind = "gloomlet" })
      W.spawn("enemy_gloomlet", x + 10 * U, y, { kind = "gloomlet" })
    end
    local r = math.random()
    if self.elite or self.boss then
      Pickup.drop("oil", x + 14 * U, y, 40)
      Pickup.drop("glim", x - 14 * U, y, self.boss and (self.kind == "king" and 60 or 150) or 10)
      Pickup.drop("heal", x, y - 14 * U, 25)
    elseif r < 0.032 then
      Pickup.drop("oil", x, y, 18)
    elseif r < 0.072 then
      Pickup.drop("glim", x, y, math.random(1, 3))
    elseif r < 0.079 then
      Pickup.drop("heal", x, y, 20)
    end
    if self.dropsChest then Pickup.drop("chest", x, y, 1, self.boss) end
    if self.boss then
      -- a breather: the lesser critters scatter and the bullets vanish
      local others = {}
      for _, o in ipairs(W.enemyList) do
        if not o.boss then others[#others + 1] = o end
      end
      for _, o in ipairs(others) do o:die(false) end
      for _, id in ipairs(scene.withTag("ebullet")) do scene.destroy(id) end
      g:bossKilled(self)
    end
  end
  self:destroy()
end

return Enemy
