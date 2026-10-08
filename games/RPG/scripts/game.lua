-- The village director: time of day (light, sky, fog, colour grading, lamps, windows, motes, smoke,
-- sun shafts), the orbiting follow camera with its opening shot, billboards and the HUD.
-- Keys: Q/E turn the camera 45 degrees, mouse wheel zooms, R/F tilt, T stops/starts time, [ ] scrub time.
-- params: { "startTime": 0.46, "daySeconds": 320, "skipIntro": false }
local Atmosphere = require("scripts.lib.atmosphere")

local Game = {}

local INTRO = 5.6
local PI = math.pi
-- Engine light colours already include pi (a white surface lit head-on shows the light colour), the
-- prototype's three.js intensities did not.
local SUN_SCALE = 1 / PI
local AMBIENT_SCALE = 1.25 / PI
local LAMP_INTENSITY = 2.2

local function clamp(x, a, b) return math.max(a, math.min(b, x)) end
local function lerp(a, b, t) return a + (b - a) * t end
local function smooth01(x) local t = clamp(x, 0, 1) return t * t * (3 - 2 * t) end
local function easeInOut(x)
  local t = clamp(x, 0, 1)
  if t < 0.5 then return 4 * t * t * t end
  return 1 - ((-2 * t + 2) ^ 3) / 2
end
local function scale3(c, k) return {c[1] * k, c[2] * k, c[3] * k} end
local function mix3(a, b, t) return {lerp(a[1], b[1], t), lerp(a[2], b[2], t), lerp(a[3], b[3], t)} end
local function mul3(a, b) return {a[1] * b[1], a[2] * b[2], a[3] * b[3]} end
local function cross(a, b) return {a[2] * b[3] - a[3] * b[2], a[3] * b[1] - a[1] * b[3], a[1] * b[2] - a[2] * b[1]} end
local function normalize(v)
  local l = math.sqrt(v[1] * v[1] + v[2] * v[2] + v[3] * v[3])
  if l < 1e-8 then return {0, 1, 0} end
  return {v[1] / l, v[2] / l, v[3] / l}
end

-- Euler degrees (X, then Y, then Z) of a rotation whose local -Z points along `f`.
local function lookRotation(f)
  return {math.deg(math.asin(clamp(f[2], -1, 1))), math.deg(math.atan(-f[1], -f[3])), 0}
end

-- Euler degrees of the rotation with these local axes (columns of the matrix Rz * Ry * Rx).
local function basisRotation(x, y, z)
  local b = math.asin(clamp(-x[3], -1, 1))
  return {math.deg(math.atan(y[3], z[3])), math.deg(b), math.deg(math.atan(x[2], x[1]))}
end

local MOUNTAIN_BASE = {{0.5, 0.61, 0.7}, {0.31, 0.45, 0.4}, {0.18, 0.32, 0.25}}
local MOUNTAIN_MIX = {0.42, 0.55, 0.7}
local MOTE_DAY, MOTE_NIGHT = {1, 0.86, 0.45}, {0.35, 1, 0.3}

function Game:onStart()
  local p = self.params
  self.tod = p.startTime or 0.46
  self.daySpeed = 1 / (p.daySeconds or 320)
  self.speed = self.daySpeed
  self.introT = p.skipIntro and INTRO + 3 or 0
  self.yaw, self.targetYaw = 0, 0
  self.dist, self.pitch = 25, math.rad(33)
  self.elapsed = 0
  self.camera = scene.find("Camera")
  self.sun = scene.find("Sun")
  self.sky = scene.find("Sky")
  self.clouds = scene.find("Clouds")
  self.windows = scene.find("Windows")
  self.water = scene.find("Water")
  self.plants = scene.find("Plants")
  self.motes = scene.find("Motes")
  self.mountains = scene.withTag("mountains")
  self.smoke = scene.withTag("smoke")
  self.shafts = scene.withTag("shaft")
  self.halos = scene.withTag("halo")
  self.lamps = scene.withTag("lamplight")
  self.billboards = scene.withTag("billboard")
  self.fade = scene.find("Fade")
  self.clock = scene.find("Clock")
  self.help = scene.find("Help")
  self.banner = {scene.find("BannerSub"), scene.find("BannerTitle"), scene.find("BannerRule")}
  self.bannerShown, self.hudShown, self.clockTimer = p.skipIntro == true, false, 0
  local hero = scene.find("Hero")
  local hp = scene.get(hero, "Transform").position
  self.focus = {hp.x, hp.y + 0.9, hp.z}
  self.fov = 24
  game.set("camYaw", 0)
  game.set("locked", true)
end

