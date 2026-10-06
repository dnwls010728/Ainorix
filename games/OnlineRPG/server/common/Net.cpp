#include "common/Net.h"

#include <chrono>
#include <cstring>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rpg {
namespace {

#ifdef _WIN32
using Native = SOCKET;
const Native kInvalid = INVALID_SOCKET;
const short kPollRead = POLLRDNORM, kPollWrite = POLLWRNORM;
void CloseNative(Native s) { closesocket(s); }
bool WouldBlock() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK; }
bool NonBlocking(Native s) { u_long on = 1; return ioctlsocket(s, FIONBIO, &on) == 0; }
int PollNative(pollfd* fds, size_t count, int timeoutMs) { return WSAPoll(fds, static_cast<ULONG>(count), timeoutMs); }
bool Startup() {
    static const bool ok = [] { WSADATA data; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }();
    return ok;
}
using IoSize = int;
#else
using Native = int;
const Native kInvalid = -1;
const short kPollRead = POLLIN, kPollWrite = POLLOUT;
void CloseNative(Native s) { close(s); }
bool WouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR; }
bool NonBlocking(Native s) { const int flags = fcntl(s, F_GETFL, 0); return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0; }
int PollNative(pollfd* fds, size_t count, int timeoutMs) { return poll(fds, static_cast<nfds_t>(count), timeoutMs); }
bool Startup() { return true; }
using IoSize = ssize_t;
#endif

constexpr uint64_t kWakeKey = ~0ull, kListenerKeys = 16;  // listener i has the key kWakeKey - 1 - i
constexpr size_t kMaxBuffer = kMaxFrameBody + 4;

class PollPoller final : public Poller {
public:
    void Add(SocketHandle socket, uint64_t key, bool wantWrite) override {
        index_[socket] = fds_.size();
        pollfd fd{};
        fd.fd = static_cast<Native>(socket);
        fd.events = static_cast<short>(kPollRead | (wantWrite ? kPollWrite : 0));
        fds_.push_back(fd);
        keys_.push_back(key);
    }
    void SetWrite(SocketHandle socket, bool wantWrite) override {
        auto it = index_.find(socket);
        if (it != index_.end()) fds_[it->second].events = static_cast<short>(kPollRead | (wantWrite ? kPollWrite : 0));
    }
    void Remove(SocketHandle socket) override {
        auto it = index_.find(socket);
        if (it == index_.end()) return;
        const size_t at = it->second, last = fds_.size() - 1;
        index_.erase(it);
        if (at != last) {
            fds_[at] = fds_[last];
            keys_[at] = keys_[last];
            index_[static_cast<SocketHandle>(fds_[at].fd)] = at;
        }
        fds_.pop_back();
        keys_.pop_back();
    }
    void Wait(int timeoutMs, std::vector<Event>& events) override {
        if (fds_.empty()) return;
        const int ready = PollNative(fds_.data(), fds_.size(), timeoutMs);
        if (ready <= 0) return;
        for (size_t i = 0; i < fds_.size(); ++i) {
            const short got = fds_[i].revents;
            if (!got) continue;
            Event event;
            event.key = keys_[i];
            event.readable = (got & kPollRead) != 0;
            event.writable = (got & kPollWrite) != 0;
            event.failed = (got & (POLLERR | POLLHUP | POLLNVAL)) != 0;
            events.push_back(event);
        }
    }

private:
    std::vector<pollfd> fds_;
    std::vector<uint64_t> keys_;
    std::unordered_map<SocketHandle, size_t> index_;
};

}  // namespace

std::unique_ptr<Poller> CreatePollPoller() { return std::make_unique<PollPoller>(); }

// ----- FrameBuffer ------------------------------------------------------------

uint8_t* FrameBuffer::Space(size_t want, size_t& available) {
    if (head_ == tail_) head_ = tail_ = 0;
    else if (head_ > 0) {
        std::memmove(bytes_.data(), bytes_.data() + head_, tail_ - head_);
        tail_ -= head_;
        head_ = 0;
    }
    if (bytes_.size() - tail_ < want && bytes_.size() < kMaxBuffer) {
        size_t size = bytes_.size() < 2048 ? 4096 : bytes_.size() * 2;
        if (size < tail_ + want) size = tail_ + want;
        if (size > kMaxBuffer) size = kMaxBuffer;
        bytes_.resize(size);
    }
    available = bytes_.size() - tail_;
    return bytes_.data() + tail_;
}

void FrameBuffer::Commit(size_t received) { tail_ += received; }

