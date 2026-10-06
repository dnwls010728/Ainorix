-- The chat field: Enter sends the text to the server as a "chat" request and leaves the field
-- (a submit takes the focus away by itself). While it has the focus the keyboard types, so the
-- avatar stands still.
local session = require("scripts.lib.session")
local Chatbox = {}

function Chatbox:onSubmit(text)
  if session.chatOpened == time.frame() then
    ui.focus(self.id) -- this is the Enter that opened the field (hud.lua): keep typing
    return
  end
  if text ~= "" then session.send("server", "chat", text) end
  self:set("UIInput", { text = "" })
end

return Chatbox
