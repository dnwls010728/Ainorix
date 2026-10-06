"""End-to-end test of oe_login as a real process.

A Python client speaks the wire protocol on its own (framing, Hello, requests) and checks the
ticket's signature with the standard library's HMAC, so the server's crypto is verified against
an independent implementation. Then the engine does the same through Lua `link` (oe script).

    python games/OnlineRPG/server/test/login_test.py        (from the repository root)
"""
import base64, hashlib, hmac, json, os, shutil, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..', '..'))
BIN = os.path.join(ROOT, 'build', 'bin')
EXE = '.exe' if os.name == 'nt' else ''
HELLO, ERROR, LOGIN, REGISTER, GUEST, RESULT = 1, 2, 10, 11, 12, 13
failed = 0


def check(condition, what):
    global failed
    if not condition:
        failed += 1
        print('FAILED', what)


def frame(kind, payload=b''):
    return struct.pack('<IH', len(payload) + 2, kind) + payload


def s1(text):
    data = text.encode('utf-8') if isinstance(text, str) else text
    return struct.pack('<B', len(data)) + data


def read(sock, count):
    data = b''
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def ask(port, kind, payload=b'', version=1):
    """One request on a fresh connection -> (type, payload), or (0, b'') when closed without an answer."""
    with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
        sock.sendall(frame(HELLO, struct.pack('<HB', version, 1)) + frame(kind, payload))
        header = read(sock, 6)
        if header is None:
            return 0, b''
        length, got = struct.unpack('<IH', header)
        body = read(sock, length - 2) if length > 2 else b''
        check(read(sock, 1) is None, 'the server closes after its answer')
        return got, body


def result(body):
    account, = struct.unpack_from('<I', body, 0)
    n = body[4]
    name = body[5:5 + n].decode('utf-8')
    t = body[5 + n]
    ticket = body[6 + n:6 + n + t].decode('ascii')
    proof = body[7 + n + t:].decode('ascii')
    check(body[6 + n + t] == len(proof) == 20, 'a 20 character name proof ends the result')
    result.proof = proof
    return account, name, ticket


def error(body):
    return struct.unpack('<H', body)[0] if len(body) == 2 else -1


def open_ticket(text, key):
    raw = base64.urlsafe_b64decode(text)  # 40 characters: no padding needed
    kind, account, server, expires, nonce = struct.unpack('<BIHII', raw[:15])
    good = hmac.compare_digest(hmac.new(key, raw[:15], hashlib.sha256).digest()[:15], raw[15:])
    return good, kind, account, server, expires, nonce


