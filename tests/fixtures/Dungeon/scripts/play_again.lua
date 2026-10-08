-- "Play again" button on the end screen.
local PlayAgain = {}

function PlayAgain:onClick()
  game.loadScene(game.scene())
end

return PlayAgain
