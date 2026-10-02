// Exercise the shipped EM_JS bridge without a browser/SDK. Does not replace a real Wasm/browser test.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../engine/platform/web/NetworkWeb.cpp'), 'utf8');
const sockets = [];
class FakeWebSocket {
    constructor(url) {
        if (url === 'invalid') throw new Error('invalid URL');
        this.readyState = 0;
        this.bufferedAmount = 0;
        this.sent = [];
        sockets.push(this);
    }
    send(bytes) { this.sent.push(bytes); }
    close(code) { this.readyState = 3; this.closeCode = code; }
}
const states = [], messages = [], allocations = [];
let acceptMessages = true;
const heap = new Uint8Array(128 * 1024);
const sandbox = {
    WebSocket: FakeWebSocket, ArrayBuffer, Uint8Array, Map,
    UTF8ToString: value => value,
    HEAPU8: heap,
    _malloc: size => { allocations.push(size); return 64; },
    _free: pointer => assert.equal(pointer, 64),
    Module: {
        _oe_NetworkWebState: (owner, state) => states.push([owner, state]),
        _oe_NetworkWebData: (owner, pointer, size) => {
            messages.push([owner, Array.from(heap.slice(pointer, pointer + size))]);
            return acceptMessages ? 1 : 0;
        }
    }
};
vm.createContext(sandbox);
let functions = 0;
for (const match of source.matchAll(/EM_JS\(\w+, (\w+), \(([^)]*)\), \{([\s\S]*?)\n\}\);/g)) {
    const parameters = match[2].split(',').map(value => value.trim().match(/(\w+)$/)[1]).join(',');
    vm.runInContext(`function ${match[1]}(${parameters}) { ${match[3]} }`, sandbox);
    ++functions;
}
assert.equal(functions, 3);
const id = sandbox.OpenWebSocket('ws://localhost:1234', 7);
assert.ok(id > 0);
const socket = sockets.at(-1);
assert.equal(socket.binaryType, 'arraybuffer');
assert.equal(sandbox.SendWebSocket(id, 10, 3), 1);
socket.readyState = 1;
socket.onopen();
assert.deepEqual(states.at(-1), [7, 1]);
heap.set([1, 2, 3], 10);
assert.equal(sandbox.SendWebSocket(id, 10, 3), 0);
heap[10] = 9;
assert.deepEqual(Array.from(socket.sent[0]), [1, 2, 3]);
socket.bufferedAmount = 4 * 1024 * 1024;
assert.equal(sandbox.SendWebSocket(id, 10, 3), 1);
socket.onmessage({data: new Uint8Array([4, 5, 6]).buffer});
assert.deepEqual(messages.at(-1), [7, [4, 5, 6]]);
socket.onmessage({data: new ArrayBuffer(0)});
assert.equal(allocations.at(-1), 1);
const before = allocations.length;
socket.onmessage({data: new ArrayBuffer(65537)});
assert.equal(allocations.length, before);
assert.deepEqual(states.at(-1), [7, 3]);
assert.equal(socket.closeCode, 1009);
assert.equal(sandbox.SendWebSocket(id, 10, 3), 2);
sandbox.CloseWebSocket(id);
assert.equal(sandbox.Module.oeNetSockets.sockets.size, 0);
for (const name of ['onopen', 'onmessage', 'onclose', 'onerror']) assert.equal(socket[name], null);
assert.equal(sandbox.SendWebSocket(id, 10, 3), 2);
assert.equal(sandbox.OpenWebSocket('invalid', 8), 0);
const textId = sandbox.OpenWebSocket('ws://localhost:1234', 8);
const textSocket = sockets.at(-1);
textSocket.onmessage({data: 'text'});
assert.equal(allocations.length, before);
assert.equal(textSocket.closeCode, 1009);
sandbox.CloseWebSocket(textId);
const fullId = sandbox.OpenWebSocket('ws://localhost:1234', 9);
acceptMessages = false;
sockets.at(-1).onmessage({data: new ArrayBuffer(1)});
assert.equal(sockets.at(-1).closeCode, 1009);
sandbox.CloseWebSocket(fullId);
sandbox.Module.oeNetSockets.next = 2147483648;
assert.equal(sandbox.OpenWebSocket('ws://localhost:1234', 10), 0);
assert.equal(sandbox.Module.oeNetSockets.sockets.size, 0);
sandbox.WebSocket = undefined;
assert.equal(sandbox.OpenWebSocket('ws://localhost:1234', 10), 0);
console.log('PASS WebSocket JS bounds, buffering and callback cleanup');
