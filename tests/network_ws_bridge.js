// Integration-test adapter: native CLI TCP datagrams to a standard WebSocket client.
// This exercises the native server's RFC 6455 wire; it is not a browser/Wasm player test.
'use strict';
const net = require('node:net');
const ws = new WebSocket(process.argv[2]);
let peer, buffered = Buffer.alloc(0);
const server = net.createServer(socket => {
    if (peer) { socket.destroy(); return; }
    peer = socket;
    socket.on('data', bytes => {
        buffered = Buffer.concat([buffered, bytes]);
        if (buffered.length > 4 * 1024 * 1024) throw new Error('TCP buffer limit');
        while (buffered.length >= 4) {
            const size = buffered.readUInt32LE();
            if (size > 65536) throw new Error('Message limit');
            if (buffered.length < size + 4) break;
            ws.send(buffered.subarray(4, size + 4)); buffered = buffered.subarray(size + 4);
        }
    });
    socket.on('error', error => { console.error(error.message); process.exitCode = 1; });
});
ws.binaryType = 'arraybuffer';
ws.addEventListener('open', () => server.listen(0, '127.0.0.1', () => console.log(JSON.stringify({port: server.address().port}))));
ws.addEventListener('message', event => {
    if (!peer || !(event.data instanceof ArrayBuffer)) throw new Error('Unexpected WebSocket message');
    const bytes = Buffer.from(event.data), prefix = Buffer.alloc(4); prefix.writeUInt32LE(bytes.length);
    peer.write(Buffer.concat([prefix, bytes]));
});
ws.addEventListener('error', () => { console.error('WebSocket connection failed'); process.exit(1); });
ws.addEventListener('close', () => { if (peer) peer.end(); server.close(); });
