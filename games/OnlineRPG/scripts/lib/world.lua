-- Written by tools/make_scene.py: where players may walk and what the world starts with.
-- bounds = {minX, maxX, minZ, maxZ}; circles = {x, z, radius}; boxes = {x, z, halfWidth, halfDepth};
-- spawns = {x, z} by player number; items = {key, x, z}.
local World = {
  bounds = {-15, 15, -2.6, 13.5},
  circles = {{-13.5, 2.5, 0.55}, {-6, 11.5, 0.55}, {8.5, 11, 0.55}, {12.5, 1.5, 0.55}, {13.5, 8, 0.55}, {-12.5, 12, 0.55}, {-4.6, 3.4, 0.3}, {4.6, 3.4, 0.3}, {-4.6, 8.6, 0.3}, {4.6, 8.6, 0.3}, {-1.8, 12.5, 0.3}, {1.8, -0.6, 0.3}},
  boxes = {{-9.5, 8, 2.8, 1.8}},
  spawns = {{0.0, 7.8}, {1.27, 7.27}, {1.8, 6.0}, {1.27, 4.73}, {0.0, 4.2}, {-1.27, 4.73}, {-1.8, 6.0}, {-1.27, 7.27}},
  items = {{"potion", -2.5, 3.5}, {"herb", 3, 4}, {"coin", 0, 9.5}, {"gem", 6.5, 6}, {"coin", -3, 11}, {"herb", -6.5, 5}, {"potion", 9, 9}, {"gem", -11, 3}, {"coin", 4, 0.5}, {"herb", 11, 4.5}},
}

-- Moves a walker of `radius` from (x, z) by (dx, dz) and returns where it ends up.
function World.move(x, z, dx, dz, radius)
  x, z = x + dx, z + dz
  for _, c in ipairs(World.circles) do
    local ox, oz = x - c[1], z - c[2]
    local d, reach = math.sqrt(ox * ox + oz * oz), c[3] + radius
    if d < reach and d > 1e-4 then x, z = c[1] + ox / d * reach, c[2] + oz / d * reach end
  end
  for _, b in ipairs(World.boxes) do
    local ox, oz = x - b[1], z - b[2]
    local px, pz = b[3] + radius - math.abs(ox), b[4] + radius - math.abs(oz)
    if px > 0 and pz > 0 then -- inside: leave through the nearer side
      if px < pz then x = b[1] + (ox < 0 and -1 or 1) * (b[3] + radius)
      else z = b[2] + (oz < 0 and -1 or 1) * (b[4] + radius) end
    end
  end
  local m = World.bounds
  return math.max(m[1], math.min(m[2], x)), math.max(m[3], math.min(m[4], z))
end

return World