function Game:controls(dt)
  if input.pressed("Q") then self.targetYaw = self.targetYaw + PI / 4 end
  if input.pressed("E") then self.targetYaw = self.targetYaw - PI / 4 end
  if input.pressed("T") then self.speed = self.speed == 0 and self.daySpeed or 0 end
  local wheel = input.wheel()
  if wheel ~= 0 then self.dist = clamp(self.dist - wheel * 1.4, 13, 42) end
  if input.down("R") then self.pitch = clamp(self.pitch + dt * 0.7, math.rad(20), math.rad(62)) end
  if input.down("F") then self.pitch = clamp(self.pitch - dt * 0.7, math.rad(20), math.rad(62)) end
  if input.down("RightBracket") then self.tod = self.tod + dt * 0.1 end
  if input.down("LeftBracket") then self.tod = self.tod - dt * 0.1 end
  local rx = input.axis("RightX")
  if math.abs(rx) > 0.5 and not self.stickHeld then
    self.targetYaw = self.targetYaw + (rx > 0 and -PI / 4 or PI / 4)
    self.stickHeld = true
  elseif math.abs(rx) < 0.3 then
    self.stickHeld = false
  end
end

function Game:applyAtmosphere(a, eye)
  local tone = {
    a.sunColor[1] * a.sunI * 0.2 + a.hemiSky[1] * a.hemiI * 0.45,
    a.sunColor[2] * a.sunI * 0.2 + a.hemiSky[2] * a.hemiI * 0.45,
    a.sunColor[3] * a.sunI * 0.2 + a.hemiSky[3] * a.hemiI * 0.45,
  }
  local sd = a.sunDir
  scene.set(self.sun, "Transform", {rotation = lookRotation({-sd[1], -sd[2], -sd[3]})})
  scene.set(self.sun, "DirectionalLight", {
    color = a.sunColor, intensity = a.sunI * SUN_SCALE,
    ambient = scale3(a.hemiSky, a.hemiI * AMBIENT_SCALE), groundAmbient = scale3(a.hemiGround, a.hemiI * AMBIENT_SCALE),
  })
  scene.set(self.camera, "Camera", {clearColor = a.haze})
  scene.set(self.camera, "PostProcess", {
    exposure = a.exposure, bloom = a.bloom * 0.9, vignette = a.vignette * 0.9, saturation = a.sat,
    shadowTint = a.shadowTint, highlightTint = a.highTint, fogColor = a.haze,
    fogStart = math.max(26, self.camDist * 0.95), fogEnd = math.max(26, self.camDist * 0.95) + 44,
    dofFocus = self.camDist,
  })
  scene.set(self.sky, "MeshRenderer", {shaderUniforms = {
    horizon = {a.haze[1], a.haze[2], a.haze[3], 1}, zenith = {a.zenith[1], a.zenith[2], a.zenith[3], 1},
    sunDir = {sd[1], sd[2], sd[3], a.stars}, sunColor = {a.sunColor[1], a.sunColor[2], a.sunColor[3], 0},
  }})
  scene.set(self.clouds, "MeshRenderer", {shaderUniforms = {
    sunDir = {sd[1], sd[2], sd[3], 0},
    cloudLit = {a.cloudLit[1], a.cloudLit[2], a.cloudLit[3], 1}, cloudShade = {a.cloudShade[1], a.cloudShade[2], a.cloudShade[3], 1},
  }})
  for i, id in ipairs(self.mountains) do
    local tint = mul3(MOUNTAIN_BASE[i], tone)
    scene.set(id, "MeshRenderer", {color = mix3(a.haze, tint, MOUNTAIN_MIX[i])})
  end
  scene.set(self.windows, "MeshRenderer", {shaderUniforms = {glow = 0.15 + 1.6 * a.lamp}})
  scene.set(self.water, "MeshRenderer", {shaderUniforms = {light = {tone[1], tone[2], tone[3], 1}}})
  for _, id in ipairs(self.halos) do scene.set(id, "Sprite", {opacity = clamp(a.lamp * 0.7, 0, 1)}) end
  local shaft = clamp(a.shaft * (1 - a.stars * 0.7) * 0.16, 0, 1)
  for _, id in ipairs(self.shafts) do scene.set(id, "Sprite", {opacity = shaft, color = scale3(a.sunColor, 1.7)}) end
  vfx.set(self.motes, "color", mix3(MOTE_DAY, MOTE_NIGHT, a.fireflies))
  vfx.set(self.motes, "size", 0.12 + a.fireflies * 0.07)
  local smoke = scale3(mix3(a.haze, {1, 0.95, 0.9}, 0.3), 0.55 + 0.45 * clamp(a.hemiI, 0, 1))
  for _, id in ipairs(self.smoke) do vfx.set(id, "color", smoke) end
  self.lampLevel = a.lamp
end