int FrameBuffer::Next(uint16_t& type, const uint8_t*& payload, size_t& size) {
    if (tail_ - head_ < 4) return 0;
    const uint8_t* at = bytes_.data() + head_;
    const size_t length = static_cast<size_t>(at[0]) | (static_cast<size_t>(at[1]) << 8) | (static_cast<size_t>(at[2]) << 16) | (static_cast<size_t>(at[3]) << 24);
    if (length < 2 || length > kMaxFrameBody) return -1;
    if (tail_ - head_ < 4 + length) return 0;
    type = static_cast<uint16_t>(at[4] | (at[5] << 8));
    payload = at + 6;
    size = length - 2;
    head_ += 4 + length;
    return 1;
}

// ----- FrameServer ------------------------------------------------------------

FrameServer::FrameServer() : poller_(CreatePollPoller()) {}

FrameServer::~FrameServer() {
    for (Connection& connection : connections_)
        if (connection.open) CloseNative(static_cast<Native>(connection.socket));
    for (const ListenSocket& listener : listeners_) CloseNative(static_cast<Native>(listener.socket));
    if (listening_) CloseNative(static_cast<Native>(wake_));
}

double FrameServer::Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool FrameServer::OpenListener(const std::string& bindAddress, uint16_t port, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!Startup()) return fail("cannot start the socket library");
    if (listeners_.size() >= kListenerKeys) return fail("too many listeners");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) return fail("bind address must be a numeric IPv4 address");

    Native listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalid) return fail("cannot create a socket");
    int on = 1;
#ifdef _WIN32
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
#else
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, SOMAXCONN) != 0 || !NonBlocking(listener)) {
        CloseNative(listener);
        return fail("cannot listen on that address and port (in use?)");
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length);
    ListenSocket entry;
    entry.socket = static_cast<SocketHandle>(listener);
    entry.port = ntohs(bound.sin_port);
    poller_->Add(entry.socket, kWakeKey - 1 - listeners_.size(), false);
    listeners_.push_back(entry);
    return true;
}

bool FrameServer::Listen(const ServerConfig& config, std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (listening_) return fail("already listening");
    if (!OpenListener(config.bind, config.port, error)) return false;

    // Wake-up for Post: a UDP socket connected to itself; one datagram makes Wait return.
    Native wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in loop{};
    loop.sin_family = AF_INET;
    loop.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t loopLength = sizeof(loop);
    if (wake == kInvalid || bind(wake, reinterpret_cast<sockaddr*>(&loop), sizeof(loop)) != 0 ||
        getsockname(wake, reinterpret_cast<sockaddr*>(&loop), &loopLength) != 0 ||
        connect(wake, reinterpret_cast<sockaddr*>(&loop), sizeof(loop)) != 0 || !NonBlocking(wake)) {
        poller_->Remove(listeners_.back().socket);
        CloseNative(static_cast<Native>(listeners_.back().socket));
        listeners_.pop_back();
        if (wake != kInvalid) CloseNative(wake);
        return fail("cannot create the wake-up socket");
    }

    config_ = config;
    wake_ = static_cast<SocketHandle>(wake);
    listening_ = true;
    connections_.reserve(config.maxConnections);
    poller_->Add(wake_, kWakeKey, false);
    lastTick_ = Now();
    return true;
}

int FrameServer::AddListener(const std::string& bind, uint16_t port, std::string* error) {
    if (!listening_) { if (error) *error = "call Listen first"; return -1; }
    return OpenListener(bind, port, error) ? static_cast<int>(listeners_.size()) - 1 : -1;
}
uint16_t FrameServer::ListenerPort(int index) const {
    return index >= 0 && static_cast<size_t>(index) < listeners_.size() ? listeners_[static_cast<size_t>(index)].port : 0;
}
int FrameServer::Listener(ConnId id) const {
    const Connection* connection = Find(id);
    return connection ? connection->listener : -1;
}

FrameServer::Connection* FrameServer::Find(ConnId id) {
    const uint32_t index = static_cast<uint32_t>(id & 0xffffffffu);
    if (index >= connections_.size()) return nullptr;
    Connection& connection = connections_[index];
    return connection.open && connection.generation == static_cast<uint32_t>(id >> 32) ? &connection : nullptr;
}
const FrameServer::Connection* FrameServer::Find(ConnId id) const { return const_cast<FrameServer*>(this)->Find(id); }

bool FrameServer::Alive(ConnId id) const { return Find(id) != nullptr; }
std::string FrameServer::Address(ConnId id) const {
    const Connection* connection = Find(id);
    return connection ? connection->address : std::string();
}
void FrameServer::SetIdleSeconds(ConnId id, double seconds) {
    if (Connection* connection = Find(id)) connection->idleSeconds = seconds;
}

void FrameServer::WantWrite(Connection& connection, bool want) {
    if (connection.wantWrite == want) return;
    connection.wantWrite = want;
    poller_->SetWrite(connection.socket, want);
}

