-- The courier: a side-view Skeleton2D that walks back and forth behind the adventurer, turning at the ends.
-- She moves at the speed her walk clip's planted foot slides back (skeleton2d.analyze ground.plantedSpeed: 1.17
-- world units per second at speed 1, times her scale), so her feet do not slide. Shift held makes her run (2.93).
-- params: { "left": -2.6, "right": 2.6, "walkSpeed": 1.17, "runSpeed": 2.93 }
local Courier = {}

function Courier:onStart()
  self.left = self.params.left or -2.6
  self.right = self.params.right or 2.6
  self.walkSpeed = self.params.walkSpeed or 1.17
  self.runSpeed = self.params.runSpeed or 2.93
  self.dir = 1
end

function Courier:onUpdate(dt)
  local running = input.down("Shift")
  local clip = running and "run" or "walk"
  if self:get("Skeleton2D").clip ~= clip then skeleton2d.play(self.id, clip, { mix = 0.25 }) end
  local t = self:get("Transform")
  t.position.x = t.position.x + self.dir * (running and self.runSpeed or self.walkSpeed) * dt
  if t.position.x > self.right then self.dir = -1 elseif t.position.x < self.left then self.dir = 1 end
  self:set("Transform", { position = t.position })
  self:set("Skeleton2D", { flipX = self.dir < 0 })
end

return Courier
