"""Exercise keeper selection, animated prefabs and render both runtime backends."""
import base64
import json
from pathlib import Path
import subprocess

PROJECT = Path(__file__).resolve().parents[1]
REPOSITORY = PROJECT.parents[1]
OUTPUT = REPOSITORY / "build/wickbound-sprites/verification"


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for keeper in ("ada", "bram", "suri"):
        calls = []

        def call(command, **args):
            calls.append({"command": command, "args": args})

        call("sim.step", frames=3)
        call("script.eval", code=f"game.set('run', {{keeperId = '{keeper}'}})")
        call("game.load_scene", path="scenes/run.scene.json")
        call("sim.step", frames=3)
        call("script.eval", code="local W = require('scripts.lib.world') W.player.iframes = 1000 "
             "local a = scene.get(W.player.body, 'SpriteAnimation') "
             "return {texture = scene.get(W.player.body, 'Sprite').texture, animated = a ~= nil, clip = a and a.clip or 'static'}")
        call("input.key", key="D", down=True)
        call("sim.step", frames=12)
        call("script.eval", code="local W = require('scripts.lib.world') "
             "local a = scene.get(W.player.body, 'SpriteAnimation') "
             "return {clip = a and a.clip or 'static', frame = scene.get(W.player.body, 'Sprite').frame, frames = a and a.clips.move.frames or {0}}")
        call("input.key", key="D", down=False)
        call("sim.step", frames=2)
        call("script.eval", code="local W = require('scripts.lib.world') "
             "for i,kind in ipairs({'shade','skitter','brute','wisp','gloom','gloomlet','stalker','leech','king','eclipse'}) do "
             "local x = W.player.x + ((i-1)%5-2)*3.8 local y = W.player.y + (i<=5 and 3 or -3) "
             "W.spawn('enemy_'..kind, x, y, {kind=kind}) scene.create('PreviewLight'..i) "
             "scene.set(scene.find('PreviewLight'..i), 'Transform', {position={x=x,y=y}}) "
             "scene.add(scene.find('PreviewLight'..i), 'Light2D', {radius=4}) end "
             "W.braziers[1]:ignite() "
             "local static = {skitter=true,gloom=true,gloomlet=true,stalker=true,leech=true} "
             "for _, e in ipairs(W.enemyList) do if static[e.kind] then "
             "assert(not scene.has(e.body,'SpriteAnimation')) "
             "local name = e.kind == 'gloomlet' and 'gloom' or e.kind "
             "assert(scene.get(e.body,'Sprite').texture == 'assets/sprites/enemy_'..name..'.png') end end "
             "assert(not scene.has(scene.child(W.braziers[1].id,'Body'),'SpriteAnimation')) "
             "assert(scene.get(scene.child(W.braziers[1].id,'Fire'),'Sprite').visible)")
        call("sim.step", frames=12)
        for renderer in ("software", "gpu"):
            call("render.screenshot", width=1280, height=720, renderer=renderer, inline=True)
        call("script.errors")
        result = subprocess.run([str(REPOSITORY / "build/bin/oe.exe"), "script", str(PROJECT)],
                                input="\n".join(map(json.dumps, calls)) + "\n",
                                text=True, encoding="utf-8", capture_output=True, check=True)
        responses = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        for response in responses:
            if not response.get("ok"):
                raise RuntimeError(response)
        assert len(responses) == len(calls)
        assert responses[-1]["result"] == [], "Runtime Lua errors"
        idle = responses[4]["result"]["value"]
        moving = responses[7]["result"]["value"]
        folder = "assets/sprites/" if keeper == "suri" else "assets/sprites/animated/"
        assert idle["texture"] == folder + f"keeper_{keeper}.png"
        if keeper == "suri":
            assert not idle["animated"] and idle["clip"] == moving["clip"] == "static"
            assert moving["frame"] == 0
        else:
            assert idle["animated"] and idle["clip"] == "idle" and moving["clip"] == "move"
        assert moving["frame"] in moving["frames"]
        shots = [response["result"] for response in responses if "png_base64" in response.get("result", {})]
        assert len(shots) == 2
        for renderer, shot in zip(("software", "gpu"), shots):
            (OUTPUT / f"{keeper}-{renderer}.png").write_bytes(base64.b64decode(shot["png_base64"]))
        (OUTPUT / f"{keeper}.json").write_text(json.dumps([
            {"command": request["command"], "result": {k: v for k, v in response["result"].items() if k != "png_base64"}
             if isinstance(response["result"], dict) else response["result"]}
            for request, response in zip(calls, responses)], indent=2) + "\n", encoding="utf-8")
        mode = "single sprite" if keeper == "suri" else "idle/move"
        print(f"PASS {keeper}: {mode}, static prefab guards, lit campfire, software/GPU screenshots")


if __name__ == "__main__":
    main()
