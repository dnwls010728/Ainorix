-- The jar: every animal in it, merging and the danger line. Animals are prefab instances
-- (prefabs/piece_NN.prefab.json: a Box2D circle with a "Face" sprite child).
local D = require("scripts.lib.defs")

local B = {}

B.pieces = {} -- entity id -> { id, tier, age, over }
B.order = {} -- the same ids, ascending (creation order), so every pass is deterministic

local function forget(id)
  B.pieces[id] = nil
  local list = B.order
  for i = 1, #list do
    if list[i] == id then
      table.remove(list, i)
      return
    end
  end
end

function B.count()
  return #B.order
end

function B.pos(id)
  local p = scene.get(id, "Transform").position
  return p.x, p.y
end

-- Puts a new animal into the jar; `pop` plays the grow-in animation of a merge result.
function B.spawn(tier, x, y, pop)
  local id = scene.instantiate(D.prefab(tier), { position = { x = x, y = y, z = 0 } })
  B.pieces[id] = { id = id, tier = tier, age = 0, over = 0 }
  B.order[#B.order + 1] = id
  if pop then
    local face = scene.child(id, "Face")
    if face then tween.from(face, "Transform", { scale = { 0.55, 0.55, 1 } }, 0.28, { ease = "outBack", scaled = true }) end
  end
  return id
end

function B.remove(id)
  if not B.pieces[id] then return end
  forget(id)
  if scene.exists(id) then scene.destroy(id) end
end

function B.clear()
  for _, id in ipairs(B.order) do
    if scene.exists(id) then scene.destroy(id) end
  end
  B.pieces, B.order = {}, {}
end

-- Merges touching animals of the same kind and advances the danger timers.
-- onMerge(tier, x, y, newId): `tier` is the kind that merged (newId = nil when two of the largest popped).
-- Returns the longest time any animal has been above the line.
function B.step(dt, onMerge)
  local pieces = B.pieces
  local merges = nil
  for _, id in ipairs(B.order) do
    local p = pieces[id]
    if not p.dead then
      for _, other in ipairs(physics.contacts(id)) do
        local q = pieces[other]
        if q and not q.dead and q ~= p and q.tier == p.tier then
          p.dead, q.dead = true, true
          merges = merges or {}
          merges[#merges + 1] = { p, q }
          break
        end
      end
    end
  end
  if merges then
    for _, m in ipairs(merges) do
      local ax, ay = B.pos(m[1].id)
      local bx, by = B.pos(m[2].id)
      local x, y, tier = (ax + bx) / 2, (ay + by) / 2, m[1].tier
      B.remove(m[1].id)
      B.remove(m[2].id)
      local newId = nil
      if tier < D.MAX_TIER then
        local r = D.TIERS[tier + 1].r
        x = math.max(-D.JAR_HALF + r, math.min(D.JAR_HALF - r, x))
        y = math.max(D.FLOOR_Y + r, y)
        newId = B.spawn(tier + 1, x, y, true)
      end
      onMerge(tier, x, y, newId)
    end
  end

  local worst = 0
  for _, id in ipairs(B.order) do
    local p = pieces[id]
    p.age = p.age + dt
    local _, y = B.pos(id)
    if p.age > D.GRACE and y + D.TIERS[p.tier].r > D.LINE_Y then
      p.over = p.over + dt
      if p.over > worst then worst = p.over end
    else
      p.over = 0
    end
  end
  return worst
end

-- The animal under a world point, or nil.
function B.at(x, y)
  local best, bestD = nil, 1e9
  for _, id in ipairs(B.order) do
    local px, py = B.pos(id)
    local r = D.TIERS[B.pieces[id].tier].r
    local d = ((px - x) ^ 2 + (py - y) ^ 2) / (r * r)
    if d <= 1.15 and d < bestD then best, bestD = B.pieces[id], d end
  end
  return best
end

-- Tosses every animal: a kick up and sideways, the same speed change for every size.
function B.shake()
  for _, id in ipairs(B.order) do
    local r = D.TIERS[B.pieces[id].tier].r
    local mass = r * r * 10 -- as set in the prefabs
    local vx, vy = (math.random() * 2 - 1) * 4.5, 4 + math.random() * 3.5
    physics.addImpulse(id, { x = vx * mass, y = vy * mass, z = 0 })
    B.pieces[id].age = 0 -- the line ignores animals in the air again
  end
end

-- Removes every animal whose centre is above y; returns their {tier, x, y}.
function B.clearAbove(y)
  local gone = {}
  for i = #B.order, 1, -1 do
    local id = B.order[i]
    local px, py = B.pos(id)
    if py > y then
      gone[#gone + 1] = { tier = B.pieces[id].tier, x = px, y = py }
      B.remove(id)
    end
  end
  for _, id in ipairs(B.order) do
    B.pieces[id].over, B.pieces[id].age = 0, 0
  end
  return gone
end

return B
