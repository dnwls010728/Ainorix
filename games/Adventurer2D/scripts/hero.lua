-- The adventurer: a Skeleton2D character (docs/SKELETON2D.md) driven by its clips.
-- Left/Right walk (the front-view walk steps in place while the body moves), Space jumps, J attacks, H is hit,
-- W held waves, K falls, R stands up again. One-shot clips play to their end before anything else starts.
-- params: { "speed": 1.2 }
local Hero = {}

-- Keys that start a one-shot clip, checked in this order.
local oneShots = { { key = "Space", clip = "jump" }, { key = "J", clip = "attack" }, { key = "H", clip = "hit" } }

function Hero:onStart()
  self.speed = self.params.speed or 1.2
  self.busy = false -- a one-shot clip is playing
  self.down = false -- fallen (the death clip); R stands up
end

function Hero:play(clip, mix)
  skeleton2d.play(self.id, clip, { mix = mix or 0.15, restart = true })
end

function Hero:onUpdate(dt)
  if self.down then
    if input.pressed("R") then
      self.down = false
      self:play("idle", 0.4)
    end
    return
  end
  if input.pressed("K") then
    self.down = true
    self:play("death", 0.1)
    return
  end
  if self.busy then
    if not skeleton2d.state(self.id).finished then return end
    self.busy = false
  end
  for _, shot in ipairs(oneShots) do
    if input.pressed(shot.key) then
      self.busy = true
      self:play(shot.clip, 0.08)
      return
    end
  end

  local dir = 0
  if input.down("Left") or input.down("A") then dir = dir - 1 end
  if input.down("Right") or input.down("D") then dir = dir + 1 end
  local wanted = "idle"
  if dir ~= 0 then
    wanted = "walk"
    local t = self:get("Transform")
    t.position.x = math.max(-3, math.min(3, t.position.x + dir * self.speed * dt))
    self:set("Transform", { position = t.position })
    self:set("Skeleton2D", { flipX = dir < 0 })
  elseif input.down("W") then
    wanted = "wave"
  end
  if self:get("Skeleton2D").clip ~= wanted then skeleton2d.play(self.id, wanted, { mix = 0.2 }) end
end

-- Clip events from the skeleton asset (step, takeoff, land, hit).
function Hero:onAnimationEvent(name, clip)
  if name == "hit" then log.info("attack lands") end
end

return Hero