function Game:placeShafts(sd, eye)
  local axis = normalize({-sd[1], -sd[2], -sd[3]})
  local n = #self.shafts
  for i, id in ipairs(self.shafts) do
    local k = i - 1
    local ang = k / n * PI * 2 + k * 1.9
    local r = 4 + (k % 4) * 3.2
    local c = {self.focus[1] + math.cos(ang) * r + sd[1] * 9, self.focus[2] + 8, self.focus[3] + math.sin(ang) * r + sd[3] * 9}
    local toCam = {eye[1] - c[1], eye[2] - c[2], eye[3] - c[3]}
    local right = normalize(cross(axis, toCam))
    local normal = cross(right, axis)
    scene.set(id, "Transform", {position = c, rotation = basisRotation(right, axis, normal)})
  end
end

function Game:onUpdate(dt)
  self.elapsed = self.elapsed + dt
  self.introT = self.introT + dt
  self:controls(dt)
  self.tod = self.tod + self.speed * dt

  local e = easeInOut(self.introT / INTRO)
  local locked = self.introT < INTRO * 0.72
  game.set("locked", locked)
  self.yaw = self.yaw + (self.targetYaw - self.yaw) * math.min(1, dt * 7)
  game.set("camYaw", self.yaw)

  local hero = game.get("hero") or {x = self.focus[1], y = self.focus[2] - 0.9, z = self.focus[3], vx = 0, vz = 0}
  local pitch = lerp(math.rad(6), self.pitch, e)
  local dist = lerp(58, self.dist, e)
  local yaw = self.yaw + lerp(-0.7, 0, e)
  local lift = lerp(7, 0, e)
  self.camDist = dist
  local k = math.min(1, dt * 6)
  local target = {hero.x + hero.vx * 0.28, hero.y + 0.9 + lift, hero.z + hero.vz * 0.28}
  for i = 1, 3 do self.focus[i] = self.focus[i] + (target[i] - self.focus[i]) * k end
  local fovTarget = (hero.running and not locked) and 26.5 or 24
  self.fov = self.fov + (fovTarget - self.fov) * math.min(1, dt * 3)
  local f = self.focus
  local eye = {
    f[1] + math.sin(yaw) * math.cos(pitch) * dist,
    f[2] + math.sin(pitch) * dist,
    f[3] + math.cos(yaw) * math.cos(pitch) * dist,
  }
  scene.set(self.camera, "Transform", {position = eye, rotation = {-math.deg(pitch), math.deg(yaw), 0}})
  scene.set(self.camera, "Camera", {fov = self.fov})

  local a = Atmosphere.sample(self.tod)
  self:applyAtmosphere(a, eye)

  -- backdrops stay centred on the camera
  scene.set(self.sky, "Transform", {position = {eye[1], 0, eye[3]}})
  scene.set(self.clouds, "Transform", {position = {eye[1], 0, eye[3]}})
  for _, id in ipairs(self.mountains) do
    local p = scene.get(id, "Transform").position
    scene.set(id, "Transform", {position = {eye[1], p.y, eye[3]}})
  end
  scene.set(self.motes, "Transform", {position = {f[1], f[2] + 1.2, f[3]}})
  scene.set(self.plants, "MeshRenderer", {shaderUniforms = {hero = {hero.x, hero.y, hero.z, 0}}})
  self:placeShafts(a.sunDir, eye)

  local yawDeg = math.deg(yaw)
  for _, id in ipairs(self.billboards) do scene.set(id, "Transform", {rotation = {0, yawDeg, 0}}) end

  local t = self.elapsed
  for _, id in ipairs(self.lamps) do
    local ph = id * 1.7
    local flicker = 0.9 + 0.1 * math.sin(t * 9 + ph) + 0.05 * math.sin(t * 23 + ph * 2)
    scene.set(id, "PointLight", {intensity = LAMP_INTENSITY * self.lampLevel * flicker})
  end

  self:hud(dt)
end

function Game:hud(dt)
  scene.set(self.fade, "UIPanel", {opacity = 1 - smooth01(self.introT / 1.4)})
  if not self.bannerShown and self.introT > 1.8 then
    self.bannerShown = true
    for _, id in ipairs(self.banner) do
      local kind = scene.has(id, "UIText") and "UIText" or "UIPanel"
      tween.to(id, kind, {opacity = kind == "UIPanel" and 0.8 or 1}, 1.6, {ease = "inOutQuad"})
      tween.to(id, kind, {opacity = 0}, 1.6, {ease = "inOutQuad", delay = 4.2})
    end
  end
  if not self.hudShown and self.introT > INTRO then
    self.hudShown = true
    tween.to(self.help, "UIText", {opacity = 0.72}, 1.2)
    tween.to(self.clock, "UIText", {opacity = 0.8}, 1.2)
  end
  self.clockTimer = self.clockTimer - dt
  if self.clockTimer <= 0 then
    self.clockTimer = 0.5
    local hrs = ((self.tod % 1) * 24 + 5) % 24
    local text = string.format("%s %02d:%02d", Atmosphere.label(self.tod), math.floor(hrs), math.floor((hrs % 1) * 60))
    if self.speed == 0 then text = text .. " · 정지" end
    scene.set(self.clock, "UIText", {text = text})
  end
end

return Game
