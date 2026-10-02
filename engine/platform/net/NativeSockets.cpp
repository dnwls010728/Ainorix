#include "platform/Network.h"

#include <limits>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace oe {
bool NetworkRandom(uint8_t* bytes, size_t size) {
    if (!bytes || size == 0 || size > 256) return false;
#ifdef _WIN32
    return BCryptGenRandom(nullptr, bytes, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    int fd = open("/dev/urandom", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, bytes + offset, size - offset);
        if (count <= 0) { close(fd); return false; }
        offset += static_cast<size_t>(count);
    }
    close(fd);
    return true;
#endif
}
namespace {
#ifdef _WIN32
using Handle = SOCKET;
using AddressLength = int;
constexpr Handle kInvalid = INVALID_SOCKET;
int LastCode() { return WSAGetLastError(); }
bool WouldBlock(int code) { return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS || code == WSAEALREADY; }
void CloseHandle(Handle handle) { closesocket(handle); }
bool StartSockets() { WSADATA data{}; return WSAStartup(MAKEWORD(2, 2), &data) == 0; }
void StopSockets() { WSACleanup(); }
#else
using Handle = int;
using AddressLength = socklen_t;
constexpr Handle kInvalid = -1;
int LastCode() { return errno; }
bool WouldBlock(int code) { return code == EAGAIN || code == EWOULDBLOCK || code == EINPROGRESS || code == EALREADY || code == EINTR; }
void CloseHandle(Handle handle) { close(handle); }
bool StartSockets() { return true; }
void StopSockets() {}
#endif

std::string ErrorText(const char* action) { return std::string(action) + " failed (socket error " + std::to_string(LastCode()) + ")"; }
sockaddr_in Address(const NetAddress& address) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_port = htons(address.port);
    inet_pton(AF_INET, address.host.c_str(), &result.sin_addr);
    return result;
}
NetAddress Address(const sockaddr_in& address) {
    char host[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address.sin_addr, host, sizeof(host));
    return {host, ntohs(address.sin_port)};
}
bool Configure(Handle handle, SocketKind kind, std::string* error) {
#ifdef _WIN32
    u_long nonblocking = 1;
    bool ok = ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
#else
    int flags = fcntl(handle, F_GETFL, 0);
    bool ok = flags >= 0 && fcntl(handle, F_SETFL, flags | O_NONBLOCK) == 0;
    int descriptorFlags = fcntl(handle, F_GETFD, 0);
    ok = ok && descriptorFlags >= 0 && fcntl(handle, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0;
#endif
    if (ok && kind == SocketKind::Tcp) {
        int enabled = 1;
        ok = setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) == 0;
#ifdef SO_NOSIGPIPE
        ok = ok && setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#endif
    }
    if (ok) {
        int bufferBytes = 256 * 1024;
        ok = setsockopt(handle, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes)) == 0 &&
             setsockopt(handle, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufferBytes), sizeof(bufferBytes)) == 0;
    }
    if (!ok && error) *error = ErrorText("nonblocking/NODELAY setup");
    return ok;
}
int SendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

