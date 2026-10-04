-- Quest and dialog of the sample: find the five crystal shards, then report to the elder.
-- Keys 1 and 2 switch the camera effects so the HD-2D look can be compared with the plain scene.
local Game = {}

local INTRO = {
  "Traveller! The lanterns grow dim tonight.",
  "Five crystal shards fell along the road. Their light keeps the lanterns burning.",
  "Bring them to me, would you? Look past the gate, by the pond and out in the meadow.",
}
local WAITING = { "%d of the shards are still out there. Follow their glow." }
local DONE = {
  "All five! See how the lanterns answer them.",
  "The road is safe again. Walk in the light, traveller.",
}

function Game:onStart()
  self.total = #scene.withTag("crystal")
  self.found = 0
  self.hero = scene.find("Hero")
  self.elder = scene.find("Elder")
  self.camera = scene.find("Camera")
  self.look = scene.get(self.camera, "PostProcess") -- the scene's settings, restored by key 1
  self.effects, self.dof = true, true
  self.talked, self.finished = false, false
  self.lines, self.line, self.shown = nil, 0, 0
  game.set("dialog", false)
  game.set("crystals", 0)
  self:refreshQuest()
end

function Game:refreshQuest()
  local text = "Crystals " .. self.found .. " / " .. self.total
  if self.finished then text = "<color=#ffd27a>The lanterns burn bright</color>" end
  scene.set(scene.find("Quest"), "UIText", { text = text })
end

function Game:onCrystal()
  self.found = self.found + 1
  game.set("crystals", self.found)
  self:refreshQuest()
end

function Game:showLine()
  local text = self.lines[self.line]
  if self.lines == WAITING then text = string.format(text, self.total - self.found) end
  self.shown = 0
  scene.set(scene.find("Dialog Text"), "UIText", { text = text, visibleCharacters = 0 })
end

function Game:talk()
  if self.found >= self.total then
    self.lines = DONE
  elseif self.talked then
    self.lines = WAITING
  else
    self.lines = INTRO
  end
  self.talked = true
  self.line = 1
  game.set("dialog", true)
  scene.set(scene.find("Dialog"), "UIPanel", { visible = true })
  self:showLine()
end

function Game:advance()
  local text = scene.get(scene.find("Dialog Text"), "UIText").text
  if self.shown < #text then -- first press completes the line
    self.shown = #text
    return
  end
  self.line = self.line + 1
  if self.line <= #self.lines then
    self:showLine()
    return
  end
  game.set("dialog", false)
  scene.set(scene.find("Dialog"), "UIPanel", { visible = false })
  if self.lines == DONE and not self.finished then
    self.finished = true
    audio.play("sounds/success.wav", { volume = 0.8 })
    self:refreshQuest()
    -- The lanterns flare up for good.
    for _, id in ipairs(scene.all("PointLight")) do
      if scene.name(id):find("Lamp Light") then scene.send(id, "brighten") end
    end
  end
end

function Game:applyEffects()
  local look = self.look
  if self.effects then
    scene.set(self.camera, "PostProcess", {
      exposure = look.exposure, toneMapping = look.toneMapping, bloom = look.bloom, vignette = look.vignette,
      dofRadius = self.dof and look.dofRadius or 0 })
  else
    -- Plain scene: the sprites and lights without the lens. Exposure keeps the lit ground readable.
    scene.set(self.camera, "PostProcess", { exposure = 1, toneMapping = "none", bloom = 0, vignette = 0, dofRadius = 0 })
  end
end

function Game:onUpdate(dt)
  if input.pressed("1") then
    self.effects = not self.effects
    self:applyEffects()
  elseif input.pressed("2") then
    self.dof = not self.dof
    self:applyEffects()
  end

  local action = input.pressed("E") or input.pressed("Enter") or input.pressed("GamepadA")
  local prompt = scene.find("Prompt")
  if game.get("dialog") then
    scene.set(prompt, "UIText", { visible = false })
    self.shown = self.shown + 45 * dt -- typewriter: characters per second
    scene.set(scene.find("Dialog Text"), "UIText", { visibleCharacters = math.floor(self.shown) })
    if action then self:advance() end
    return
  end
  local hero = scene.get(self.hero, "Transform").position
  local elder = scene.get(self.elder, "Transform").position
  local dx, dz = hero.x - elder.x, hero.z - elder.z
  local near = dx * dx + dz * dz < 1.6 * 1.6
  scene.set(prompt, "UIText", { visible = near })
  if near and action then self:talk() end
end

return Game
