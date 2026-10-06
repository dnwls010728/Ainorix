-- Where things are: positions on the ground plane and where a world point shows on the screen.
local Space = {}

-- An entity's world position { x =, y =, z = }.
function Space.at(id)
  return scene.get(id, "Transform").position
end

-- Squared distance of two positions on the ground (x, z); heights do not count.
function Space.distance2(a, b)
  return (a.x - b.x) ^ 2 + (a.z - b.z) ^ 2
end

function Space.within(a, b, reach)
  return Space.distance2(a, b) <= reach ^ 2
end

-- World point -> actual effective canvas reference pixels; preserve offscreen labels.
function Space.project(cameraId, x, y, z)
  local screen = camera.worldToScreen({ camera = cameraId, position = { x = x, y = y, z = z } })
  if screen.status == "behind" or screen.depth < 0.1 then return nil end
  local point = ui.screenToCanvas({ screen = screen })
  return point.x, point.y
end

return Space
