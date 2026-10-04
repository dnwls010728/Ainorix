-- Run state and rules: time, kills, Glims, XP and levels, the build (weapons and
-- passives), level-up offers, chests, death, victory and the result handed to the menu.
-- Entities talk to it through World.game.
local D = require("scripts.lib.defs")
local B = require("scripts.lib.balance")
local W = require("scripts.lib.world")
local Meta = require("scripts.lib.meta")
local Audio = require("scripts.lib.audio")
local SpriteAnimations = require("scripts.lib.sprite_animations")

local Game = {}

local MAX_LEVEL = D.MAX_ITEM_LEVEL
local floor, min, max = math.floor, math.min, math.max

local HEAL_CHOICE = {
  kind = "heal", id = "heal", level = 0, isNew = false,
  name = { en = "Warm Cocoa", ko = "따뜻한 코코아" },
  desc = { en = "Restore 30% of max HP.", ko = "최대 체력의 30%를 회복해요." },
}
local GLIM_CHOICE = {
  kind = "glims", id = "glims", level = 0, isNew = false,
  name = { en = "Pocket of Glims", ko = "글림 한 줌" },
  desc = { en = "Gain 25 Glims.", ko = "글림 25개를 얻어요." },
}

function Game:onStart()
  W.reset()
  W.game = self
  if not Meta.data then Meta.load() end
  local run = game.get("run") or {}
  self.keeper = D.keeperById(run.keeperId or Meta.data.selectedKeeper)
  self.stage = D.stageById(run.stageId or Meta.data.selectedStage)
  self.mods = Meta.runModifiers()
  -- the sandbox has no clock: the frame Start was pressed on and the run count vary the run
  math.randomseed(time.frame() * 7919 + Meta.data.stats.runs * 104729 + 17)

  self.phase = "playing" -- playing | paused | levelup | chest | dying | victory | over
  self.time, self.kills, self.glims = 0, 0, 0
  self.endless, self.won = false, false
  self.rerolls, self.revives = self.mods.rerolls, self.mods.revives
  self.bossesKilled, self.evolutions, self.braziersLit, self.braziersTotal = 0, 0, 0, B.BRAZIER_COUNT
  self.damageByWeapon = {}
  self.level, self.xp, self.xpNext, self.pendingLevels = 1, 0, B.xpForLevel(1), 0
  self.weapons, self.passives = {}, {}
  self.dmgMul, self.cdMul, self.areaMul, self.lightMul, self.drainMul = 1, 1, 1, 1, 1
  self.choices = {}
  self.dialogs = scene.find("Dialogs")

  scene.set(scene.find("Ground"), "Tilemap", { tileset = "assets/sprites/ground_" .. self.stage.id .. ".png" })
  Audio.sceneChanged()
  Audio.music("run")
end

-- Called by the player script once it exists.
function Game:initPlayer(p)
  local body = scene.child(p.id, "Body")
  local art = SpriteAnimations["keeper_" .. self.keeper.id]
  local size = 68 * B.U * (art.sizeScale or 1)
  scene.set(body, "Sprite", { texture = art.texture, columns = art.columns, rows = art.rows, width = size, height = size, frame = 0 })
  p.bodyAnimated = next(art.clips) ~= nil
  if p.bodyAnimated then
    scene.set(body, "SpriteAnimation", { clips = art.clips, clip = "idle" })
  else
    scene.remove(body, "SpriteAnimation")
  end
  self:addWeapon(self.keeper.startWeapon)
  self:recompute()
  p.hp = p.maxHp
  p.oil = p.maxOil
end

function Game:glimMul()
  return self.mods.glimMul * self.stage.glimMul
end

function Game:onUpdate(dt)
  if self.phase ~= "playing" then return end
  self.time = self.time + dt
  if self.pendingLevels > 0 then self:offerLevelUp() end
end

-- ---------------------------------------------------------------- build

function Game:findWeapon(id)
  for _, w in ipairs(self.weapons) do
    if w.id == id then return w end
  end
  return nil
end

function Game:passiveLevel(id)
  for _, q in ipairs(self.passives) do
    if q.id == id then return q.level end
  end
  return 0
end

