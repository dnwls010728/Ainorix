-- Sends a UIButton click or UISlider drag to another entity's script:
-- params { target = entity name, a = action, b = argument } -> target:onAction(a, b).
local Button = {}

function Button:onStart()
  self.target = scene.find(self.params.target or "Menu")
end

function Button:onClick()
  scene.send(self.target, "onAction", self.params.a, self.params.b)
end

function Button:onValueChanged(value)
  scene.send(self.target, "onAction", self.params.a, value)
end

return Button
