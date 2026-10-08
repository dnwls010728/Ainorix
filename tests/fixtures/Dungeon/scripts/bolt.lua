-- Magic bolt: a fast RigidBody2D (bullet) that hits once, pushing what it hits.
local Bolt = {}

function Bolt:onStart()
  self.life = self.params.life or 1.2
end

function Bolt:onUpdate(dt)
  self.life = self.life - dt
  if self.life <= 0 then self:destroy() end
end

function Bolt:onCollisionEnter(other)
  if self.done then return end
  self.done = true
  local tag = scene.get(other, "Tag")
  if tag and string.find(tag.tags, "enemy", 1, true) then
    scene.send(other, "hit", self:velocity())
  end
  self:destroy()
end

return Bolt