bool FrameServer::Flush(Connection& connection) {
    while (connection.outHead < connection.out.size()) {
        const IoSize sent = send(static_cast<Native>(connection.socket), reinterpret_cast<const char*>(connection.out.data() + connection.outHead),
                                 static_cast<int>(connection.out.size() - connection.outHead), 0);
        if (sent > 0) {
            connection.outHead += static_cast<size_t>(sent);
            stats_.bytesOut += static_cast<uint64_t>(sent);
        } else if (WouldBlock()) {
            // Keep what is left at the front, so a slow reader does not grow the buffer for ever.
            if (connection.outHead > 65536) {
                connection.out.erase(connection.out.begin(), connection.out.begin() + static_cast<ptrdiff_t>(connection.outHead));
                connection.outHead = 0;
            }
            WantWrite(connection, true);
            return true;
        } else {
            return false;
        }
    }
    connection.out.clear();
    connection.outHead = 0;
    WantWrite(connection, false);
    return true;
}

bool FrameServer::Send(ConnId id, uint16_t type, const void* payload, size_t size) {
    Connection* connection = Find(id);
    if (!connection || connection->closing || size > kMaxPayload) return false;
    if (connection->out.size() - connection->outHead + size + 6 > config_.maxQueuedBytes) {
        ++stats_.overflows;
        Close(id, "send queue overflow");
        return false;
    }
    const uint32_t length = static_cast<uint32_t>(size + 2);
    const uint8_t header[6] = {static_cast<uint8_t>(length), static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length >> 16),
                               static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(type), static_cast<uint8_t>(type >> 8)};
    connection->out.insert(connection->out.end(), header, header + 6);
    if (size) connection->out.insert(connection->out.end(), static_cast<const uint8_t*>(payload), static_cast<const uint8_t*>(payload) + size);
    ++stats_.framesOut;
    // Written at once when the socket takes it; otherwise the poller says when to continue.
    if (!connection->wantWrite && !Flush(*connection)) Close(id, "send failed");
    return true;
}

void FrameServer::Close(ConnId id, const char* reason, bool afterFlush) {
    Connection* connection = Find(id);
    if (!connection) return;
    if (connection->closing && afterFlush) return;
    const bool pending = connection->outHead < connection->out.size();
    connection->closing = true;  // no more frames are read or queued
    connection->closeReason = reason;
    if (!afterFlush) { doomed_.push_back({id, reason}); return; }
    connection->lastFrame = Now();  // the idle timeout bounds a reader that never takes the answer
    if (!pending) Linger(*connection);
}

void FrameServer::Linger(Connection& connection) {
    if (connection.lingering) return;
#ifdef _WIN32
    shutdown(static_cast<Native>(connection.socket), SD_SEND);
#else
    shutdown(static_cast<Native>(connection.socket), SHUT_WR);
#endif
    connection.lingering = true;
    connection.lastFrame = Now();
    connection.idleSeconds = 1.0;
}

void FrameServer::Discard(Handler& handler, uint32_t index) {
    Connection& connection = connections_[index];
    char bytes[4096];
    for (int round = 0; round < 16; ++round) {
        const IoSize received = recv(static_cast<Native>(connection.socket), bytes, sizeof(bytes), 0);
        if (received > 0) continue;
        if (received == 0 || !WouldBlock()) Drop(&handler, index, connection.closeReason);
        return;
    }
}

void FrameServer::Drop(Handler* handler, uint32_t index, const char* reason) {
    Connection& connection = connections_[index];
    if (!connection.open) return;
    const ConnId id = (static_cast<uint64_t>(connection.generation) << 32) | index;
    poller_->Remove(connection.socket);
    CloseNative(static_cast<Native>(connection.socket));
    connection.open = false;
    connection.out.clear();
    connection.outHead = 0;
    connection.in.Reset();
    free_.push_back(index);
    ++stats_.closed;
    --stats_.connections;
    if (handler) handler->OnClose(id, reason);
}

void FrameServer::Accept(Handler& handler, int listener) {
    for (int i = 0; i < 64; ++i) {
        sockaddr_in from{};
        socklen_t length = sizeof(from);
        Native socket = accept(static_cast<Native>(listeners_[static_cast<size_t>(listener)].socket), reinterpret_cast<sockaddr*>(&from), &length);
        if (socket == kInvalid) return;
        int on = 1;
        if (stats_.connections >= config_.maxConnections || !NonBlocking(socket) ||
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
            CloseNative(socket);
            ++stats_.refused;
            continue;
        }
        uint32_t index;
        if (free_.empty()) {
            index = static_cast<uint32_t>(connections_.size());
            connections_.emplace_back();
        } else {
            index = free_.back();
            free_.pop_back();
        }
        Connection& connection = connections_[index];
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        connection.socket = static_cast<SocketHandle>(socket);
        ++connection.generation;  // starts at 1: an id is never 0
        connection.open = true;
        connection.closing = connection.wantWrite = connection.lingering = false;
        connection.closeReason = "closed";
        connection.listener = listener;
        connection.address = text;
        connection.lastFrame = Now();
        connection.idleSeconds = config_.idleSeconds;
        const ConnId id = (static_cast<uint64_t>(connection.generation) << 32) | index;
        poller_->Add(connection.socket, id, false);
        ++stats_.accepted;
        ++stats_.connections;
        handler.OnOpen(id, connection.address);
    }
}

