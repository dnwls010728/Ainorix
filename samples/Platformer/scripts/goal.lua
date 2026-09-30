-- Invisible trigger around the flag pole (created by level.lua for the "G" marker).
local Goal = {}

function Goal:onTriggerEnter(other)
  local tag = scene.get(other, "Tag")
  if tag and string.find(tag.tags, "player", 1, true) then scene.broadcast("onGoal") end
end

return Goal
