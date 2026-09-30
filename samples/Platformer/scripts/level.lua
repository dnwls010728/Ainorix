-- The level is the Tilemap text. Marker characters become objects at start:
--   S = player start   c = coin   e = slime   G = goal flag
-- (markers are replaced by empty cells, so the map stays the single source).
local Level = {}

local spawners = {
  c = function(pos) scene.instantiate("prefabs/coin.prefab.json", { position = pos }) end,
  e = function(pos) scene.instantiate("prefabs/slime.prefab.json", { position = { x = pos.x, y = pos.y - 0.1, z = 0 } }) end,
  G = function(pos)
    scene.create("Goal", {
      Transform = { position = { pos.x, pos.y + 1.5, 0 } },
      Collider = { shape = "box", size = { 0.6, 4, 1 }, isTrigger = true },
      Script = { path = "scripts/goal.lua" },
    })
  end,
  S = function(pos) game.set("spawn", { x = pos.x, y = pos.y + 0.05, z = 0 }) end,
}

function Level:onStart()
  local map = self:get("Tilemap").map
  for row, line in ipairs(map) do
    for col = 1, #line do
      local spawn = spawners[line:sub(col, col)]
      if spawn then
        tilemap.set(self.id, col - 1, row - 1, " ")
        spawn(tilemap.cellCenter(self.id, col - 1, row - 1))
      end
    end
  end
end

return Level
