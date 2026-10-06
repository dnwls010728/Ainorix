-- Pure merge score rule; no save, effects, UI, or service dependency.
local Scoring = {}

function Scoring.mergePoints(base, combo)
  return math.floor(base * (1 + 0.25 * math.min(combo - 1, 6)) + 0.5)
end

return Scoring
