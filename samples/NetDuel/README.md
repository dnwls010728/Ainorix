# NetDuel

A small rollback crystal-collection game using built-in meshes, with keyboard,
gamepad and on-screen touch controls. The first player to collect five crystals wins.
Press Space near the gold crystal to collect it; R starts a fresh round.

Run `build/bin/oe.exe editor samples/NetDuel`. Choose Players 2 (up to 4),
then Play. Switch Game peers to control each player; use the Network panel
for seeded latency/loss. Players 1 provides offline practice.

For two native players, run `build/bin/oe.exe run samples/NetDuel` twice.
Click Host on one, Join on the other, Ready on both, then Start on the host.
Change the Join button's Script.params.address/port before play for a remote host.
Default binding is loopback; set project.json network.bind to an explicit LAN
address to accept remote players. Stop/reopen before changing project settings.
See [the complete setup and platform verification guide](../../docs/NETWORK_SAMPLES.md).
