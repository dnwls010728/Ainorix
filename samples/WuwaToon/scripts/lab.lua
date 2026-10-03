-- The studio camera/light are fixed to match the graph's explicit uniforms.
-- Material changes use existing component bindings; source assets stay untouched.
local Lab = {}
local parts = {"skin", "hair", "coat", "boots", "trim", "eyes", "iris", "ink", "blush"}

function Lab:onStart()
  if self.params.action then return end
  self.mode, self.outline, self.turntable, self.angle = "toon", true, false, 0
  self.lastActions = {}
  self:updateOutlines()
  scene.set("Button_turntable", "UIButton", {text = "Spin: OFF"})
end

function Lab:updateOutlines()
  scene.set("Button_outline", "UIButton", {text = self.outline and "Outline: ON" or "Outline: OFF"})
  for _, name in ipairs({"skin", "hair", "coat", "boots"}) do
    scene.set("Outline_" .. name, "MeshRenderer", {visible = self.outline and self.mode == "toon"})
  end
end

function Lab:setMode(mode)
  self.mode = mode
  for _, name in ipairs(parts) do
    local path = mode == "normals" and "materials/normals.mat.json" or "materials/" .. name .. "-" .. mode .. ".mat.json"
    scene.set("Part_" .. name, "MeshRenderer", {material = path})
  end
  local descriptions = {
    toon = "MODE 01   TOON / TINTED SHADOW + RIM + SPECULAR",
    pbr = "MODE 02   STANDARD PBR / SAME GEOMETRY",
    normals = "MODE 03   WORLD NORMALS / SAME GEOMETRY"
  }
  scene.set("Mode", "UIText", {text = descriptions[mode]})
  self:updateOutlines()
end

function Lab:onLabAction(action)
  if self.params.action then return end
  -- A physical mouse/primary touch can deliver both UIButton.key and onClick.
  -- Remember actions before Systems clears pressed input ahead of callbacks.
  local frame = time.frame()
  if self.lastActions[action] == frame then return end
  self.lastActions[action] = frame
  if action == "toon" or action == "pbr" or action == "normals" then
    self:setMode(action)
  elseif action == "outline" then
    self.outline = not self.outline
    self:updateOutlines()
  elseif action == "turntable" then
    self.turntable = not self.turntable
    scene.set("Button_turntable", "UIButton", {text = self.turntable and "Spin: ON" or "Spin: OFF"})
  elseif action == "reset" then
    self.angle, self.turntable, self.outline = 0, false, true
    scene.set("Button_turntable", "UIButton", {text = "Spin: OFF"})
    scene.set(self.id, "Transform", {rotation = {0, 0, 0}})
    self:setMode("toon")
  end
end

function Lab:onClick()
  if self.params.action then scene.send("Character", "onLabAction", self.params.action) end
end

function Lab:onUpdate(dt)
  if self.params.action then return end
  local actions = {"toon", "pbr", "normals"}
  for index, action in ipairs(actions) do
    if input.pressed(tostring(index)) then self:onLabAction(action) end
  end
  if input.pressed("O") then self:onLabAction("outline") end
  if input.pressed("P") then self:onLabAction("turntable") end
  if input.pressed("R") then self:onLabAction("reset") end
  if self.turntable then
    self.angle = (self.angle + dt * 24) % 360
    scene.set(self.id, "Transform", {rotation = {0, self.angle, 0}})
  end
end

return Lab