void FrameServer::Read(Handler& handler, uint32_t index) {
    // The reference stays valid: connections are only added by Accept, never inside a callback.
    Connection& connection = connections_[index];
    const ConnId id = (static_cast<uint64_t>(connection.generation) << 32) | index;
    for (int round = 0; round < 8 && connection.open && !connection.closing; ++round) {
        size_t available = 0;
        uint8_t* space = connection.in.Space(4096, available);
        if (!available) { ++stats_.badFrames; Drop(&handler, index, "malformed frame"); return; }
        const IoSize received = recv(static_cast<Native>(connection.socket), reinterpret_cast<char*>(space), static_cast<int>(available), 0);
        if (received == 0) { Drop(&handler, index, "closed by the other side"); return; }
        if (received < 0) {
            if (!WouldBlock()) Drop(&handler, index, "connection lost");
            return;
        }
        connection.in.Commit(static_cast<size_t>(received));
        stats_.bytesIn += static_cast<uint64_t>(received);
        for (;;) {
            uint16_t type = 0;
            const uint8_t* payload = nullptr;
            size_t size = 0;
            const int got = connection.in.Next(type, payload, size);
            if (got < 0) { ++stats_.badFrames; Drop(&handler, index, "malformed frame"); return; }
            if (got == 0) break;
            ++stats_.framesIn;
            connection.lastFrame = Now();
            handler.OnFrame(id, type, payload, size);
            if (!connection.open || connection.closing) return;  // what follows a closing request is not served
        }
        if (static_cast<size_t>(received) < available) return;  // the socket is drained
    }
}

void FrameServer::Post(std::function<void()> task) {
    bool first;
    {
        std::lock_guard<std::mutex> lock(postMutex_);
        first = posted_.empty();
        posted_.push_back(std::move(task));
    }
    if (first && listening_) {
        const char byte = 1;
        send(static_cast<Native>(wake_), &byte, 1, 0);
    }
}

void FrameServer::RunOnce(Handler& handler, int timeoutMs) {
    events_.clear();
    poller_->Wait(timeoutMs, events_);
    for (const Poller::Event& event : events_) {
        if (event.key < kWakeKey && event.key >= kWakeKey - kListenerKeys) { Accept(handler, static_cast<int>(kWakeKey - 1 - event.key)); continue; }
        if (event.key == kWakeKey) {
            char bytes[64];
            while (recv(static_cast<Native>(wake_), bytes, sizeof(bytes), 0) > 0) {}
            continue;
        }
        Connection* connection = Find(event.key);
        if (!connection) continue;  // dropped earlier in this round
        const uint32_t index = static_cast<uint32_t>(event.key & 0xffffffffu);
        if (event.writable) {
            if (!Flush(*connection)) { Drop(&handler, index, "send failed"); continue; }
            if (connection->closing && connection->outHead == connection->out.size()) Linger(*connection);
        }
        if (connection->lingering) {
            if (event.readable) Discard(handler, index);
            else if (event.failed) Drop(&handler, index, connection->closeReason);
        } else if (event.readable && !connection->closing) Read(handler, index);
        else if (event.failed) Drop(&handler, index, "connection lost");
    }
    {
        std::lock_guard<std::mutex> lock(postMutex_);
        running_.swap(posted_);
    }
    for (auto& task : running_) task();
    running_.clear();

    const double now = Now();
    if (now - lastTick_ >= 0.25) {
        lastTick_ = now;
        for (uint32_t index = 0; index < connections_.size(); ++index) {
            Connection& connection = connections_[index];
            if (!connection.open || connection.idleSeconds <= 0 || now - connection.lastFrame <= connection.idleSeconds) continue;
            if (connection.lingering) { Drop(&handler, index, connection.closeReason); continue; }
            ++stats_.idleCloses;
            Drop(&handler, index, "idle");
        }
        handler.OnTick(now);
    }
    // Closes asked for inside callbacks; OnClose may ask for more.
    for (size_t i = 0; i < doomed_.size(); ++i) {
        const Doomed doomed = doomed_[i];
        if (Find(doomed.id)) Drop(&handler, static_cast<uint32_t>(doomed.id & 0xffffffffu), doomed.reason);
    }
    doomed_.clear();
}

void FrameServer::Run(Handler& handler, const std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_relaxed)) RunOnce(handler, 100);
}

}  // namespace rpg
