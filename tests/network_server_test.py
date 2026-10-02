"""Real dedicated CLI/server/player smoke. Python stdlib; WebSocket case needs Node >=22."""
import json
from pathlib import Path
import queue
import shutil
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build/bin/oe.exe"

class JsonProcess:
    def __init__(self, arguments):
        self.process = subprocess.Popen([str(x) for x in arguments], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.results = queue.Queue()
        def read():
            text = ""
            for line in self.process.stdout:
                text += line.decode("utf-8")
                try:
                    value = json.loads(text)
                except json.JSONDecodeError:
                    continue
                self.results.put(value)
                text = ""
            self.results.put(None)
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
    def next(self):
        value = self.results.get(timeout=15)
        assert value is not None, "process ended before JSON response"
        return value
    def call(self, command, arguments=None):
        self.process.stdin.write((json.dumps({"command": command, "args": arguments or {}}) + "\n").encode())
        self.process.stdin.flush()
        response = self.next()
        assert response["ok"], response
        return response["result"]
    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=10)
        self.process.stdin.close()
        self.reader.join(timeout=2)
        self.process.stdout.close()

def api(port, command):
    body = json.dumps({"command": command, "args": {}}).encode()
    request = urllib.request.Request(f"http://127.0.0.1:{port}/api/call", body,
                                    {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=5) as response:
        value = json.load(response)
    assert value["ok"], value
    return value["result"]

with tempfile.TemporaryDirectory(prefix="network-server-", dir=ROOT / "build") as temporary:
    project = Path(temporary)
    subprocess.run([str(EXE), "new", str(project), "--name", "DedicatedSmoke"], capture_output=True, check=True)
    settings = json.loads((project / "project.json").read_text(encoding="utf-8"))
    settings["network"] = {"mode": "authoritative", "transport": "tcp", "port": 0,
        "gameId": "server-smoke", "actions": ["W"], "playerPrefab": "prefabs/network.prefab.json"}
    for scene in (project / "scenes").glob("*.scene.json"):
        data = json.loads(scene.read_text(encoding="utf-8"))
        data["entities"] = []
        scene.write_text(json.dumps(data), encoding="utf-8")
    (project / "scripts/network.lua").write_text("local M={}\nfunction M:onUpdate() local p=self:get('NetPlayer').player; local t=self:get('Transform'); if input.player(p).down('W') then t.position.x=t.position.x+1; self:set('Transform',t) end end\nreturn M", encoding="utf-8")
    prefab = {"format": "ownengine.prefab", "version": 1, "name": "Player", "entities": [{"id": 1,
        "name": "Player", "components": {"Transform": {}, "Script": {"path": "scripts/network.lua"}, "NetPlayer": {}, "NetSync": {}}}]}
    (project / "prefabs/network.prefab.json").write_text(json.dumps(prefab), encoding="utf-8")
    # Copy outside the server resource tree: ContentHash includes all non-tool resources.
    with tempfile.TemporaryDirectory(prefix="network-client-", dir=ROOT / "build") as client_temporary:
        client_project = Path(client_temporary)
        for item in project.iterdir():
            if item.is_dir(): shutil.copytree(item, client_project / item.name)
            else: shutil.copy2(item, client_project / item.name)
        for transport in ("tcp", "udp", "websocket"):
            settings["network"]["transport"] = transport
            (project / "project.json").write_text(json.dumps(settings), encoding="utf-8")
            client_settings = json.loads(json.dumps(settings)); client_settings["network"]["transport"] = "tcp" if transport == "websocket" else transport
            (client_project / "project.json").write_text(json.dumps(client_settings), encoding="utf-8")
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0)); api_port = reservation.getsockname()[1]
            server = JsonProcess([EXE, "serve-game", project, "--frames", "210", "--seed", "71", "--api-port", api_port])
            client = bridge = None
            try:
                startup = server.next(); assert startup["ok"], startup
                state = startup["result"]; assert state["dedicated"] and state["isServer"] and not state["isHost"]
                port = state["port"]; assert port > 0
                if transport == "websocket":
                    bridge = JsonProcess(["node", ROOT / "tests/network_ws_bridge.js", f"ws://127.0.0.1:{port}/game"])
                    port = bridge.next()["port"]
                client = JsonProcess([EXE, "script", client_project])
                client.call("net.join", {"port": port})
                for _ in range(30): client.call("sim.step", {"frames": 1}); time.sleep(1 / 60)
                assert api(api_port, "net.state")["sync"]["state"] == "idle"
                client.call("net.ready", {"ready": True}); client.call("input.key", {"key": "W", "down": True})
                for _ in range(110): client.call("sim.step", {"frames": 1}); time.sleep(1 / 60)
                assert client.call("net.state")["sync"]["state"] == "running"
                entities = client.call("net.entities"); assert len(entities) == 1, entities
                assert entities[0]["owner"] == 2, entities
                world = client.call("entity.get", {"id": entities[0]["id"]})
                assert world["components"]["Transform"]["position"][0] > 20, world
                assert client.call("net.stats")["corrections"] > 10
                if transport == "udp": assert api(api_port, "net.stats")["peers"][0]["route"] == "udp"
                finish = server.next(); assert finish["ok"] and finish["result"]["ticks"] == 210, finish
                assert finish["result"]["frame"] > 50, finish
                assert server.process.wait(timeout=10) == 0
                print(f"PASS dedicated {transport}: ready barrier, real client input, prefab replication, corrections, HTTP API, bounded shutdown")
            finally:
                for process in (client, bridge, server):
                    if process: process.close()
    settings["network"]["transport"] = "tcp"
    (project / "project.json").write_text(json.dumps(settings), encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="network-package-", dir=ROOT / "build") as output:
        subprocess.run([str(EXE), "package", str(project), "--out", output], capture_output=True, check=True, timeout=30)
        player = subprocess.run([str(Path(output) / "DedicatedSmoke.exe"), "--server", "--port", "0", "--frames", "6"], capture_output=True, timeout=15)
        result = json.loads(player.stdout); assert player.returncode == 0 and result["ok"], result
        assert result["result"]["dedicated"] and result["result"]["ticks"] == 6 and result["result"]["frame"] == 0
        print("PASS oe package + packaged --server: default game directory, headless idle ticks and clean exit")