def start(work, *extra):
    process = subprocess.Popen([os.path.join(BIN, 'oe_login' + EXE), '--port', '0', '--db', os.path.join(work, 'accounts.db'),
                                '--key-file', os.path.join(work, 'server.key')] + list(extra),
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    line = json.loads(process.stdout.readline())
    return process, line['port']


def main():
    work = tempfile.mkdtemp(prefix='oe_login_test_')
    process, port = start(work, '--login-limit', '0', '--guest-limit', '0')
    try:
        key = bytes.fromhex(open(os.path.join(work, 'server.key')).read().strip())
        check(len(key) == 32, 'the server wrote a 32-byte key file')

        # Register with the real password cost (100000 PBKDF2 rounds), then log in.
        began = time.perf_counter()
        kind, body = ask(port, REGISTER, s1('Hero') + s1('correct horse battery'))
        register_ms = (time.perf_counter() - began) * 1000
        check(kind == RESULT, 'register answers LoginResult')
        account, name, ticket = result(body)
        good, tkind, taccount, tserver, expires, nonce = open_ticket(ticket, key)
        check(name == 'Hero' and len(ticket) == 40, 'name and a 40 character ticket')
        check(good and tkind == 1 and taccount == account and tserver == 0, 'session ticket signed with the shared key (checked with Python hmac)')
        check(abs(expires - (time.time() + 12 * 3600)) < 30, 'ticket valid for 12 hours')
        expected = base64.urlsafe_b64encode(hmac.new(key, b'\x03' + struct.pack('<I', account) + b'Hero', hashlib.sha256).digest()[:15]).decode()
        check(result.proof == expected, 'name proof is HMAC(key, 3, account, name) (checked with Python hmac)')

        began = time.perf_counter()
        kind, body = ask(port, LOGIN, s1('hero') + s1('correct horse battery'))
        login_ms = (time.perf_counter() - began) * 1000
        check(kind == RESULT and result(body)[0] == account and result(body)[1] == 'Hero', 'login (name is case-insensitive) returns the same account')
        check(open_ticket(result(body)[2], key)[5] != nonce, 'each ticket has its own nonce')

        began = time.perf_counter()
        kind, body = ask(port, LOGIN, s1('Hero') + s1('wrong password'))
        wrong_ms = (time.perf_counter() - began) * 1000
        check(kind == ERROR and error(body) == 5, 'wrong password -> credentials error')
        began = time.perf_counter()
        kind, body = ask(port, LOGIN, s1('Nobody') + s1('wrong password'))
        unknown_ms = (time.perf_counter() - began) * 1000
        check(kind == ERROR and error(body) == 5, 'unknown name -> the same error')
        check(unknown_ms > wrong_ms * 0.5, 'an unknown name costs a hash too (%.0f ms vs %.0f ms)' % (unknown_ms, wrong_ms))

        # The stored password is a salted PBKDF2-HMAC-SHA256 hash: recompute it with hashlib.
        import sqlite3
        row = sqlite3.connect(os.path.join(work, 'accounts.db')).execute('SELECT salt, hash, iterations FROM accounts WHERE name = ?', ('Hero',)).fetchone()
        check(row and row[2] == 100000 and len(row[0]) == 16 and
              hashlib.pbkdf2_hmac('sha256', b'correct horse battery', row[0], row[2]) == row[1], 'stored hash equals hashlib.pbkdf2_hmac')

        kind, body = ask(port, REGISTER, s1('HERO') + s1('another password'))
        check(kind == ERROR and error(body) == 6, 'name taken')
        kind, body = ask(port, REGISTER, s1('홍길동') + s1('password1'))
        check(kind == RESULT and result(body)[1] == '홍길동', 'Korean name')
        kind, body = ask(port, GUEST)
        check(kind == RESULT and result(body)[1].startswith('Guest') and open_ticket(result(body)[2], key)[0], 'guest login')
        guest_began = time.perf_counter()
        for _ in range(50):
            ask(port, GUEST)
        guest_ms = (time.perf_counter() - guest_began) * 1000 / 50

        # Protocol errors and malformed input never take the server down.
        check(ask(port, GUEST, version=2)[1] == struct.pack('<H', 1), 'version mismatch')
        check(error(ask(port, 999)[1]) == 2, 'unknown message')
        check(error(ask(port, LOGIN, b'\x05ab')[1]) == 2, 'truncated payload')
        for junk in (b'\xff\xff\xff\xff\x00\x00', b'\x00\x00\x00\x00', b'GET / HTTP/1.1\r\n\r\n', os.urandom(4096)):
            with socket.create_connection(('127.0.0.1', port), timeout=10) as sock:
                sock.sendall(junk)
                try:
                    while sock.recv(4096):
                        pass
                except ConnectionError:
                    pass
        check(ask(port, GUEST)[0] == RESULT, 'still serving after malformed input')

        # The engine as the client: Lua link + string.pack, exactly what the game will run.
        lua = ("local id = LINK if not id then return 'no link' end local p = link.poll(id) "
               "for _, m in ipairs(p.messages) do if m.type == 13 then local account, name, ticket = string.unpack('<I4s1s1s1', m.data) "
               "RESULT = {account, name, #ticket} end end return RESULT or p.state")
        oe = subprocess.Popen([os.path.join(BIN, 'oe' + EXE), 'script', os.path.join(ROOT, 'games', 'OnlineRPG'), '--set', 'login=%d' % port],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, encoding='utf-8')

        def call(command, args, wait=0.3):
            oe.stdin.write(json.dumps({'command': command, 'args': args}) + '\n')
            oe.stdin.flush()
            time.sleep(wait)

        call('sim.step', {'frames': 1})
        call('script.eval', {'code': "LINK = link.open{port = tonumber(game.arg('login'))} "
                                     "link.send(LINK, 1, string.pack('<I2B', 1, 1)) "
                                     "link.send(LINK, 10, string.pack('<s1s1', 'Hero', 'correct horse battery')) return LINK"})
        for _ in range(4):
            call('sim.step', {'frames': 5})
        call('script.eval', {'code': lua})
        call('sim.step', {'frames': 5})
        call('script.eval', {'code': "return link.poll(LINK).state"})
        oe.stdin.close()
        lines = [json.loads(line) for line in oe.stdout.read().splitlines() if line.startswith('{')]
        oe.wait()
        values = [line['result'].get('value') for line in lines if line.get('ok') and isinstance(line.get('result'), dict) and 'value' in line['result']]
        check(len(values) == 3 and values[1] == [account, 'Hero', 40], 'the engine logs in through Lua link: %r' % (values,))
        check(len(values) == 3 and values[2] == 'closed', 'and sees the server close the link afterwards')
    finally:
        process.terminate()
        process.wait()

    # Rate limit per address, and accounts that survive a restart.
    process, port = start(work, '--login-limit', '2', '--iterations', '1000')
    try:
        check(ask(port, LOGIN, s1('Hero') + s1('correct horse battery'))[0] == RESULT, 'accounts survive a restart (hash keeps its own iteration count)')
        check(error(ask(port, LOGIN, s1('Hero') + s1('wrong password'))[1]) == 5, 'second attempt')
        check(error(ask(port, LOGIN, s1('Hero') + s1('correct horse battery'))[1]) == 4, 'third attempt within a minute is rate limited')
    finally:
        process.terminate()
        process.wait()
    shutil.rmtree(work, ignore_errors=True)

    if failed:
        print('FAIL oe_login: %d checks failed' % failed)
        return 1
    print('PASS oe_login: register %.0f ms, login %.0f ms, wrong password %.0f ms, unknown name %.0f ms, guest %.1f ms (one client, sequential)'
          % (register_ms, login_ms, wrong_ms, unknown_ms, guest_ms))
    return 0


if __name__ == '__main__':
    sys.exit(main())
