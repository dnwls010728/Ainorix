local Effects = {}

local function apply(camera, hdr)
  scene.set(camera, "PostProcess", { exposure = hdr and 0.75 or 1,
    toneMapping = hdr and "reinhard" or "none", bloom = hdr and 1.2 or 0,
    bloomThreshold = 0.8, bloomRadius = 12 })
end

function Effects:onUpdate()
  if self.params.mode then return end
  if input.pressed("1") or input.pressed("2") then
    apply(self.id, input.pressed("2"))
  end
end

function Effects:onClick()
  local camera = scene.find("Camera")
  if camera then apply(camera, self.params.mode == 2) end
end

return Effects
