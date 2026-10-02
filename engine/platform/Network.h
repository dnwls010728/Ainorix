#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oe {

// Native sockets use numeric IPv4 only: DNS never blocks the simulation thread.
struct NetAddress {
    std::string host = "127.0.0.1";
    uint16_t port = 0;  // bind port zero requests an ephemeral port
    bool operator==(const NetAddress& other) const { return host == other.host && port == other.port; }
};
// Canonical dotted decimal (no leading zeroes). Non-loopback bind must be explicitly supplied.
bool ValidNetAddress(const NetAddress& address, bool allowZeroPort = false);
enum class SocketKind { Udp, Tcp };
enum class SocketState { Connecting, Open, Closed };
enum class SocketIo { Progress, WouldBlock, Closed, TooLarge, Error };

// RAII non-blocking socket. Every operation returns immediately; errors are retained in LastError.
class NetSocket {
public:
    virtual ~NetSocket();
    virtual bool Bind(const NetAddress& address, std::string* error) = 0;
    virtual bool Listen(std::string* error) = 0;
    virtual bool Connect(const NetAddress& address, std::string* error) = 0;
    // Null with empty error means no pending connection. Accepted streams also use NODELAY.
    virtual std::unique_ptr<NetSocket> Accept(NetAddress& address, std::string* error) = 0;
    virtual NetAddress LocalAddress() const = 0;
    // Connecting state is resolved with zero-timeout readiness polling, not a blocking wait.
    virtual SocketState State() = 0;
    virtual const std::string& LastError() const = 0;
    // Progress may consume only part of a TCP buffer; UDP progress is an entire datagram.
    virtual SocketIo Send(const uint8_t* bytes, size_t size, size_t& sent) = 0;
    virtual SocketIo Receive(uint8_t* bytes, size_t capacity, size_t& received) = 0;
    virtual SocketIo SendTo(const NetAddress& address, const uint8_t* bytes, size_t size) = 0;
    // Oversized datagrams are consumed and reported as TooLarge, never delivered truncated.
    virtual SocketIo ReceiveFrom(NetAddress& address, uint8_t* bytes, size_t capacity, size_t& received) = 0;
protected:
    NetSocket();
};

// Explicit factory only; unsupported browser UDP/TCP returns null with a hint.
std::unique_ptr<NetSocket> CreateNetSocket(SocketKind kind, std::string* error);

// Browser callbacks only buffer bounded binary messages; game code drains them at a frame boundary.
class PlatformWebSocket {
public:
    virtual ~PlatformWebSocket();
    virtual SocketState State() const = 0;
    virtual const std::string& LastError() const = 0;
    virtual SocketIo Send(const uint8_t* bytes, size_t size) = 0;
    virtual void Receive(std::vector<std::vector<uint8_t>>& messages) = 0;
protected:
    PlatformWebSocket();
};
// ws:// or wss:// client; native builds return null. No worker thread is created.
std::unique_ptr<PlatformWebSocket> CreatePlatformWebSocket(const std::string& url, std::string* error);
// Monotonic creation/live-object counters for the single-player zero-cost pin and resource tests.
uint64_t PlatformNetSocketsCreated();
uint64_t PlatformNetSocketsLive();

}  // namespace oe
