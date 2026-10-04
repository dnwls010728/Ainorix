-- Sends a UIButton click to the App script: params { a = action, b = argument } -> App:onAction(a, b).
local Button = {}

function Button:onStart()
  self.target = scene.find(self.params.target or "App")
end

function Button:onClick()
  scene.send(self.target, "onAction", self.params.a, self.params.b)
end

return Button
