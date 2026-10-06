#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rpg {

// One message on every link of the back end (and of the engine's `link`):
//
//   u32 length (of what follows, 2..65536)   u16 type   payload
//
// little endian.
constexpr size_t kMaxFrameBody = 65536;
constexpr size_t kMaxPayload = kMaxFrameBody - 2;

// Incremental frame parser over one contiguous buffer: bytes are received straight into its free
// space and frames are handed out as views into it, so a frame costs no allocation and no copy
// beyond the kernel's. Tested without sockets (test/: split, coalesced and malformed input).
class FrameBuffer {
public:
    // Free space to receive into (at least `want` bytes unless the buffer is at its limit).
    uint8_t* Space(size_t want, size_t& available);
    void Commit(size_t received);
    // 1: a frame (valid until the next Space call); 0: need more bytes; -1: malformed, close.
    int Next(uint16_t& type, const uint8_t*& payload, size_t& size);
    size_t Buffered() const { return tail_ - head_; }
    void Reset() { head_ = tail_ = 0; }

private:
    std::vector<uint8_t> bytes_;
    size_t head_ = 0, tail_ = 0;
};

using SocketHandle = uintptr_t;

// Readiness notification. The poll() implementation is enough for the connection counts of this
// back end; an epoll or IOCP one replaces it behind this interface if a measurement says so.
class Poller {
public:
    struct Event {
        uint64_t key = 0;
        bool readable = false, writable = false, failed = false;
    };
    virtual ~Poller() = default;
    virtual void Add(SocketHandle socket, uint64_t key, bool wantWrite) = 0;
    virtual void SetWrite(SocketHandle socket, bool wantWrite) = 0;
    virtual void Remove(SocketHandle socket) = 0;
    // Appends the sockets that are ready; returns after `timeoutMs` at the latest.
    virtual void Wait(int timeoutMs, std::vector<Event>& events) = 0;
};
std::unique_ptr<Poller> CreatePollPoller();

struct ServerConfig {
    std::string bind = "127.0.0.1";
    uint16_t port = 0;                  // 0 = any free port (tests)
    size_t maxConnections = 4096;
    size_t maxQueuedBytes = 256 * 1024; // unsent bytes per connection; more closes it
    double idleSeconds = 10.0;          // default time without a received frame before a connection is closed
};

struct ServerStats {
    uint64_t accepted = 0, refused = 0, closed = 0, framesIn = 0, framesOut = 0, bytesIn = 0, bytesOut = 0;
    uint64_t badFrames = 0, overflows = 0, idleCloses = 0;
    size_t connections = 0;
};

// A single-threaded framed TCP server: accepts, reads, parses, writes; never blocks. Everything
// except Post is called on the thread that runs it.
class FrameServer {
public:
    using ConnId = uint64_t;  // never reused: slot index + generation
    class Handler {
    public:
        virtual ~Handler() = default;
        virtual void OnOpen(ConnId, const std::string& /*address*/) {}
        // `payload` is a view into the receive buffer: copy what must outlive the call.
        virtual void OnFrame(ConnId id, uint16_t type, const uint8_t* payload, size_t size) = 0;
        virtual void OnClose(ConnId, const char* /*reason*/) {}
        // About every 250 ms, with a monotonic clock in seconds.
        virtual void OnTick(double /*now*/) {}
    };

    FrameServer();
    ~FrameServer();
    FrameServer(const FrameServer&) = delete;
    FrameServer& operator=(const FrameServer&) = delete;

    bool Listen(const ServerConfig& config, std::string* error);
    uint16_t Port() const { return ListenerPort(0); }
    // Another listening socket on the same loop (a second port for another kind of peer), after
    // Listen. Returns its index (1, 2, ...), or -1 with `error`.
    int AddListener(const std::string& bind, uint16_t port, std::string* error);
    uint16_t ListenerPort(int index) const;
    // Which listener accepted the connection (0 = the one of Listen); -1 for an unknown id.
    int Listener(ConnId id) const;

    // Queues one frame. False for an unknown connection or an oversized payload; a connection
    // whose unsent bytes exceed the limit is closed.
    bool Send(ConnId id, uint16_t type, const void* payload, size_t size);
    // Closes at the end of this round, or (`afterFlush`, for a last answer) once everything queued
    // was written and the other side closed or a second passed. `reason` must be a string literal
    // (it is kept until OnClose).
    void Close(ConnId id, const char* reason, bool afterFlush = false);
    void SetIdleSeconds(ConnId id, double seconds);
    bool Alive(ConnId id) const;
    std::string Address(ConnId id) const;

    // Thread-safe: runs `task` on the server thread, soon. How worker threads hand results back.
    void Post(std::function<void()> task);

    // One round: wait at most `timeoutMs` for I/O, serve it, run posted tasks and the tick.
    void RunOnce(Handler& handler, int timeoutMs);
    // Rounds until `stop` is set.
    void Run(Handler& handler, const std::atomic<bool>& stop);

    const ServerStats& Stats() const { return stats_; }
    static double Now();  // monotonic seconds

private:
    struct Connection {
        SocketHandle socket = 0;
        uint32_t generation = 0;
        bool open = false, closing = false, wantWrite = false, lingering = false;
        int listener = 0;
        const char* closeReason = "closed";
        std::string address;
        FrameBuffer in;
        std::vector<uint8_t> out;
        size_t outHead = 0;
        double lastFrame = 0, idleSeconds = 0;
    };
    Connection* Find(ConnId id);
    const Connection* Find(ConnId id) const;
    void Accept(Handler& handler, int listener);
    bool OpenListener(const std::string& bind, uint16_t port, std::string* error);
    void Read(Handler& handler, uint32_t index);
    bool Flush(Connection& connection);  // false: the socket failed
    // The last answer is out: stop sending and wait (briefly) for the other side to close. Closing
    // at once could reset the connection while unread input is pending and lose the answer.
    void Linger(Connection& connection);
    void Discard(Handler& handler, uint32_t index);
    void Drop(Handler* handler, uint32_t index, const char* reason);
    void WantWrite(Connection& connection, bool want);

    ServerConfig config_;
    std::unique_ptr<Poller> poller_;
    struct ListenSocket {
        SocketHandle socket = 0;
        uint16_t port = 0;
    };
    std::vector<ListenSocket> listeners_;
    SocketHandle wake_ = 0;
    bool listening_ = false;
    std::vector<Connection> connections_;
    std::vector<uint32_t> free_;
    struct Doomed {
        ConnId id;
        const char* reason;
    };
    std::vector<Doomed> doomed_;  // closed from inside a callback: dropped at the end of the round
    std::vector<Poller::Event> events_;
    std::mutex postMutex_;
    std::vector<std::function<void()>> posted_, running_;
    ServerStats stats_;
    double lastTick_ = 0;
};

}  // namespace rpg
