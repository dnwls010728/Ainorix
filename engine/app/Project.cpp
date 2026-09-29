#include "app/Project.h"

#include "core/FileSystem.h"
#include "scene/Components.h"
#include "scene/Scene.h"

namespace oe {

Json MakeSampleScene(const std::string& name) {
    Scene s;
    s.name = name;

    EntityId cam = s.Create("Main Camera");
    s.Add<Transform>(cam) = Transform{{0, 4.5f, 9}, {-22, 0, 0}, {1, 1, 1}};
    s.Add<Camera>(cam);

    EntityId sun = s.Create("Sun");
    s.Add<Transform>(sun) = Transform{{0, 6, 0}, {-55, 35, 0}, {1, 1, 1}};
    s.Add<DirectionalLight>(sun);

    EntityId ground = s.Create("Ground");
    s.Add<Transform>(ground) = Transform{{0, 0, 0}, {0, 0, 0}, {16, 1, 16}};
    MeshRenderer& gm = s.Add<MeshRenderer>(ground);
    gm.mesh = "plane";
    gm.color = Color(0.36f, 0.5f, 0.34f);

    EntityId cube = s.Create("Spinning Cube");
    s.Add<Transform>(cube) = Transform{{-2.5f, 1, 0}, {0, 0, 0}, {1.2f, 1.2f, 1.2f}};
    s.Add<MeshRenderer>(cube).color = Color(0.9f, 0.35f, 0.25f);
    s.Add<Rotator>(cube).degreesPerSecond = Vec3(20, 60, 0);

    EntityId player = s.Create("Player");
    s.Add<Transform>(player) = Transform{{0, 0.5f, 2}, {0, 0, 0}, {1, 1, 1}};
    MeshRenderer& pm = s.Add<MeshRenderer>(player);
    pm.mesh = "sphere";
    pm.color = Color(0.25f, 0.55f, 0.95f);
    s.Add<PlayerController>(player);
    s.Add<Tag>(player).tags = "player";

    EntityId pyramid = s.Create("Pyramid");
    s.Add<Transform>(pyramid) = Transform{{2.8f, 0.75f, -1}, {0, 20, 0}, {1.5f, 1.5f, 1.5f}};
    MeshRenderer& pyr = s.Add<MeshRenderer>(pyramid);
    pyr.mesh = "pyramid";
    pyr.color = Color(0.95f, 0.8f, 0.3f);

    return s.ToJson();
}

bool CreateProject(const std::string& dir, const std::string& name, std::string* error) {
    if (FileExists(JoinPath(dir, "project.json"))) {
        if (error) *error = "a project already exists at " + dir;
        return false;
    }
    Json project = Json::MakeObject();
    project["format"] = "ownengine.project";
    project["version"] = 1;
    project["name"] = name;
    project["startScene"] = "scenes/main.scene.json";

    const char* agents =
        "# Working on this OwnEngine project (for AI agents)\n\n"
        "- Scenes are plain JSON in `scenes/`. You may edit them directly, but prefer the engine API so values are validated.\n"
        "- Inspect: `oe exec . scene.summary` / `oe exec . component.types`.\n"
        "- Edit:    `oe exec . entity.create '{\"name\":\"Box\",\"components\":{\"MeshRenderer\":{}}}' --save`.\n"
        "- Verify:  `oe render . --out shot.png` then look at the PNG; `--frames 120` simulates 2 seconds first.\n"
        "- Live:    `oe mcp .` exposes every command as an MCP tool (screenshots come back as images).\n"
        "- Human:   `oe editor .` opens the web editor on http://127.0.0.1:7777 (same API).\n";

    if (!WriteTextFile(JoinPath(dir, "project.json"), project.dump(2) + "\n") ||
        !WriteTextFile(JoinPath(dir, "scenes/main.scene.json"), MakeSampleScene("Main").dump(2) + "\n") ||
        !WriteTextFile(JoinPath(dir, "AGENTS.md"), agents)) {
        if (error) *error = "cannot write project files in " + dir;
        return false;
    }
    return true;
}

}  // namespace oe
