-- Stateless effects methods. The App owner explicitly supplies self.services.
local Effects = {}
local POPS = 6

function Effects:toUI(x, y)
  local screen = camera.worldToScreen({ camera = self.camera, position = { x = x, y = y, z = 0 } })
  local point = ui.screenToCanvas({ canvas = self.canvas, screen = screen })
  return point.x - point.width * 0.5, point.y
end

function Effects:popText(text, x, y, color, size)
  local UI = self.services.UI
  self.pop = self.pop % POPS + 1
  local id = UI.id("Pop" .. self.pop)
  local ux, uy = self:toUI(x, y)
  tween.stop(id, "UIText")
  scene.set(id, "UIText", { text = text, x = ux, y = uy, opacity = 1, size = size or 34, color = UI.rgb(color or "#ffffff") })
  tween.to(id, "UIText", { y = uy - 80, opacity = 0 }, 0.9, { ease = "outQuad" })
end

function Effects:burst(x, y, tier, big)
  local D = self.services.D
  local UI = self.services.UI
  local r = D.TIERS[tier].r
  local id = scene.instantiate("prefabs/fx_burst.prefab.json", { position = { x = x, y = y, z = 0 } })
  local c = UI.rgb(D.TIERS[tier].color)
  scene.set(id, "ParticleEmitter", { startColor = c, endColor = c, speed = 2.5 + r * 3, startSize = 0.12 + r * 0.22 })
  particles.burst(id, big and 22 or 12)
  self.fx[#self.fx + 1] = { id = id, left = 80 }
  local ring = scene.instantiate("prefabs/fx_ring.prefab.json", { position = { x = x, y = y, z = 0 } })
  scene.set(ring, "Sprite", { width = r * 2, height = r * 2, color = c })
  tween.to(ring, "Sprite", { width = r * 4.4, height = r * 4.4, opacity = 0 }, 0.4, { ease = "outQuad", scaled = true })
  self.fx[#self.fx + 1] = { id = ring, left = 40 }
end

function Effects:updateFx()
  if game.paused() then return end
  local fx = self.fx
  for i = #fx, 1, -1 do
    fx[i].left = fx[i].left - 1
    if fx[i].left <= 0 then
      if scene.exists(fx[i].id) then scene.destroy(fx[i].id) end
      table.remove(fx, i)
    end
  end
end

Effects.methods = {
  "toUI",
  "popText",
  "burst",
  "updateFx",
}
return Effects
