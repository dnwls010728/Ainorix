-- Attached to the "Play again" UIButton on the results screen.
local PlayAgain = {}

function PlayAgain:onClick()
  game.loadScene(game.scene())
end

return PlayAgain
