-- Follow camera of the local player. Unreplicated, so every peer runs its own copy; it never
-- turns, which is why the sprites of this game need no billboard script.
-- params: { "offset": [0, 6.2, 8.8], "lookHeight": 0.9, "smoothing": 6 }
local session = require("scripts.lib.session")
local Camera = {}

function Camera:onStart()
  self.offset = self.params.offset or { 0, 6.2, 8.8 } -- from the player to the camera
  self.lookHeight = self.params.lookHeight or 0.9     -- height above the feet the camera looks at
  self.smoothing = self.params.smoothing or 6         -- higher follows faster
  self.followed = nil
end

function Camera:onUpdate(dt)
  local id = session.avatar(session.me())
  if not id then return end -- lobby or dedicated server: keep the overview
  local target = scene.get(id, "Transform").position
  local o = self.offset
  local x, y, z = target.x + o[1], target.y + o[2], target.z + o[3]
  if self.followed == id then
    local now = self:get("Transform").position
    local k = math.min(1, self.smoothing * dt)
    x, y, z = now.x + (x - now.x) * k, now.y + (y - now.y) * k, now.z + (z - now.z) * k
  end
  self.followed = id -- a new avatar (match start) is framed at once
  self:set("Transform", {
    position = { x = x, y = y, z = z },
    rotation = { x = -math.deg(math.atan(o[2] - self.lookHeight, o[3])), y = 0, z = 0 } })
end

return Camera
