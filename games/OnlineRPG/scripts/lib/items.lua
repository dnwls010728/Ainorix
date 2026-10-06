-- Item kinds. A key names the prefab (prefabs/items/<key>.prefab.json), the world sprite and the
-- bag icon (assets/ui/<key>.png). Keep in sync with tools/make_art.py and tools/make_scene.py.
local Items = {
  names = { potion = "Potion", herb = "Herb", coin = "Coin", gem = "Gem" },
  slots = 8,     -- bag size
  stack = 99,    -- most of one kind in a slot
  ground = 64,   -- most items lying in the world
  reach = 1.3,   -- pick-up distance in world units
}

function Items.prefab(key)
  return "prefabs/items/" .. key .. ".prefab.json"
end

function Items.icon(key)
  return "assets/ui/" .. key .. ".png"
end

return Items
