-- Billboards: turns tagged sprites toward the camera every frame. The engine draws a Sprite along its
-- entity's rotation; facing the camera is this project's choice.
--   tag "billboard"        stays upright and turns around world +Y (characters, trees, lamps)
--   tag "billboard-camera" fully faces the camera (pickups, glows)
-- The local rotation is written, so the parents of a billboard must not be rotated. Scripts run before
-- CameraFollow moves the camera, so sprites use the camera rotation of the previous frame.
-- params: { "camera": "Camera" }
local Billboards = {}

function Billboards:onStart()
  self.camera = scene.find(self.params.camera or "Camera") -- name of the camera entity to face
end

function Billboards:onUpdate()
  local view = scene.get(self.camera, "Transform").rotation
  for _, id in ipairs(scene.withTag("billboard")) do
    scene.set(id, "Transform", { rotation = { 0, view.y, 0 } })
  end
  for _, id in ipairs(scene.withTag("billboard-camera")) do
    scene.set(id, "Transform", { rotation = { view.x, view.y, 0 } })
  end
end

return Billboards
