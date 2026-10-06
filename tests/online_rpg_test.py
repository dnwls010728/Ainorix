"""OnlineRPG with its real back end: oe_login, oe_channel, two game servers (oe serve-game, one per
channel) and client processes that only click Login.

Covers: login -> channel server -> entry ticket -> a world that is already running; pick up, chat,
drop; a player leaving and another arriving; tickets that are forged or used twice (kicked before
an avatar exists); a game server dying (popup, OK asks the game to quit) and the next login landing
on the other channel; the channel server dying (worlds keep running, nobody new gets in, the login
attempt says so) and coming back (the game server registers again by itself).
Python stdlib; needs build/bin/oe.exe, oe_login.exe and oe_channel.exe
(build.bat and games/OnlineRPG/server/build.bat).

    python tests/online_rpg_test.py
"""
import json
from pathlib import Path
import queue
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build/bin"
EXE = BIN / "oe.exe"
PROJECT = ROOT / "games/OnlineRPG"
VIEW = {"width": 1280, "height": 720}
LOGIN_BUTTON, POPUP_OK, BAG_SLOT_1 = (640, 370), (640, 407), (1006, 140)


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
        line = json.dumps({"command": command, "args": arguments or {}}, ensure_ascii=False) + "\n"
        self.process.stdin.write(line.encode("utf-8"))
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
    request = urllib.request.Request(f"http://127.0.0.1:{port}/api/call", body, {"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=5) as response:
        value = json.load(response)
    assert value["ok"], value
    return value["result"]


def free_port():
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        return reservation.getsockname()[1]


# ----- the back end's wire protocol, for a client that is not the game (ticket tests) -----

def frame(kind, payload=b""):
    return struct.pack("<IH", len(payload) + 2, kind) + payload


def s1(text):
    data = text if isinstance(text, bytes) else text.encode("utf-8")
    return struct.pack("<B", len(data)) + data


def read_frame(sock):
    def exact(count):
        data = b""
        while len(data) < count:
            chunk = sock.recv(count - len(data))
            assert chunk, "server closed the connection"
            data += chunk
        return data
    length, kind = struct.unpack("<IH", exact(6))
    return kind, exact(length - 2)


def take_s1(body, at):
    size = body[at]
    return body[at + 1:at + 1 + size], at + 1 + size


def entry_ticket(login_port, lobby_port, channel):
    """Logs in as a guest and asks for a channel, like the game does. Returns (name, entry ticket)."""
    hello = frame(1, struct.pack("<HB", 1, 1))
    with socket.create_connection(("127.0.0.1", login_port), timeout=10) as sock:
        sock.sendall(hello + frame(12))
        kind, body = read_frame(sock)
        assert kind == 13, (kind, body)
        name, at = take_s1(body, 4)
        ticket, at = take_s1(body, at)
        proof, at = take_s1(body, at)
    with socket.create_connection(("127.0.0.1", lobby_port), timeout=10) as sock:
        sock.sendall(hello + frame(20, s1(ticket) + s1(name) + s1(proof)) + frame(22, struct.pack("<B", channel)))
        while True:
            kind, body = read_frame(sock)
            if kind == 23:
                address, at = take_s1(body, 0)
                entry, at = take_s1(body, at + 2)
                return name.decode(), entry.decode()
            assert kind == 21, (kind, body)


work = Path(tempfile.mkdtemp(prefix="oe_rpg_test_"))
login_server = JsonProcess([BIN / "oe_login.exe", "--port", 0, "--db", work / "accounts.db", "--key-file", work / "server.key",
                            "--login-limit", 0, "--guest-limit", 0])
LOGIN = login_server.next()["port"]


def start_channel(port=0, game_port=0):
    process = JsonProcess([BIN / "oe_channel.exe", "--port", port, "--game-port", game_port, "--key-file", work / "server.key",
                           "--game-key-file", work / "game.key"])
    started = process.next()
    assert started["ok"], started
    return process, started["port"], started["gamePort"]


channel_server, LOBBY, PLATFORM = start_channel()


def serve(channel, port, api_port):
    server = JsonProcess([EXE, "serve-game", PROJECT, "--port", port, "--seed", 71, "--api-port", api_port,
                          "--set", f"channel={channel}", "--set", "map=town", "--set", f"platform=127.0.0.1:{PLATFORM}",
                          "--set-file", f"linkkey={work / 'game.key'}"])
    startup = server.next()
    assert startup["ok"] and startup["result"]["dedicated"], startup
    return server


game1_port, game2_port, game1_api, game2_api = free_port(), free_port(), free_port(), free_port()
servers = [login_server, channel_server, serve(1, game1_port, game1_api)]
time.sleep(1.5)  # channel 1 registers first: it gets the lower server id, which decides ties
servers.append(serve(2, game2_port, game2_api))
clients = []
try:
    def client(channel=None, raw=False):
        arguments = [EXE, "script", PROJECT]
        if not raw:
            arguments += ["--set", f"login=127.0.0.1:{LOGIN}", "--set", f"lobby=127.0.0.1:{LOBBY}"]
        if channel:
            arguments += ["--set", f"channel={channel}"]
        process = JsonProcess(arguments)
        clients.append(process)
        return process

    def step(frames):
        for _ in range(frames):
            for process in clients:
                process.call("sim.step", {"frames": 1})
            time.sleep(1 / 60)

    def click(process, point):
        process.call("input.click", dict(VIEW, x=point[0], y=point[1]))

    def key(process, name, down):
        process.call("input.key", {"key": name, "down": down})

    def tap(process, name, frames=2):
        key(process, name, True)
        step(frames)
        key(process, name, False)

    def running(process):
        return process.call("net.state").get("sync", {}).get("state") == "running"

    def data(process):
        return process.call("game.state")["data"]

    def login(process):
        click(process, LOGIN_BUTTON)
        for _ in range(900):
            step(1)
            if running(process):
                break
        assert running(process), (process.call("net.state"), data(process))
        step(40)

    def avatar(process):
        me = process.call("net.state")["localPlayer"]
        for entity in process.call("net.entities"):
            if entity["owner"] == me:
                return process.call("entity.get", {"id": entity["id"]})["components"]["Transform"]["position"]
        raise AssertionError("no avatar")

    def walk(process, x, z):
        for _ in range(400):
            px, _, pz = avatar(process)
            if abs(px - x) < 0.15 and abs(pz - z) < 0.15:
                break
            for name, down in (("A", px > x + 0.1), ("D", px < x - 0.1), ("W", pz > z + 0.1), ("S", pz < z - 0.1)):
                key(process, name, down)
            step(1)
        for name in "WASD":
            key(process, name, False)
        step(10)

    def avatars(api_port):
        return sum(1 for entity in api(api_port, "net.entities") if entity["owner"] != 0)

    # The worlds run before anybody is there.
    time.sleep(1.5)
    assert api(game1_api, "net.state")["sync"]["state"] == "running"
    assert api(game1_api, "game.state")["data"]["ground"] == 10

    # One click: login server -> channel server -> Channel 1, into the running world.
    a = client(channel=1)
    step(5)
    assert not running(a) and "popup" not in data(a)
    login(a)
    assert data(a)["chat"][-1] == "Guest0001 joined.", data(a)
    assert len(a.call("net.entities")) == 11  # own avatar and the ten starting items
    assert len(api(game1_api, "net.players")) == 2 and len(api(game2_api, "net.players")) == 1

    walk(a, 3, 4.4)  # a herb lies at (3, 4)
    tap(a, "E")
    step(40)
    assert data(a)["bagShown"] == [{"key": "herb", "count": 1}], data(a)
    assert api(game1_api, "game.state")["data"]["ground"] == 9

    # A second player logs in much later and finds the world as it is now.
    b = client(channel=1)
    step(5)
    login(b)
    assert len(b.call("net.entities")) == 11 and len(a.call("net.entities")) == 11  # two avatars, nine items
    assert data(a)["chat"][-1] == "Guest0002 joined." and data(b)["chat"][-1] == "Guest0002 joined."
    assert "bagShown" not in data(b)

    # Chat: typing does not walk, and both players get the line with the speaker's name.
    before = avatar(a)
    tap(a, "Enter")
    a.call("input.text", {"text": "안녕 wasd"})
    key(a, "A", True)
    step(30)
    key(a, "A", False)
    tap(a, "Enter")
    step(40)
    assert avatar(a) == before, (before, avatar(a))
    for process in (a, b):
        assert data(process)["chat"][-1] == "Guest0001: 안녕 wasd", data(process)

    # Dropping: a click on bag slot 1 puts the herb back on the ground for everybody.
    click(a, BAG_SLOT_1)
    step(40)
    assert data(a)["bagShown"] in ([], {}), data(a)
    assert api(game1_api, "game.state")["data"]["ground"] == 10
    assert len(b.call("net.entities")) == 12
    for process in (a, b):
        assert process.call("script.errors") == [], process.call("script.errors")
    assert api(game1_api, "script.errors") == []

    # A player leaving does not end the world.
    b.close()
    clients.remove(b)
    step(420)
    assert running(a) and data(a)["chat"][-1] == "Guest0002 left.", data(a)

    # Tickets. A join name that is not a ticket never gets an avatar and is kicked.
    def join(process, name):
        process.call("net.join", {"address": "127.0.0.1", "port": game1_port, "name": name})
        for _ in range(200):
            step(1)
            state = process.call("net.state")["state"]
            if state == "lobby":
                process.call("net.ready", {"ready": True})
                break
        step(120)
        return process.call("net.state")

    forger = client(raw=True)
    step(5)
    state = join(forger, "Guest0001")
    assert state["state"] != "lobby" and not running(forger), state
    assert avatars(game1_api) == 1
    # A real entry ticket (asked for outside the game) works once: the second use is kicked.
    name, ticket = entry_ticket(LOGIN, LOBBY, 1)
    c = client(raw=True)
    step(5)
    join(c, ticket)
    assert running(c) and avatars(game1_api) == 2, c.call("net.state")
    assert data(a)["chat"][-1] == f"{name} joined." and name == "Guest0003", data(a)
    thief = client(raw=True)
    step(5)
    state = join(thief, ticket)
    assert state["state"] != "lobby" and not running(thief), state
    assert avatars(game1_api) == 2
    # A ticket for channel 2 does not open channel 1.
    _, elsewhere = entry_ticket(LOGIN, LOBBY, 2)
    state = join(forger, elsewhere)
    assert state["state"] != "lobby" and not running(forger), state
    assert avatars(game1_api) == 2 and api(game1_api, "script.errors") == []
    for process in (forger, thief):
        process.close()
        clients.remove(process)

    # The game server of channel 1 dies: a modal popup says so, and its OK button asks the game to quit.
    servers[2].close()
    for _ in range(900):
        step(1)
        if "popup" in data(a):
            break
    assert data(a)["popup"] == "The connection to the server was lost.", data(a)
    assert not a.call("game.state")["quitRequested"]
    click(a, POPUP_OK)
    step(5)
    assert a.call("game.state")["quitRequested"]

    # Channel 1 is offline at the channel server: a login without a wish lands on channel 2.
    d = client()
    step(5)
    login(d)
    assert data(d)["chat"][-1].endswith(" joined."), data(d)
    assert len(api(game2_api, "net.players")) == 2
    # ... and asking for channel 1 is answered with a notice; OK returns to the title.
    e = client(channel=1)
    step(5)
    click(e, LOGIN_BUTTON)
    for _ in range(600):
        step(1)
        if "popup" in data(e):
            break
    assert data(e)["popup"] == "No channel is open right now.", data(e)
    click(e, POPUP_OK)
    step(5)
    assert not e.call("game.state")["quitRequested"] and not running(e)
    e.close()
    clients.remove(e)

    # The channel server dies: the world keeps running, but nobody new gets in.
    channel_server.close()
    step(240)
    assert running(d) and "popup" not in data(d)
    f = client()
    step(5)
    click(f, LOGIN_BUTTON)
    for _ in range(900):
        step(1)
        if "popup" in data(f):
            break
    assert data(f)["popup"] == "Could not reach the server.", data(f)
    click(f, POPUP_OK)
    step(5)

    # It comes back on the same ports: the game server registers again by itself and logins work.
    channel_server, _, _ = start_channel(LOBBY, PLATFORM)
    servers[1] = channel_server
    for _ in range(40):
        step(30)
        try:
            entry_ticket(LOGIN, LOBBY, 2)
            break
        except AssertionError:
            pass
    else:
        raise AssertionError("the game server did not register again")
    login(f)
    assert running(d) and avatars(game2_api) == 2
    assert data(d)["chat"][-1] == data(f)["chat"][-1] and data(f)["chat"][-1].endswith(" joined."), (data(d), data(f))
    for process in (d, f):
        assert process.call("script.errors") == [], process.call("script.errors")
    assert api(game2_api, "script.errors") == []
    print("PASS OnlineRPG back end: login -> channel -> entry ticket -> world, pick up, chat, drop, leaving, "
          "forged / reused / misdirected tickets kicked, game server death (popup + quit, other channel), "
          "channel server death and return")
finally:
    for process in clients + servers:
        try:
            process.close()
        except Exception:
            pass
    shutil.rmtree(work, ignore_errors=True)
