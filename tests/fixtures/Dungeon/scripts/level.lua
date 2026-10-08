-- The level is the Tilemap text (tileset: tilesets/dungeon.tileset.json).
-- Tiles: # wall (autotile), ~ water (autotile), . floor, , moss, E stairs (exit).
-- Marker characters become objects at start and turn into floor:
--   P player start   c coin   s slime   b crate   o pillar   r boulder
local Level = {}

-- Boulder outline, the same polygon tools/make_art.py draws the sprite from.
local ROCK = { { -1.0, -0.5 }, { -0.4, -0.75 }, { 0.6, -0.7 }, { 1.0, -0.2 }, { 0.8, 0.5 },
               { 0.25, 0.75 }, { -0.05, 0.3 }, { -0.55, 0.6 }, { -1.0, 0.1 } }

local spawners = {
  P = function(pos) scene.set(scene.find("Player"), "Transform", { position = { pos.x, pos.y, 0 } }) end,
  c = function(pos) scene.instantiate("prefabs/coin.prefab.json", { position = { x = pos.x, y = pos.y, z = 0.05 } }) end,
  s = function(pos) scene.instantiate("prefabs/slime.prefab.json", { position = { x = pos.x, y = pos.y, z = 0.1 } }) end,
  b = function(pos) scene.instantiate("prefabs/crate.prefab.json", { position = { x = pos.x, y = pos.y, z = 0.05 } }) end,
  o = function(pos)
    scene.create("Pillar", {
      Transform = { position = { pos.x, pos.y, 0.05 } },
      Sprite = { texture = "assets/sprites/pillar.png", pixelArt = true },
      Collider2D = { shape = "circle", radius = 0.45 },
    })
  end,
  r = function(pos)
    scene.create("Boulder", {
      Transform = { position = { pos.x + 0.5, pos.y, 0.05 } },
      Sprite = { texture = "assets/sprites/rock.png", pixelArt = true },
      Collider2D = { shape = "polygon", points = ROCK },
    })
  end,
}

function Level:onStart()
  local map = self:get("Tilemap").map
  local coins = 0
  for row, line in ipairs(map) do
    for col = 1, #line do
      local ch = line:sub(col, col)
      local pos = tilemap.cellCenter(self.id, col - 1, row - 1)
      local spawn = spawners[ch]
      if spawn then
        tilemap.set(self.id, col - 1, row - 1, ".")
        spawn(pos)
        if ch == "c" then coins = coins + 1 end
      elseif ch == "E" then
        scene.create("Exit", {
          Transform = { position = { pos.x, pos.y, 0 } },
          Collider2D = { shape = "box", size = { 0.6, 0.6, 1 }, isTrigger = true },
          Script = { path = "scripts/exit.lua" },
        })
      end
    end
  end
  game.set("coinsTotal", coins)
end

return Level