-- A weapon is a child entity of the player with its own script.
function Game:addWeapon(id)
  self.weapons[#self.weapons + 1] = { id = id, level = 1, evolved = false }
  local ent = scene.instantiate("prefabs/weapon.prefab.json", { parent = W.player.id, name = "Weapon " .. id })
  scene.set(ent, "Script", { params = { id = id } })
end

-- Derived stats from keeper, meta upgrades and passives.
function Game:recompute()
  local p, m, k = W.player, self.mods, self.keeper
  local hpFrac = (p.maxHp or 0) > 1 and p.hp / p.maxHp or 1
  local oldMaxOil = p.maxOil or B.BASE_OIL
  self.dmgMul = m.damageMul * (1 + 0.12 * self:passiveLevel("wick"))
  self.cdMul = max(0.4, 1 - 0.07 * self:passiveLevel("bellows"))
  self.areaMul = 1 + 0.1 * self:passiveLevel("prism")
  self.lightMul = m.lightMul * (1 + 0.1 * self:passiveLevel("lens"))
  local res = self:passiveLevel("reservoir")
  self.drainMul = m.oilDrainMul * self.stage.oilDrainMul * max(0.4, 1 - 0.06 * res)
  p.maxOil = B.BASE_OIL * (1 + 0.2 * res)
  if p.maxOil > oldMaxOil then p.oil = (p.oil or 0) + (p.maxOil - oldMaxOil) end
  local pl = self:passiveLevel("plating")
  p.maxHp = floor(B.BASE_HP * k.hpMul * m.hpMul * (1 + 0.15 * pl) + 0.5)
  p.hp = max(1, p.maxHp * hpFrac)
  p.armor = pl
  p.speed = B.BASE_SPEED * k.speedMul * m.speedMul * (1 + 0.07 * self:passiveLevel("boots"))
  p.pickup = B.BASE_PICKUP * k.pickupMul * (1 + 0.25 * self:passiveLevel("magnet"))
  scene.set(W.magnet, "Collider2D", { radius = p.pickup })
end

-- ---------------------------------------------------------------- level up

function Game:gainXp(v)
  self.xp = self.xp + v * self.mods.xpMul
  while self.xp >= self.xpNext do
    self.xp = self.xp - self.xpNext
    self.level = self.level + 1
    self.xpNext = B.xpForLevel(self.level)
    self.pendingLevels = self.pendingLevels + 1
  end
end

local function weaponChoice(id, level)
  local d = D.weaponById(id)
  return { kind = "weapon", id = id, level = level, isNew = level == 1, name = d.name, desc = level == 1 and d.desc or B.describeUpgrade(id, level) }
end
local function passiveChoice(id, level)
  local d = D.passiveById(id)
  return { kind = "passive", id = id, level = level, isNew = level == 1, name = d.name, desc = d.desc }
end

-- { {choice, weight}, ... }
function Game:candidates()
  local out = {}
  for _, w in ipairs(self.weapons) do
    if not w.evolved and w.level < MAX_LEVEL then out[#out + 1] = { weaponChoice(w.id, w.level + 1), 1.4 } end
  end
  for _, q in ipairs(self.passives) do
    if q.level < MAX_LEVEL then out[#out + 1] = { passiveChoice(q.id, q.level + 1), 1.2 } end
  end
  if #self.weapons < D.MAX_WEAPONS then
    for _, d in ipairs(D.WEAPONS) do
      if not self:findWeapon(d.id) then out[#out + 1] = { weaponChoice(d.id, 1), 1 } end
    end
  end
  if #self.passives < D.MAX_PASSIVES then
    for _, d in ipairs(D.PASSIVES) do
      if self:passiveLevel(d.id) == 0 then
        local paired = false
        for _, w in ipairs(self.weapons) do
          if D.weaponById(w.id).pair == d.id then paired = true end
        end
        out[#out + 1] = { passiveChoice(d.id, 1), paired and 1.6 or 0.9 }
      end
    end
  end
  return out
end

function Game:rollChoices(n)
  local pool = self:candidates()
  local out = {}
  while #out < n and #pool > 0 do
    local weights = {}
    for i = 1, #pool do weights[i] = { i, pool[i][2] } end
    local pick = B.weighted(weights)
    out[#out + 1] = pool[pick][1]
    table.remove(pool, pick)
  end
  if #out == 0 then return { HEAL_CHOICE, GLIM_CHOICE } end
  if #out < n then out[#out + 1] = HEAL_CHOICE end
  return out
end

function Game:offerLevelUp()
  self.choices = self:rollChoices(3)
  self.phase = "levelup"
  game.pause(true)
  W.sfx("levelup")
  scene.send(self.dialogs, "showLevelup", self.choices)
end

function Game:choose(index)
  if self.phase ~= "levelup" then return end
  local c = self.choices[index]
  if not c then return end
  self:applyChoice(c)
  self.pendingLevels = self.pendingLevels - 1
  W.sfx("select")
  if self.pendingLevels > 0 then self:offerLevelUp() else self:resumePlay() end
end

function Game:reroll(free)
  if self.phase ~= "levelup" then return false end
  if not free then
    if self.rerolls <= 0 then return false end
    self.rerolls = self.rerolls - 1
  end
  self:offerLevelUp()
  return true
end

function Game:applyChoice(c)
  local p = W.player
  if c.kind == "weapon" then
    local w = self:findWeapon(c.id)
    if w then w.level = min(MAX_LEVEL, w.level + 1) else self:addWeapon(c.id) end
  elseif c.kind == "passive" then
    local found = false
    for _, q in ipairs(self.passives) do
      if q.id == c.id then
        q.level = min(MAX_LEVEL, q.level + 1)
        found = true
      end
    end
    if not found then self.passives[#self.passives + 1] = { id = c.id, level = 1 } end
  elseif c.kind == "evolve" then
    local w = self:findWeapon(c.id)
    if w then
      w.evolved = true
      self.evolutions = self.evolutions + 1
    end
  elseif c.kind == "heal" then
    p.hp = min(p.maxHp, p.hp + p.maxHp * 0.3)
  elseif c.kind == "glims" then
    self.glims = self.glims + 25 * self:glimMul()
  end
  self:recompute()
end

-- ---------------------------------------------------------------- chests

function Game:openChest(boss)
  local rewards = {}
  local evo = nil
  for _, w in ipairs(self.weapons) do
    if not w.evolved and w.level >= MAX_LEVEL and self:passiveLevel(D.weaponById(w.id).pair) > 0 then
      evo = w
      break
    end
  end
  if evo then
    local d = D.weaponById(evo.id)
    rewards[#rewards + 1] = { kind = "evolve", id = evo.id, level = MAX_LEVEL, isNew = true, name = d.evoName, desc = d.evoDesc }
  end
  local want = (boss and 3 or 1) - #rewards
  local owned = {}
  for _, c in ipairs(self:candidates()) do
    if not c[1].isNew and not (evo and c[1].id == evo.id) then owned[#owned + 1] = c end
  end
  for _ = 1, want do
    if #owned == 0 then break end
    local k = math.random(#owned)
    rewards[#rewards + 1] = owned[k][1]
    table.remove(owned, k)
  end
  local g = floor((boss and 60 or 25) * self:glimMul() + 0.5)
  rewards[#rewards + 1] = {
    kind = "glims", id = "glims", level = 0, isNew = false,
    name = { en = g .. " Glims", ko = "글림 " .. g .. "개" }, desc = { en = "Shiny.", ko = "반짝반짝." },
  }
  for _, r in ipairs(rewards) do
    if r.kind == "glims" then self.glims = self.glims + g else self:applyChoice(r) end
  end
  self.phase = "chest"
  game.pause(true)
  W.sfx(evo and "evolve" or "chest")
  scene.send(self.dialogs, "showChest", rewards)
end

-- ---------------------------------------------------------------- flow

function Game:resumePlay()
  self.phase = "playing"
  game.pause(false)
end

function Game:pause()
  if self.phase ~= "playing" then return false end
  self.phase = "paused"
  game.pause(true)
  return true
end

function Game:playerDied()
  if self.phase ~= "playing" then return end
  self.phase = "dying"
  game.pause(true)
  W.sfx("death")
  scene.send(self.dialogs, "showDying", self.revives > 0)
end

function Game:revive()
  if self.phase ~= "dying" then return end
  if self.revives > 0 then self.revives = self.revives - 1 end
  local p = W.player
  p.hp = p.maxHp * 0.5
  p.oil = p.maxOil
  p.iframes = 2.5
  local r = 360 * B.U
  for _, e in ipairs(W.enemiesIn(p.x, p.y, r)) do
    local dx, dy = e.x - p.x, e.y - p.y
    local d = math.sqrt(dx * dx + dy * dy)
    if d == 0 then d = 1 end
    e:takeDamage(e.boss and 0 or e.maxHp * 0.6 + 40, "revive", dx / d * 600 * B.U, dy / d * 600 * B.U)
  end
  for _, id in ipairs(scene.withTag("ebullet")) do scene.destroy(id) end
  W.ring(p.x, p.y, r, "#fff2b0", true)
  W.sfx("evolve")
  self:resumePlay()
end

function Game:bossKilled(e)
  self.bossesKilled = self.bossesKilled + 1
  W.shake(16)
  W.sfx("victory")
  if e.kind == "eclipse" and not self.won then
    self.won = true
    self.phase = "victory"
    game.pause(true)
    scene.send(self.dialogs, "showVictory")
  end
end

function Game:continueEndless()
  if self.phase ~= "victory" then return end
  self.endless = true
  self:resumePlay()
end

function Game:result(victory)
  local bonus = floor(self.time / 60) * 5 + (victory and 100 or 0)
  return {
    victory = victory, time = self.time, kills = self.kills, level = self.level,
    glimsEarned = floor(self.glims + bonus * self:glimMul()),
    braziersLit = self.braziersLit, bossesKilled = self.bossesKilled, evolutions = self.evolutions,
    stageId = self.stage.id, keeperId = self.keeper.id, damageByWeapon = self.damageByWeapon,
  }
end

-- Ends the run: the menu scene shows the result.
function Game:finish(victory)
  if self.phase == "over" then return end
  self.phase = "over"
  local result = self:result(victory)
  game.set("result", result)
  game.set("newAch", Meta.applyRun(result))
  game.loadScene("scenes/menu.scene.json")
end

function Game:giveUp()
  self:finish(self.won)
end

function Game:addDamage(weapon, dealt)
  self.damageByWeapon[weapon] = (self.damageByWeapon[weapon] or 0) + dealt
end

return Game
