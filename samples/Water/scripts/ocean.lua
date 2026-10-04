-- The sea surface: keys 1 / 2 / 3 pick a sea state. The wave amplitudes and colors ease to the
-- preset and go to the water graph every frame through MeshRenderer.shaderUniforms.
local Waves = require("scripts.waves")
local Ocean = {}

local STATES = {
  { name = "Calm", scale = 0.4, deep = { 0.03, 0.27, 0.42 }, shallow = { 0.14, 0.62, 0.68 } },
  { name = "Swell", scale = 1.0, deep = { 0.02, 0.2, 0.34 }, shallow = { 0.1, 0.55, 0.62 } },
  { name = "Storm", scale = 1.9, deep = { 0.02, 0.1, 0.17 }, shallow = { 0.1, 0.32, 0.38 } },
}

local function copy(color) return { color[1], color[2], color[3] } end

function Ocean:onStart()
  self.state = 2
  local state = STATES[self.state]
  Waves.scale = state.scale
  self.deep, self.shallow = copy(state.deep), copy(state.shallow)
  game.set("seaState", state.name)
  self:apply()
end

function Ocean:apply()
  self:set("MeshRenderer", { shaderUniforms = {
    amp = Waves.amplitudes(),
    deep = { self.deep[1], self.deep[2], self.deep[3], 1 },
    shallow = { self.shallow[1], self.shallow[2], self.shallow[3], 1 },
  } })
end

function Ocean:onUpdate(dt)
  for i = 1, #STATES do
    if input.pressed(tostring(i)) and i ~= self.state then
      self.state = i
      game.set("seaState", STATES[i].name)
      scene.set(scene.find("Title"), "UIText", { text = "Sea state: " .. STATES[i].name })
    end
  end
  local target = STATES[self.state]
  local ease = math.min(1, dt * 1.5)
  Waves.scale = Waves.scale + (target.scale - Waves.scale) * ease
  for c = 1, 3 do
    self.deep[c] = self.deep[c] + (target.deep[c] - self.deep[c]) * ease
    self.shallow[c] = self.shallow[c] + (target.shallow[c] - self.shallow[c]) * ease
  end
  self:apply()
end

return Ocean