class NativeSocket final : public NetSocket {
public:
    NativeSocket(Handle handle, SocketKind kind) : handle_(handle), kind_(kind) {}
    ~NativeSocket() override { CloseHandle(handle_); StopSockets(); }
    bool Bind(const NetAddress& address, std::string* error) override {
        if (!ValidNetAddress(address, true)) return Fail("Use a canonical numeric IPv4 bind address", error);
        auto native = Address(address);
        if (bind(handle_, reinterpret_cast<const sockaddr*>(&native), sizeof(native)) != 0) return Fail(ErrorText("bind"), error);
        return true;
    }
    bool Listen(std::string* error) override {
        if (kind_ != SocketKind::Tcp || listen(handle_, 64) != 0) return Fail(ErrorText("listen"), error);
        return true;
    }
    bool Connect(const NetAddress& address, std::string* error) override {
        if (kind_ != SocketKind::Tcp || !ValidNetAddress(address)) return Fail("TCP connect requires numeric IPv4 and a nonzero port", error);
        auto native = Address(address);
        int result = connect(handle_, reinterpret_cast<const sockaddr*>(&native), sizeof(native));
        if (result == 0) { state_ = SocketState::Open; return true; }
        if (WouldBlock(LastCode())) { state_ = SocketState::Connecting; return true; }
        state_ = SocketState::Closed;
        return Fail(ErrorText("connect"), error);
    }
    std::unique_ptr<NetSocket> Accept(NetAddress& address, std::string* error) override {
        if (error) error->clear();
        sockaddr_in native{};
        AddressLength length = sizeof(native);
        Handle child = accept(handle_, reinterpret_cast<sockaddr*>(&native), &length);
        if (child == kInvalid) {
            if (!WouldBlock(LastCode())) Fail(ErrorText("accept"), error);
            return nullptr;
        }
        if (!StartSockets()) { CloseHandle(child); Fail("socket runtime startup failed", error); return nullptr; }
        if (!Configure(child, SocketKind::Tcp, error)) { CloseHandle(child); StopSockets(); return nullptr; }
        address = Address(native);
        return std::make_unique<NativeSocket>(child, SocketKind::Tcp);
    }
    NetAddress LocalAddress() const override {
        sockaddr_in native{};
        AddressLength length = sizeof(native);
        if (getsockname(handle_, reinterpret_cast<sockaddr*>(&native), &length) != 0) return {};
        return Address(native);
    }
    SocketState State() override {
        if (state_ != SocketState::Connecting) return state_;
#ifdef _WIN32
        WSAPOLLFD item{handle_, POLLWRNORM, 0};
        int result = WSAPoll(&item, 1, 0);
#else
        pollfd item{handle_, POLLOUT, 0};
        int result = poll(&item, 1, 0);
#endif
        if (result < 0 && WouldBlock(LastCode())) return state_;
        if (result < 0) { Fail(ErrorText("poll"), nullptr); state_ = SocketState::Closed; }
        else if (result > 0) {
            int code = 0;
            AddressLength length = sizeof(code);
            if (getsockopt(handle_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) != 0 || code != 0) {
                error_ = "TCP connection failed (socket error " + std::to_string(code ? code : LastCode()) + ")";
                state_ = SocketState::Closed;
            } else state_ = SocketState::Open;
        }
        return state_;
    }
    const std::string& LastError() const override { return error_; }
    SocketIo Send(const uint8_t* bytes, size_t size, size_t& sent) override {
        sent = 0;
        if (size > static_cast<size_t>(std::numeric_limits<int>::max()) || (!bytes && size != 0)) return SocketIo::Error;
        if (size == 0) return SocketIo::Progress;
        auto count = send(handle_, reinterpret_cast<const char*>(bytes), static_cast<int>(size), SendFlags());
        if (count < 0) return IoError("send");
        sent = static_cast<size_t>(count);
        return count == 0 ? SocketIo::WouldBlock : SocketIo::Progress;
    }
    SocketIo Receive(uint8_t* bytes, size_t capacity, size_t& received) override {
        received = 0;
        if (!bytes || capacity == 0 || capacity > static_cast<size_t>(std::numeric_limits<int>::max())) return SocketIo::Error;
        auto count = recv(handle_, reinterpret_cast<char*>(bytes), static_cast<int>(capacity), 0);
        if (count < 0) return IoError("recv");
        if (count == 0) { state_ = SocketState::Closed; return SocketIo::Closed; }
        received = static_cast<size_t>(count);
        return SocketIo::Progress;
    }
    SocketIo SendTo(const NetAddress& address, const uint8_t* bytes, size_t size) override {
        if (!ValidNetAddress(address) || size > 65507 || (!bytes && size != 0)) return SocketIo::Error;
        const uint8_t empty = 0;
        auto native = Address(address);
        auto count = sendto(handle_, reinterpret_cast<const char*>(size ? bytes : &empty), static_cast<int>(size), SendFlags(),
                            reinterpret_cast<const sockaddr*>(&native), sizeof(native));
        if (count < 0) return IoError("sendto");
        return static_cast<size_t>(count) == size ? SocketIo::Progress : SocketIo::Error;
    }
    SocketIo ReceiveFrom(NetAddress& address, uint8_t* bytes, size_t capacity, size_t& received) override {
        received = 0;
        if (!bytes || capacity == 0 || capacity > 65536) return SocketIo::Error;
        sockaddr_in native{};
#ifdef _WIN32
        AddressLength length = sizeof(native);
        int count = recvfrom(handle_, reinterpret_cast<char*>(bytes), static_cast<int>(capacity), 0,
                             reinterpret_cast<sockaddr*>(&native), &length);
        if (count < 0) {
            if (LastCode() == WSAEMSGSIZE) { address = Address(native); return SocketIo::TooLarge; }
            if (LastCode() == WSAECONNRESET) return SocketIo::WouldBlock;
            return IoError("recvfrom");
        }
#else
        iovec buffer{bytes, capacity};
        msghdr message{};
        message.msg_name = &native;
        message.msg_namelen = sizeof(native);
        message.msg_iov = &buffer;
        message.msg_iovlen = 1;
        auto count = recvmsg(handle_, &message, 0);
        if (count < 0) return IoError("recvmsg");
        if ((message.msg_flags & MSG_TRUNC) != 0) { address = Address(native); return SocketIo::TooLarge; }
#endif
        address = Address(native);
        received = static_cast<size_t>(count);
        return SocketIo::Progress;
    }
private:
    bool Fail(const std::string& message, std::string* error) { error_ = message; if (error) *error = message; return false; }
    SocketIo IoError(const char* action) {
        if (WouldBlock(LastCode())) return SocketIo::WouldBlock;
        Fail(ErrorText(action), nullptr);
        if (kind_ == SocketKind::Tcp) state_ = SocketState::Closed;
        return SocketIo::Error;
    }
    Handle handle_;
    SocketKind kind_;
    SocketState state_ = SocketState::Open;
    std::string error_;
};
}

std::unique_ptr<NetSocket> CreateNetSocket(SocketKind kind, std::string* error) {
    if (error) error->clear();
    if (!StartSockets()) { if (error) *error = "socket runtime startup failed"; return nullptr; }
    Handle handle = socket(AF_INET, kind == SocketKind::Udp ? SOCK_DGRAM : SOCK_STREAM,
                           kind == SocketKind::Udp ? IPPROTO_UDP : IPPROTO_TCP);
    if (handle == kInvalid) { if (error) *error = ErrorText("socket"); StopSockets(); return nullptr; }
    if (!Configure(handle, kind, error)) { CloseHandle(handle); StopSockets(); return nullptr; }
    return std::make_unique<NativeSocket>(handle, kind);
}
std::unique_ptr<PlatformWebSocket> CreatePlatformWebSocket(const std::string&, std::string* error) {
    if (error) *error = "WebSocket client requires the Emscripten platform";
    return nullptr;
}
}  // namespace oe
