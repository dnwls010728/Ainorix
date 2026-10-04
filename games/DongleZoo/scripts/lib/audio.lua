-- Sound effects and the music loop (sounds/*.wav, rendered by tools/make_audio.py).
local A = {}

local MIN_GAP = { merge = 0.03, drop = 0.05, click = 0.03, pop = 0.03, warn = 0.6 }
local MUSIC_VOLUME = 0.5

local sfxOn, musicOn, muted = true, true, false
local last = {}
local musicId = nil

function A.sfx(name, pitch, volume)
  if muted or not sfxOn then return end
  local now = time.frame() / 60 -- real frames: sounds also play while the game is paused
  if now - (last[name] or -1) < (MIN_GAP[name] or 0.02) then return end
  last[name] = now
  audio.play("sounds/" .. name .. ".wav", { volume = volume or 0.9, pitch = pitch or 1 })
end

local function applyMusic()
  local on = musicOn and not muted
  if on and not (musicId and scene.exists(musicId)) then
    musicId = scene.create("Music", { AudioSource = { clip = "sounds/music.wav", volume = MUSIC_VOLUME, loop = true } })
  elseif not on and musicId and scene.exists(musicId) then
    scene.destroy(musicId)
    musicId = nil
  end
end

function A.configure(sfx, music)
  sfxOn, musicOn = sfx, music
  applyMusic()
end

-- Silence everything while an ad plays.
function A.setMuted(m)
  muted = m
  applyMusic()
end

return A
