-- Camera effects stay neutral until a preset key is pressed.
local Effects = {}
local names = { "Off", "Tone", "Bloom", "Vignette", "FXAA", "All" }

function Effects:onStart()
  self.mode = 1
end

function Effects:onUpdate()
  for mode = 1, 6 do
    if input.pressed(tostring(mode)) then
      local hdr = mode == 2 or mode == 3 or mode == 6
      self:set("PostProcess", {
        exposure = hdr and 0.75 or 1,
        toneMapping = hdr and "reinhard" or "none",
        bloom = (mode == 3 or mode == 6) and 1.2 or 0,
        bloomThreshold = 0.8,
        bloomRadius = 12,
        vignette = (mode == 4 or mode == 6) and 0.65 or 0,
        vignetteRadius = 0.5,
        vignetteSoftness = 0.6,
        fxaa = mode == 5 or mode == 6
      })
      self.mode = mode
      local hint = scene.find("Hint")
      if hint then
        scene.set(hint, "UIText", { text = "WASD / arrows: move   Shift: run   Space: jump\n"
          .. "1 Off  2 Tone  3 Bloom  4 Vignette  5 FXAA  6 All: " .. names[mode] })
      end
      log.info("PostProcess preset: " .. names[mode])
      break
    end
  end
end

return Effects
