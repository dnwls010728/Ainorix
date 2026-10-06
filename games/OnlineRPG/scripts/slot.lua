-- A bag button. index 1..8: a bag slot, a click asks the server to drop one item of it.
-- index 0: the Bag button, which shows or hides the panel.
-- params: { "index": 1 }
local session = require("scripts.lib.session")
local Slot = {}

function Slot:onClick()
  local index = self.params.index or 0
  if index == 0 then
    scene.send(scene.find("HUD"), "toggleBag")
  else
    session.send("server", "drop", index)
  end
end

return Slot
