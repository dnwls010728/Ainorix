-- Attached to the "Play Again" UIButton: restart from the first level.
local PlayAgain = {}

function PlayAgain:onClick()
  game.set("total", 0)
  game.loadScene("scenes/main.scene.json")
end

return PlayAgain
