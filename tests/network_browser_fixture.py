"""Prepare a disposable browser/native-server networking fixture (Python stdlib only).

Run from the repository root, then open the printed URL in a browser.
Ctrl+C stops both servers. The network project and package stay under build/.
"""
import argparse
import functools
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample", choices=["NetCoop", "NetDuel", "NetArena"], default="NetArena")
    parser.add_argument("--http-port", type=int, default=8098)
    parser.add_argument("--game-port", type=int, default=7778)
    parser.add_argument("--api-port", type=int, default=7798)
    parser.add_argument("--test-controls", action="store_true", help="Add a DOM button emitting a bounded 450 ms A+W input")
    args = parser.parse_args()
    executable = ROOT / "build/bin" / ("oe.exe" if os.name == "nt" else "oe")
    project = ROOT / "build/network-browser" / args.sample
    output = ROOT / "build/network-browser" / (args.sample + "-web")
    shutil.copytree(ROOT / "samples" / args.sample, project, dirs_exist_ok=True)
    settings = json.loads((project / "project.json").read_text(encoding="utf-8"))
    settings["network"].update(transport="websocket", port=args.game_port)
    (project / "project.json").write_text(json.dumps(settings, indent=2) + chr(10), encoding="utf-8")
    scene_path = project / "scenes/main.scene.json"
    scene = json.loads(scene_path.read_text(encoding="utf-8"))
    for entity in scene["entities"]:
        script = entity["components"].get("Script", {})
        if script.get("path") == "scripts/lobby.lua":
            script["params"]["port"] = args.game_port
        if entity["name"] == "Join":
            script["params"]["address"] = "ws://127.0.0.1:" + str(args.game_port)
    scene_path.write_text(json.dumps(scene, indent=2) + chr(10), encoding="utf-8")
    subprocess.run([str(executable), "package", str(project), "--web", "--out", str(output)], check=True)
    if args.test_controls:
        index = output / "index.html"
        controls = """<div style="position:fixed;right:0;top:0;z-index:10000;background:white;padding:5px">
<button id="networkMove">Move to crystal</button></div>
<script>
document.getElementById('networkMove').onclick=function(){
  function emit(type, code, key){window.dispatchEvent(new KeyboardEvent(type,{code:code,key:key,bubbles:true}));}
  emit('keydown','KeyA','a'); emit('keydown','KeyW','w');
  setTimeout(function(){emit('keyup','KeyA','a');emit('keyup','KeyW','w');},450);
};
</script>"""
        index.write_text(index.read_text(encoding="utf-8").replace("</body>", controls + "</body>"), encoding="utf-8")
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(output))
    http_server = http.server.ThreadingHTTPServer(("127.0.0.1", args.http_port), handler)
    with (output.parent / (args.sample + "-server.log")).open("w", encoding="utf-8") as log:
        server = subprocess.Popen([str(executable), "serve-game", str(project), "--api-port", str(args.api_port)], stdout=log, stderr=log)
        try:
            threading.Thread(target=http_server.serve_forever, daemon=True).start()
            print(json.dumps({"url": "http://127.0.0.1:" + str(args.http_port), "serverPid": server.pid,
                              "steps": "Join, Ready, move to the gold crystal, Collect, Reset; inspect server via /api/call."}), flush=True)
            code = server.wait()
            if code:
                raise subprocess.CalledProcessError(code, server.args)
        except KeyboardInterrupt:
            pass
        finally:
            http_server.shutdown()
            http_server.server_close()
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    main()
