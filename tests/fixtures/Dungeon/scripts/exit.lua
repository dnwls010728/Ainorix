-- Stairs down (Collider2D trigger): the game decides whether the hero may leave.
local Exit = {}

function Exit:onTriggerEnter(other)
  local tag = scene.get(other, "Tag")
  if tag and string.find(tag.tags, "player", 1, true) then scene.broadcast("onExit") end
end

return Exit
