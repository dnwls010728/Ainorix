-- A button that calls a method of another entity's script: the Login / Offline buttons and the
-- popup's OK call connect.lua on the "Net" entity.
-- params: { "target": "Net", "method": "login" }
local Button = {}

function Button:onClick()
  local target = scene.find(self.params.target or "Net")
  if target then scene.send(target, self.params.method or "confirm") end
end

return Button
