-- Sound effects and music. The WAV files in sounds/ are rendered by tools/make_audio.py
-- from the original's synthesizer definitions.
local A = {}

local MIN_GAP = { shoot = 0.06, hit = 0.05, kill = 0.045, pickup = 0.04, zap = 0.06, click = 0.03, hurt = 0.08 }
local FADE = 0.8
local SFX_GAIN, MUSIC_GAIN = 1.6, 1.4 -- the recordings keep the original's quiet per-voice levels

local musicVol, sfxVol, muted = 0.6, 0.8, false
local last = {}
local pickupCombo, lastPickup = 0, -1
local voices = {} -- { id = entity, level = 0..1 fade, live = bool }
local playing = nil

function A.sfx(name)
  if muted or sfxVol <= 0 then return end
  local now = time.now()
  if now - (last[name] or -1) < (MIN_GAP[name] or 0.02) then return end
  last[name] = now
  local pitch = 1
  if name == "pickup" then
    -- quick pickups in a row climb a semitone each
    if now - lastPickup > 0.45 then pickupCombo = 0 else pickupCombo = math.min(pickupCombo + 1, 10) end
    lastPickup = now
    pitch = 2 ^ (pickupCombo / 12)
  end
  audio.play("sounds/" .. name .. ".wav", { volume = math.min(2, sfxVol * SFX_GAIN), pitch = pitch })
end

local function voiceVolume(v)
  return muted and 0 or math.min(2, musicVol * MUSIC_GAIN * v.level)
end

-- track = "menu" | "run" | "boss" | nil; cross-fades from the current one.
function A.music(track)
  if playing == track then return end
  playing = track
  for _, v in ipairs(voices) do v.live = false end
  if track then
    local v = { level = 0, live = true }
    v.id = scene.create("wb:music", { AudioSource = { clip = "sounds/music_" .. track .. ".wav", volume = 0, loop = true } })
    voices[#voices + 1] = v
  end
end

-- Entities do not survive a scene change: forget the music voices of the old scene.
function A.sceneChanged()
  voices = {}
  playing = nil
end

function A.update(dt)
  local j = 1
  for i = 1, #voices do
    local v = voices[i]
    v.level = math.max(0, math.min(1, v.level + (v.live and dt or -dt) / FADE))
    if not scene.exists(v.id) then
      v.live, v.level = false, 0
    elseif v.live or v.level > 0 then
      scene.set(v.id, "AudioSource", { volume = voiceVolume(v) })
      voices[j] = v
      j = j + 1
    elseif scene.exists(v.id) then
      scene.destroy(v.id)
    end
  end
  for i = j, #voices do voices[i] = nil end
end

function A.setVolumes(music, sfx)
  musicVol = math.max(0, math.min(1, music))
  sfxVol = math.max(0, math.min(1, sfx))
end

-- Silence everything while a (mock) ad plays.
function A.setMuted(m)
  muted = m
end

return A
