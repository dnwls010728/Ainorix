-- Short-lived world-space asset bursts survive the coin/enemy that spawned them.
local Effect = {}

function Effect.spawn(position, kind)
  local coin = kind == "coin"
  local id = scene.create(coin and "Coin Sparkles" or "Hit Sparks", {
    Transform = { position = { position.x, position.y, (position.z or 0) + 0.4 } },
    Tag = { tags = "effect" },
    VFXSystem = { asset = coin and "effects/coin.vfx.json" or "effects/hit.vfx.json", autoDestroy = true },
  })
  vfx.burst(id, coin and 12 or 16)
  return id
end

return Effect
