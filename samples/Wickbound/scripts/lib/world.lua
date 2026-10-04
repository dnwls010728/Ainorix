-- Shared state of the current run: who is in the world, and helpers every entity script
-- uses (spawning prefabs, finding enemies, lit areas, effects). The Lua state survives
-- scene changes, so the Game script resets this when a run starts.
local B = require("scripts.lib.balance")

local W = {}

function W.reset()
  W.game = nil -- instance of scripts/run/game.lua
  W.player = nil -- instance of scripts/run/player.lua
  W.hud = nil -- instance of scripts/run/hud.lua
  W.camera = nil -- instance of scripts/run/camera.lua
  W.hurtbox, W.magnet = nil, nil -- entity ids of the player's sensors
  W.enemies = {} -- entity id -> enemy instance
  W.enemyList = {} -- the same instances in spawn (= id) order
  W.lights = {} -- { {x, y, r} }: lit braziers
  W.braziers = {} -- brazier instances
  W.pickups = 0
  W.xpGems = {} -- live XP pickups, for merging when there are too many
end
W.reset()

-- ---------------------------------------------------------------- prefabs

-- Instantiates a prefab at (x, y) on depth z. `params` become the root Script's params
-- (set before its onStart runs).
function W.spawn(prefab, x, y, params, z)
  local id = scene.instantiate("prefabs/" .. prefab .. ".prefab.json", { position = { x = x, y = y, z = z or 0 } })
  if params then scene.set(id, "Script", { params = params }) end
  return id
end

-- ---------------------------------------------------------------- enemies

function W.addEnemy(e)
  W.enemies[e.id] = e
  W.enemyList[#W.enemyList + 1] = e
end

function W.removeEnemy(e)
  W.enemies[e.id] = nil
  local list = W.enemyList
  for i = 1, #list do
    if list[i] == e then
      table.remove(list, i)
      return
    end
  end
end

-- Nearest living enemy within maxD of (x, y); `exclude` is a set of entity ids.
function W.nearest(x, y, maxD, exclude)
  local best, bd = nil, maxD * maxD
  local list = W.enemyList
  for i = 1, #list do
    local e = list[i]
    if not e.dead then
      local dx, dy = x - e.x, y - e.y
      local d = dx * dx + dy * dy
      if d < bd and not (exclude and exclude[e.id]) then best, bd = e, d end
    end
  end
  return best
end

local center = { x = 0, y = 0, z = 0 }

-- Living enemies whose collider touches the circle, in id order (physics query).
function W.enemiesIn(x, y, r)
  center.x, center.y = x, y
  local ids = physics.overlapSphere(center, r)
  local out = {}
  for i = 1, #ids do
    local e = W.enemies[ids[i]]
    if e and not e.dead then out[#out + 1] = e end
  end
  return out
end

-- Damages every enemy in the circle; returns how many were hit.
function W.areaDamage(x, y, r, dmg, weapon, knock, stun)
  local hits = W.enemiesIn(x, y, r)
  for _, e in ipairs(hits) do
    local kx, ky = 0, 0
    if knock and knock ~= 0 then
      local dx, dy = e.x - x, e.y - y
      local d = math.sqrt(dx * dx + dy * dy)
      if d == 0 then d = 1 end
      kx, ky = dx / d * knock, dy / d * knock
    end
    e:takeDamage(dmg, weapon, kx, ky, stun)
  end
  return #hits
end

-- ---------------------------------------------------------------- light

function W.isLit(x, y)
  local p = W.player
  if p then
    local dx, dy = x - p.x, y - p.y
    if dx * dx + dy * dy < p.lightR * p.lightR then return true end
  end
  local lights = W.lights
  for i = 1, #lights do
    local l = lights[i]
    local dx, dy = x - l.x, y - l.y
    if dx * dx + dy * dy < l.r * l.r then return true end
  end
  return false
end

-- ---------------------------------------------------------------- effects

-- Star puffs: a one-shot additive particle burst.
function W.burst(x, y, color, n, speed)
  W.spawn("fx_burst", x, y, { color = color, n = n, speed = speed }, 0)
end

-- Expanding ring (radius in world units); fill adds a soft disc.
function W.ring(x, y, r, color, fill)
  W.spawn("fx_ring", x, y, { r = r, color = color, fill = fill or false }, 0)
end

-- Beam of light from (x1, y1) to (x2, y2).
function W.beam(x1, y1, x2, y2, width, color, life)
  W.spawn("fx_beam", (x1 + x2) / 2, (y1 + y2) / 2, { dx = x2 - x1, dy = y2 - y1, w = width, color = color, life = life or 0.22 }, 0)
end

-- Floating damage number or word above a world position.
function W.number(x, y, text, color, big)
  if W.hud then W.hud:number(x, y, text, color, big) end
end

function W.sfx(name)
  require("scripts.lib.audio").sfx(name)
end

function W.shake(amount)
  if W.camera then W.camera:shake(amount) end
end

function W.toast(text)
  if W.hud then W.hud:toast(require("scripts.lib.i18n").L(text)) end
end

W.U = B.U

return W
