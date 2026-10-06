// oe_server_tests: unit tests of server/common and the login server's rules.
// Exit code 0 = every check passed.
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "common/Crypto.h"
#include "common/Net.h"
#include "common/Protocol.h"
#include "common/Store.h"
#include "common/Ticket.h"
#include "common/Wire.h"
#include "common/Workers.h"
#include "channel/ChannelServer.h"
#include "login/LoginServer.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace rpg;

namespace {
int g_failed = 0, g_checks = 0;
#define CHECK(condition) \
    do { ++g_checks; if (!(condition)) { ++g_failed; std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

std::string HexOf(const Digest& digest) { return Hex(digest.data(), digest.size()); }
std::string Repeat(char c, size_t count) { return std::string(count, c); }

void TestSha256() {
    CHECK(HexOf(Sha256Of("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(HexOf(Sha256Of("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(HexOf(Sha256Of(two.data(), two.size())) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a', fed in uneven pieces: the buffering across Update calls.
    Sha256 hash;
    const std::string chunk = Repeat('a', 1000);
    size_t fed = 0;
    for (size_t piece = 1; fed < 1000000; piece = piece % 997 + 1) {
        const size_t take = piece < 1000000 - fed ? piece : 1000000 - fed;
        hash.Update(chunk.data(), take);
        fed += take;
    }
    CHECK(HexOf(hash.Finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void TestHmac() {
    // RFC 4231 test cases 1, 2, 3 and 6.
    const std::string key1 = Repeat('\x0b', 20);
    CHECK(HexOf(HmacKey(key1.data(), key1.size()).Mac("Hi There", 8)) == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    const std::string data2 = "what do ya want for nothing?";
    CHECK(HexOf(HmacKey("Jefe", 4).Mac(data2.data(), data2.size())) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    const std::string key3 = Repeat('\xaa', 20), data3 = Repeat('\xdd', 50);
    CHECK(HexOf(HmacKey(key3.data(), key3.size()).Mac(data3.data(), data3.size())) == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
    const std::string key6 = Repeat('\xaa', 131), data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    CHECK(HexOf(HmacKey(key6.data(), key6.size()).Mac(data6.data(), data6.size())) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

std::string Derive(const std::string& password, const std::string& salt, uint32_t iterations, size_t size) {
    std::vector<uint8_t> out(size);
    Pbkdf2(password.data(), password.size(), salt.data(), salt.size(), iterations, out.data(), out.size());
    return Hex(out.data(), out.size());
}

void TestPbkdf2() {
    // The PBKDF2-HMAC-SHA256 counterparts of the RFC 6070 vectors.
    CHECK(Derive("password", "salt", 1, 32) == "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(Derive("password", "salt", 2, 32) == "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    CHECK(Derive("password", "salt", 4096, 32) == "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    CHECK(Derive("passwordPASSWORDpassword", "saltSALTsaltSALTsaltSALTsaltSALTsalt", 4096, 40) ==
          "348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9");
    // The fast inner loop against the plain definition: U(i+1) = HMAC(password, U(i)).
    const HmacKey key("pw", 2);
    Digest u = key.Mac("salt\0\0\0\1", 8), sum = u;
    for (int i = 1; i < 37; ++i) {
        u = key.Mac(u.data(), u.size());
        for (size_t k = 0; k < sum.size(); ++k) sum[k] ^= u[k];
    }
    CHECK(Derive("pw", "salt", 37, 32) == HexOf(sum));
    CHECK(ConstantTimeEqual("abcd", "abcd", 4) && !ConstantTimeEqual("abcd", "abce", 4) && !ConstantTimeEqual("xbcd", "abcd", 4));
}

void TestEncodings() {
    std::vector<uint8_t> bytes;
    for (size_t size = 0; size < 70; ++size) {
        std::vector<uint8_t> data(size);
        for (size_t i = 0; i < size; ++i) data[i] = static_cast<uint8_t>(i * 37 + size);
        const std::string text = Base64Url(data.data(), data.size());
        CHECK(text.size() == (size * 4 + 2) / 3 && Base64UrlDecode(text, bytes) && bytes == data);
        CHECK(HexDecode(Hex(data.data(), data.size()), bytes) && bytes == data);
    }
    const uint8_t sample[] = {0xfb, 0xff, 0xfe};
    CHECK(Base64Url(sample, 3) == "-__-");
    CHECK(!Base64UrlDecode("a", bytes) && !Base64UrlDecode("ab+d", bytes) && !Base64UrlDecode("ab=", bytes));
    CHECK(!Base64UrlDecode("QR", bytes));  // unused low bits set: not the one text of its value
    CHECK(!HexDecode("abc", bytes) && !HexDecode("zz", bytes));
    uint8_t a[16] = {}, b[16] = {};
    CHECK(RandomBytes(a, sizeof(a)) && RandomBytes(b, sizeof(b)) && std::memcmp(a, b, sizeof(a)) != 0);
}

void TestTickets() {
    TicketKey key(32), other(32);
    for (size_t i = 0; i < 32; ++i) { key[i] = static_cast<uint8_t>(i); other[i] = static_cast<uint8_t>(i + 1); }
    Ticket ticket;
    ticket.kind = Ticket::Entry;
    ticket.account = 0x01020304;
    ticket.server = 7;
    ticket.expires = 1000;
    ticket.nonce = 0xdeadbeef;
    const std::string text = SealTicket(ticket, key);
    CHECK(text.size() == Ticket::kChars);
    Ticket opened;
    CHECK(OpenTicket(text, key, Ticket::Entry, 1000, opened));
    CHECK(opened.kind == Ticket::Entry && opened.account == 0x01020304 && opened.server == 7 && opened.expires == 1000 && opened.nonce == 0xdeadbeef);
    CHECK(!OpenTicket(text, key, Ticket::Entry, 1001, opened));    // expired
    CHECK(!OpenTicket(text, key, Ticket::Session, 1000, opened));  // the other kind
    CHECK(!OpenTicket(text, other, Ticket::Entry, 1000, opened));  // another key
    CHECK(!OpenTicket(text.substr(1), key, Ticket::Entry, 1000, opened) && !OpenTicket(text + "A", key, Ticket::Entry, 1000, opened));
    // Any changed character is refused: either the fields no longer match the signature or the signature itself changed.
    for (size_t i = 0; i < text.size(); ++i) {
        std::string forged = text;
        forged[i] = forged[i] == 'A' ? 'B' : 'A';
        CHECK(!OpenTicket(forged, key, Ticket::Entry, 0, opened));
    }
    // Random text never opens.
    std::mt19937 random(11);
    for (int i = 0; i < 20000; ++i) {
        std::string noise(Ticket::kChars, 'A');
        for (char& c : noise) c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[random() % 64];
        CHECK(!OpenTicket(noise, key, Ticket::Entry, 0, opened));
    }
    // Key files: created once, read back, a damaged one refused.
    const std::string path = "oe_server_tests.key";
    std::remove(path.c_str());
    TicketKey first, second;
    std::string error;
    CHECK(!LoadTicketKey(path, false, first, &error));
    CHECK(LoadTicketKey(path, true, first, &error) && first.size() == 32);
    CHECK(LoadTicketKey(path, true, second, &error) && second == first);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    std::fputs("not a key", file);
    std::fclose(file);
    CHECK(!LoadTicketKey(path, true, second, &error));
    std::remove(path.c_str());
}

void TestWire() {
    Writer out;
    out.U8(7).U16(0x1234).U32(0xcafebabe).Str8("name").Str8("");
    Reader in(out.Data(), out.Size());
    uint8_t a = 0; uint16_t b = 0; uint32_t c = 0; std::string s, t;
    CHECK(in.U8(a) && in.U16(b) && in.U32(c) && in.Str8(s, 16) && in.Str8(t, 16) && in.End());
    CHECK(a == 7 && b == 0x1234 && c == 0xcafebabe && s == "name" && t.empty());
    // Truncation at every length fails and stays failed.
    for (size_t cut = 0; cut < out.Size(); ++cut) {
        Reader shortened(out.Data(), cut);
        const bool all = shortened.U8(a) && shortened.U16(b) && shortened.U32(c) && shortened.Str8(s, 16) && shortened.Str8(t, 16);
        CHECK(!all && !shortened.End());
    }
    Reader bounded(out.Data(), out.Size());
    CHECK(bounded.U8(a) && bounded.U16(b) && bounded.U32(c) && !bounded.Str8(s, 3) && !bounded.Ok() && !bounded.U8(a));

    CHECK(ValidName("ab") && ValidName("Player_01") && ValidName("\xed\x99\x8d\xea\xb8\xb8\xeb\x8f\x99") && ValidName(Repeat('x', 16)));
    CHECK(!ValidName("") && !ValidName("a") && !ValidName(Repeat('x', 17)) && !ValidName("a b") && !ValidName("a\tb") && !ValidName(std::string("a\0b", 3)));
    CHECK(!ValidName("\xc0\xaf") && !ValidName("\xed\xa0\x80" "a") && !ValidName("ab\xe3\x80\x80") && !ValidName("\xff\xfe") && !ValidName("a\xe2\x82"));
}

void TestFrames() {
    auto frame = [](uint16_t type, const std::string& payload) {
        const uint32_t length = static_cast<uint32_t>(payload.size() + 2);
        std::string bytes;
        for (int i = 0; i < 4; ++i) bytes += static_cast<char>(length >> (8 * i));
        bytes += static_cast<char>(type);
        bytes += static_cast<char>(type >> 8);
        return bytes + payload;
    };
    // A stream of frames cut at random places gives the same frames back.
    std::mt19937 random(5);
    std::vector<std::pair<uint16_t, std::string>> sent;
    std::string stream;
    for (int i = 0; i < 400; ++i) {
        const size_t size = i % 50 == 0 ? kMaxPayload : random() % 300;
        std::string payload(size, '\0');
        for (char& c : payload) c = static_cast<char>(random());
        const uint16_t type = static_cast<uint16_t>(random());
        sent.push_back({type, payload});
        stream += frame(type, payload);
    }
    FrameBuffer buffer;
    size_t at = 0, received = 0;
    bool same = true;
    while (at < stream.size()) {
        size_t available = 0;
        uint8_t* space = buffer.Space(4096, available);
        size_t take = 1 + random() % 5000;
        if (take > available) take = available;
        if (take > stream.size() - at) take = stream.size() - at;
        std::memcpy(space, stream.data() + at, take);
        buffer.Commit(take);
        at += take;
        uint16_t type = 0; const uint8_t* payload = nullptr; size_t size = 0;
        int got;
        while ((got = buffer.Next(type, payload, size)) == 1) {
            same = same && received < sent.size() && sent[received].first == type && sent[received].second == std::string(reinterpret_cast<const char*>(payload), size);
            ++received;
        }
        same = same && got == 0;
    }
    CHECK(same && received == sent.size() && buffer.Buffered() == 0);

    // Lengths outside 2..65536 are malformed, whatever follows.
    for (uint32_t length : {0u, 1u, 65537u, 0x7fffffffu, 0xffffffffu}) {
        FrameBuffer bad;
        size_t available = 0;
        uint8_t* space = bad.Space(8, available);
        for (int i = 0; i < 4; ++i) space[i] = static_cast<uint8_t>(length >> (8 * i));
        bad.Commit(8);
        uint16_t type = 0; const uint8_t* payload = nullptr; size_t size = 0;
        CHECK(bad.Next(type, payload, size) == -1);
    }
    // Random bytes never read outside the buffer (run under a sanitizer to make that a hard check) and never loop.
    for (int round = 0; round < 2000; ++round) {
        FrameBuffer noise;
        for (int step = 0; step < 20; ++step) {
            size_t available = 0;
            uint8_t* space = noise.Space(64, available);
            const size_t take = available < 64 ? available : 64;
            for (size_t i = 0; i < take; ++i) space[i] = static_cast<uint8_t>(random() % (round % 2 ? 256 : 4));
            noise.Commit(take);
            uint16_t type = 0; const uint8_t* payload = nullptr; size_t size = 0;
            int got, frames = 0;
            while ((got = noise.Next(type, payload, size)) == 1) ++frames;
            CHECK(frames <= 16);
            if (got < 0) break;
        }
    }
}

void TestStore() {
    const std::string path = "oe_server_tests.db";
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
    std::string error;
    {
        Db db;
        CHECK(db.Open(path, &error));
        CHECK(db.Exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT UNIQUE COLLATE NOCASE, data BLOB)", &error));
        Statement insert, find, mode;
        CHECK(insert.Prepare(db, "INSERT INTO t (name, data) VALUES (?1, ?2)", &error) && find.Prepare(db, "SELECT id, name, data FROM t WHERE name = ?1", &error));
        CHECK(mode.Prepare(db, "PRAGMA journal_mode", &error) && mode.Step() && mode.Text(0) == "wal");
        const uint8_t blob[] = {0, 1, 2, 0, 255};
        insert.Bind(1, std::string("Alice"));
        insert.BindBlob(2, blob, sizeof(blob));
        CHECK(!insert.Step() && insert.Done() && db.LastInsertId() == 1);
        insert.Reset();
        insert.Bind(1, std::string("ALICE"));  // the same name in another case: refused by the constraint
        insert.BindBlob(2, blob, 1);
        CHECK(!insert.Step() && !insert.Done());
        insert.Reset();
        find.Bind(1, std::string("alice"));
        CHECK(find.Step() && find.Int(0) == 1 && find.Text(1) == "Alice" && find.Blob(2) == std::vector<uint8_t>(blob, blob + sizeof(blob)));
        find.Reset();
        find.Bind(1, std::string("bob"));
        CHECK(!find.Step() && find.Done());
        CHECK(!db.Exec("NOT SQL", &error) && !error.empty());
        Statement broken;
        CHECK(!broken.Prepare(db, "SELECT * FROM missing", &error));
    }
    {
        Db again;  // what was committed is there after reopening
        Statement count;
        CHECK(again.Open(path, &error) && count.Prepare(again, "SELECT COUNT(*) FROM t", &error) && count.Step() && count.Int(0) == 1);
    }
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
}

void TestWorkers() {
    std::atomic<int> ran{0};
    std::atomic<bool> hold{true};
    {
        TaskQueue queue(1, 2);
        CHECK(queue.Push([&] { while (hold) std::this_thread::yield(); ++ran; }));
        while (queue.Pending()) std::this_thread::yield();  // the thread took it and waits
        CHECK(queue.Push([&] { ++ran; }) && queue.Push([&] { ++ran; }));
        CHECK(!queue.Push([&] { ++ran; }));  // full: refused, never blocks the caller
        hold = false;
        queue.Stop();  // runs what is queued
        CHECK(ran == 3 && !queue.Push([&] { ++ran; }));
    }
    {
        TaskQueue pool(4, 1000);
        std::atomic<int> sum{0};
        for (int i = 1; i <= 500; ++i) CHECK(pool.Push([&sum, i] { sum += i; }));
        pool.Stop();
        CHECK(sum == 125250 && pool.Threads() == 4);
    }
}

// ----- a blocking client for the server tests ---------------------------------

struct Client {
#ifdef _WIN32
    SOCKET s = INVALID_SOCKET;
#else
    int s = -1;
#endif
    explicit Client(uint16_t port) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    }
    ~Client() { Shut(); }
    void Shut() {
#ifdef _WIN32
        if (s != INVALID_SOCKET) closesocket(s);
        s = INVALID_SOCKET;
#else
        if (s >= 0) close(s);
        s = -1;
#endif
    }
    void Raw(const std::string& bytes) { send(s, bytes.data(), static_cast<int>(bytes.size()), 0); }
    void Send(uint16_t type, const Writer& payload) {
        Writer frame;
        frame.U32(static_cast<uint32_t>(payload.Size() + 2)).U16(type).Bytes(payload.Data(), payload.Size());
        Raw(std::string(reinterpret_cast<const char*>(frame.Data()), frame.Size()));
    }
    bool Exact(char* out, size_t size) {
        for (size_t got = 0; got < size;) {
            const auto n = recv(s, out + got, static_cast<int>(size - got), 0);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }
    // False when the server closed the connection instead of sending a frame.
    bool Receive(uint16_t& type, std::string& payload) {
        char header[6];
        if (!Exact(header, 6)) return false;
        const uint32_t length = static_cast<uint8_t>(header[0]) | (static_cast<uint8_t>(header[1]) << 8) | (static_cast<uint8_t>(header[2]) << 16) |
                                (static_cast<uint32_t>(static_cast<uint8_t>(header[3])) << 24);
        type = static_cast<uint16_t>(static_cast<uint8_t>(header[4]) | (static_cast<uint8_t>(header[5]) << 8));
        payload.assign(length - 2, '\0');
        return length == 2 || Exact(&payload[0], length - 2);
    }
};

// The server runs on its own thread, like in the real process; the test is a blocking client.
template <typename HandlerType>
struct Running {
    FrameServer& server;
    HandlerType& handler;
    std::atomic<bool> stop{false};
    std::thread thread;
    Running(FrameServer& s, HandlerType& h) : server(s), handler(h), thread([this] { server.Run(handler, stop); }) {}
    ~Running() { stop = true; thread.join(); }
};

struct Echo final : FrameServer::Handler {
    FrameServer* server = nullptr;
    std::atomic<int> opened{0}, closed{0};
    std::string lastReason;
    void OnOpen(FrameServer::ConnId, const std::string&) override { ++opened; }
    void OnFrame(FrameServer::ConnId id, uint16_t type, const uint8_t* payload, size_t size) override {
        if (type == 99) { server->Send(id, 100, "bye", 3); server->Close(id, "asked", true); return; }
        if (type == 98) { for (int i = 0; i < 100000; ++i) if (!server->Send(id, 1, payload, size)) break; return; }  // floods its own queue
        server->Send(id, static_cast<uint16_t>(type + 1), payload, size);
    }
    void OnClose(FrameServer::ConnId, const char* reason) override { lastReason = reason; ++closed; }
};

void WaitFor(const std::atomic<int>& value, int wanted) {
    for (int i = 0; i < 2000 && value < wanted; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void TestFrameServer() {
    FrameServer server;
    Echo echo;
    echo.server = &server;
    ServerConfig config;
    config.maxConnections = 3;
    config.idleSeconds = 0.6;
    std::string error;
    CHECK(server.Listen(config, &error) && server.Port() != 0);
    {
        FrameServer second;
        ServerConfig taken;
        taken.port = server.Port();
        CHECK(!second.Listen(taken, &error));
    }
    Running<Echo> running(server, echo);
    uint16_t type = 0;
    std::string payload;
    {
        // Frames come back in order, also when several arrive in one segment or one arrives in pieces.
        Client client(server.Port());
        Writer a, b, big;
        a.Bytes("hello", 5);
        big.Bytes(std::string(kMaxPayload, 'x').data(), kMaxPayload);
        client.Send(10, a);
        CHECK(client.Receive(type, payload) && type == 11 && payload == "hello");
        std::string joined;
        for (int i = 0; i < 50; ++i) {
            Writer frame;
            frame.U32(3).U16(static_cast<uint16_t>(200 + i)).U8(static_cast<uint8_t>(i));
            joined.append(reinterpret_cast<const char*>(frame.Data()), frame.Size());
        }
        client.Raw(joined.substr(0, 7));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        client.Raw(joined.substr(7));
        bool ordered = true;
        for (int i = 0; i < 50; ++i) ordered = ordered && client.Receive(type, payload) && type == 201 + i && payload.size() == 1 && payload[0] == static_cast<char>(i);
        CHECK(ordered);
        client.Send(20, big);
        CHECK(client.Receive(type, payload) && type == 21 && payload.size() == kMaxPayload);
        client.Send(40, b);
        CHECK(client.Receive(type, payload) && type == 41 && payload.empty());
        // A last answer is delivered before the server closes.
        client.Send(99, b);
        CHECK(client.Receive(type, payload) && type == 100 && payload == "bye" && !client.Receive(type, payload));
        client.Shut();
        WaitFor(echo.closed, 1);
        CHECK(echo.closed == 1 && echo.lastReason == "asked");
    }
    {
        // A malformed length closes the connection.
        Client client(server.Port());
        client.Raw(std::string("\xff\xff\xff\x7f\x01\x00", 6));
        CHECK(!client.Receive(type, payload));
        WaitFor(echo.closed, 2);
        CHECK(echo.lastReason == "malformed frame");
    }
    {
        // Posted tasks run on the server thread; a silent connection is closed as idle.
        std::atomic<int> posted{0};
        std::thread::id serverThread;
        server.Post([&] { serverThread = std::this_thread::get_id(); ++posted; });
        WaitFor(posted, 1);
        CHECK(posted == 1 && serverThread == running.thread.get_id());
        Client silent(server.Port());
        CHECK(!silent.Receive(type, payload));
        WaitFor(echo.closed, 3);
        CHECK(echo.lastReason == "idle");
    }
    {
        // A reader that never reads: its queue is bounded, the server closes it and keeps serving others.
        Client stuck(server.Port());
        Writer chunk;
        chunk.Bytes(std::string(60000, 'y').data(), 60000);
        stuck.Send(98, chunk);
        WaitFor(echo.closed, 4);
        CHECK(echo.closed == 4 && echo.lastReason == "send queue overflow");
        Client fine(server.Port());
        Writer a;
        a.Bytes("ok", 2);
        fine.Send(1, a);
        CHECK(fine.Receive(type, payload) && type == 2 && payload == "ok");
    }
    {
        // More connections than the limit: the extra ones are closed at once.
        WaitFor(echo.closed, echo.opened.load());
        const int before = echo.opened;
        Client one(server.Port()), two(server.Port()), three(server.Port()), four(server.Port());
        WaitFor(echo.opened, before + 3);
        CHECK(!four.Receive(type, payload));
        Writer a;
        a.Bytes("x", 1);
        one.Send(1, a);
        CHECK(one.Receive(type, payload) && type == 2);
    }
    WaitFor(echo.closed, echo.opened);
    CHECK(echo.closed == echo.opened);
    CHECK(server.Stats().refused >= 1 && server.Stats().badFrames == 1 && server.Stats().overflows >= 1 && server.Stats().idleCloses >= 1);
}

// One request on a fresh connection, as the game does it. Returns the answer's type (0 = closed without one).
uint16_t Ask(uint16_t port, uint16_t type, const Writer& request, std::string& answer, uint16_t version = kProtocolVersion) {
    Client client(port);
    Writer hello;
    hello.U16(version).U8(RoleClient);
    client.Send(MsgHello, hello);
    client.Send(type, request);
    uint16_t got = 0;
    std::string extra;
    if (!client.Receive(got, answer)) return 0;
    uint16_t more = 0;
    CHECK(!client.Receive(more, extra));  // one answer, then the server closes
    return got;
}
uint16_t ErrorOf(const std::string& answer) {
    return answer.size() == 2 ? static_cast<uint16_t>(static_cast<uint8_t>(answer[0]) | (static_cast<uint8_t>(answer[1]) << 8)) : 0xffff;
}
struct Result {
    uint32_t account = 0;
    std::string name, ticket, proof;
};
bool Parse(const std::string& answer, Result& result) {
    Reader in(reinterpret_cast<const uint8_t*>(answer.data()), answer.size());
    return in.U32(result.account) && in.Str8(result.name, 64) && in.Str8(result.ticket, 64) && in.Str8(result.proof, 64) && in.End();
}

void TestLoginServer() {
    const std::string path = "oe_server_tests_accounts.db";
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
    TicketKey key(32, 9);
    LoginOptions options;
    options.database = path;
    options.workers = 2;
    options.iterations = 2000;  // the real cost is measured by oe_loadtest, not here
    options.loginLimit = 0;
    options.guestLimit = 0;
    std::string error, answer;
    Result result;
    Writer credentials, empty;
    auto with = [&](const std::string& name, const std::string& password) -> const Writer& {
        credentials.Clear();
        credentials.Str8(name).Str8(password);
        return credentials;
    };
    uint32_t alice = 0;
    {
        FrameServer server;
        LoginServer login(server, key, options);
        CHECK(login.Open(&error) && server.Listen(ServerConfig(), &error));
        const uint16_t port = server.Port();
        {
            Running<LoginServer> running(server, login);
            // Register, then log in; the ticket is a session ticket for that account, signed with the key.
            CHECK(Ask(port, MsgRegister, with("Alice", "correct horse"), answer) == MsgLoginResult && Parse(answer, result) && result.name == "Alice");
            alice = result.account;
            Ticket ticket;
            CHECK(alice != 0 && OpenTicket(result.ticket, key, Ticket::Session, UnixNow(), ticket) && ticket.account == alice && ticket.server == 0);
            CHECK(CheckNameProof(key, alice, "Alice", result.proof) && !CheckNameProof(key, alice, "Alicf", result.proof) && !CheckNameProof(key, alice + 1, "Alice", result.proof));
            CHECK(ticket.expires >= UnixNow() + Ticket::kSessionSeconds - 5 && ticket.expires <= UnixNow() + Ticket::kSessionSeconds + 5);
            CHECK(Ask(port, MsgLogin, with("alice", "correct horse"), answer) == MsgLoginResult && Parse(answer, result) && result.account == alice && result.name == "Alice");
            Ticket again;
            CHECK(OpenTicket(result.ticket, key, Ticket::Session, UnixNow(), again) && again.nonce != ticket.nonce);
            // Wrong password and unknown name answer alike.
            CHECK(Ask(port, MsgLogin, with("Alice", "wrong horse!"), answer) == MsgError && ErrorOf(answer) == ErrCredentials);
            CHECK(Ask(port, MsgLogin, with("Nobody", "correct horse"), answer) == MsgError && ErrorOf(answer) == ErrCredentials);
            CHECK(Ask(port, MsgLogin, with("Alice", "short"), answer) == MsgError && ErrorOf(answer) == ErrCredentials);
            // Registration rules.
            CHECK(Ask(port, MsgRegister, with("ALICE", "another password"), answer) == MsgError && ErrorOf(answer) == ErrNameTaken);
            CHECK(Ask(port, MsgRegister, with("a", "long enough"), answer) == MsgError && ErrorOf(answer) == ErrBadName);
            CHECK(Ask(port, MsgRegister, with("has space", "long enough"), answer) == MsgError && ErrorOf(answer) == ErrBadName);
            CHECK(Ask(port, MsgRegister, with("Guest0007", "long enough"), answer) == MsgError && ErrorOf(answer) == ErrBadName);
            CHECK(Ask(port, MsgRegister, with("Bob", "short"), answer) == MsgError && ErrorOf(answer) == ErrBadPassword);
            CHECK(Ask(port, MsgRegister, with("Bob", std::string(129, 'p')), answer) == MsgError && ErrorOf(answer) == ErrBadPassword);
            CHECK(Ask(port, MsgRegister, with("\xed\x99\x8d\xea\xb8\xb8\xeb\x8f\x99", "password1"), answer) == MsgLoginResult && Parse(answer, result) && result.account != alice);
            // Guests: an account each, named after it, with no password to log in with.
            Result first, second;
            CHECK(Ask(port, MsgGuest, empty, answer) == MsgLoginResult && Parse(answer, first));
            CHECK(Ask(port, MsgGuest, empty, answer) == MsgLoginResult && Parse(answer, second));
            CHECK(first.account != second.account && first.name != second.name && first.name.compare(0, 5, "Guest") == 0 && first.name.size() >= 9);
            CHECK(OpenTicket(first.ticket, key, Ticket::Session, UnixNow(), ticket) && ticket.account == first.account);
            CHECK(Ask(port, MsgLogin, with(first.name, "anything at all"), answer) == MsgError && ErrorOf(answer) == ErrCredentials);
            // Protocol errors.
            CHECK(Ask(port, MsgGuest, empty, answer, 999) == MsgError && ErrorOf(answer) == ErrVersion);
            CHECK(Ask(port, 777, empty, answer) == MsgError && ErrorOf(answer) == ErrBadRequest);
            credentials.Clear();
            credentials.Str8("Alice");
            CHECK(Ask(port, MsgLogin, credentials, answer) == MsgError && ErrorOf(answer) == ErrBadRequest);  // truncated
            {
                Client noHello(port);
                noHello.Send(MsgGuest, empty);
                uint16_t type = 0;
                CHECK(noHello.Receive(type, answer) && type == MsgError && ErrorOf(answer) == ErrVersion);
            }
            // Many at once: every request is answered, the accounts are distinct.
            std::vector<std::thread> threads;
            std::vector<uint32_t> accounts(24, 0);
            for (size_t i = 0; i < accounts.size(); ++i)
                threads.emplace_back([&, i] {
                    Writer request, none;
                    std::string reply;
                    Result got;
                    request.Str8("user" + std::to_string(i)).Str8("password" + std::to_string(i));
                    if (Ask(port, MsgRegister, request, reply) == MsgLoginResult && Parse(reply, got) &&
                        Ask(port, MsgLogin, request, reply) == MsgLoginResult && Parse(reply, got)) accounts[i] = got.account;
                });
            for (std::thread& thread : threads) thread.join();
            bool distinct = true;
            for (size_t i = 0; i < accounts.size(); ++i)
                for (size_t k = 0; k < i; ++k) distinct = distinct && accounts[i] != 0 && accounts[i] != accounts[k];
            CHECK(distinct);
        }
        login.Stop();
        CHECK(login.Stats().registrations == 26 && login.Stats().guests == 2 && login.Stats().logins == 25);
    }
    {
        // A restart keeps the accounts; the per-address limits refuse before any hashing.
        options.loginLimit = 3;
        options.guestLimit = 2;
        FrameServer server;
        LoginServer login(server, key, options);
        CHECK(login.Open(&error) && server.Listen(ServerConfig(), &error));
        const uint16_t port = server.Port();
        Running<LoginServer> running(server, login);
        CHECK(Ask(port, MsgLogin, with("Alice", "correct horse"), answer) == MsgLoginResult && Parse(answer, result) && result.account == alice);
        CHECK(Ask(port, MsgLogin, with("Alice", "wrong horse!"), answer) == MsgError && ErrorOf(answer) == ErrCredentials);
        CHECK(Ask(port, MsgLogin, with("Alice", "wrong horse!"), answer) == MsgError && ErrorOf(answer) == ErrCredentials);
        CHECK(Ask(port, MsgLogin, with("Alice", "correct horse"), answer) == MsgError && ErrorOf(answer) == ErrRateLimited);
        CHECK(Ask(port, MsgRegister, with("Carol", "password1"), answer) == MsgError && ErrorOf(answer) == ErrRateLimited);
        CHECK(Ask(port, MsgGuest, empty, answer) == MsgLoginResult && Ask(port, MsgGuest, empty, answer) == MsgLoginResult);
        CHECK(Ask(port, MsgGuest, empty, answer) == MsgError && ErrorOf(answer) == ErrRateLimited);
    }
    for (const char* suffix : {"", "-wal", "-shm"}) std::remove((path + suffix).c_str());
}

// Skips the periodic channel list unless that is what is wanted. 0 = the server closed.
uint16_t Next(Client& client, std::string& payload, bool wantChannels = false) {
    uint16_t type = 0;
    while (client.Receive(type, payload)) {
        if (type != MsgChannels || wantChannels) return type;
    }
    return 0;
}

void TestChannelServer() {
    TicketKey key(32, 4), wrongKey(32, 5);
    const std::string linkKey = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    ChannelOptions options;
    options.channels = 2;
    FrameServer server;
    ChannelServer channel(server, key, linkKey, options);
    std::string error, payload;
    CHECK(server.Listen(ServerConfig(), &error));
    const int gameListener = server.AddListener("127.0.0.1", 0, &error);
    CHECK(gameListener == 1 && server.ListenerPort(1) != 0 && server.ListenerPort(1) != server.Port());
    channel.SetGameListener(gameListener);
    const uint16_t port = server.Port(), gamePort = server.ListenerPort(1);
    Running<ChannelServer> running(server, channel);

    auto session = [&](Client& client, uint32_t account, const std::string& name, const TicketKey& signer, uint32_t expires = 0) {
        Ticket ticket;
        ticket.kind = Ticket::Session;
        ticket.account = account;
        ticket.expires = expires ? expires : UnixNow() + 60;
        ticket.nonce = account * 7;
        Writer hello, request;
        hello.U16(kProtocolVersion).U8(RoleClient);
        request.Str8(SealTicket(ticket, signer)).Str8(name).Str8(NameProof(signer, account, name));
        client.Send(MsgHello, hello);
        client.Send(MsgSession, request);
    };
    auto enter = [&](Client& client, uint8_t wanted) {
        Writer request;
        request.U8(wanted);
        client.Send(MsgEnter, request);
    };
    auto gameHello = [&](Client& game, const std::string& presented) {
        Writer hello;
        hello.U16(kProtocolVersion).U8(RoleGame).Str8(presented);
        game.Send(MsgHello, hello);
    };
    auto registerGame = [&](Client& game, uint8_t number, uint16_t listen, uint16_t capacity) -> uint16_t {
        Writer request;
        request.U8(number).Str8("town").Str8("127.0.0.1").U16(listen).U16(capacity);
        game.Send(MsgGameRegister, request);
        uint16_t type = 0, id = 0;
        if (!game.Receive(type, payload) || type != MsgGameRegistered) return 0;
        Reader in(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        return in.U16(id) && in.End() ? id : 0;
    };
    auto heartbeat = [&](Client& game, uint16_t players) {
        Writer request;
        request.U16(players);
        game.Send(MsgHeartbeat, request);
    };
    struct Admission { uint32_t player = 0, account = 0; uint16_t code = 0xffff; std::string name; };
    auto admit = [&](Client& game, uint32_t player, const std::string& ticket) {
        Writer request;
        request.U32(player).Str8(ticket);
        game.Send(MsgAdmit, request);
        Admission result;
        uint16_t type = 0;
        if (!game.Receive(type, payload) || type != MsgAdmitResult) return result;
        Reader in(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        in.U32(result.player); in.U16(result.code); in.U32(result.account); in.Str8(result.name, 64);
        if (!in.End()) result.code = 0xfffe;
        return result;
    };
    struct Entered { std::string address, ticket, map; uint16_t port = 0; uint8_t channel = 0; };
    auto entered = [&](const std::string& bytes, Entered& out) {
        Reader in(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        return in.Str8(out.address, 64) && in.U16(out.port) && in.Str8(out.ticket, 64) && in.U8(out.channel) && in.Str8(out.map, 32) && in.End();
    };
    // channel list: per channel {players, capacity, online}
    auto list = [&](const std::string& bytes) {
        std::vector<std::array<int, 3>> out;
        Reader in(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        uint8_t count = 0;
        in.U8(count);
        for (int i = 0; i < count; ++i) {
            uint8_t number = 0, online = 0; uint16_t players = 0, capacity = 0;
            in.U8(number); in.U16(players); in.U16(capacity); in.U8(online);
            out.push_back({players, capacity, online});
        }
        if (!in.End()) out.clear();
        return out;
    };

    {
        // Tickets that are forged, expired, or presented with another name are refused.
        Client forged(port), expired(port), renamed(port), unasked(port);
        session(forged, 1, "Mallory", wrongKey);
        CHECK(Next(forged, payload) == MsgError && ErrorOf(payload) == ErrTicket && Next(forged, payload) == 0);
        session(expired, 1, "Old", key, UnixNow() - 5);
        CHECK(Next(expired, payload) == MsgError && ErrorOf(payload) == ErrTicket);
        Ticket ticket;
        ticket.kind = Ticket::Session; ticket.account = 9; ticket.expires = UnixNow() + 60;
        Writer hello, request;
        hello.U16(kProtocolVersion).U8(RoleClient);
        request.Str8(SealTicket(ticket, key)).Str8("Admin").Str8(NameProof(key, 9, "Nine"));
        renamed.Send(MsgHello, hello);
        renamed.Send(MsgSession, request);
        CHECK(Next(renamed, payload) == MsgError && ErrorOf(payload) == ErrTicket);
        unasked.Send(MsgHello, hello);
        Writer wanted;
        wanted.U8(1);
        unasked.Send(MsgEnter, wanted);  // Enter without a session
        CHECK(Next(unasked, payload) == MsgError && ErrorOf(payload) == ErrBadRequest);
    }
    {
        // A game server must know the link key and register before it asks anything.
        Client stranger(gamePort), early(gamePort), asClient(gamePort);
        gameHello(stranger, std::string(64, '0'));
        CHECK(Next(stranger, payload) == MsgError && ErrorOf(payload) == ErrTicket);
        gameHello(early, linkKey);
        heartbeat(early, 1);
        CHECK(Next(early, payload) == MsgError && ErrorOf(payload) == ErrNotRegistered);
        Writer hello;
        hello.U16(kProtocolVersion).U8(RoleClient);
        asClient.Send(MsgHello, hello);  // a player on the game servers' port
        CHECK(Next(asClient, payload) == MsgError && ErrorOf(payload) == ErrVersion);
    }

    Client alice(port);
    session(alice, 11, "Alice", key);
    CHECK(Next(alice, payload, true) == MsgChannels && list(payload) == (std::vector<std::array<int, 3>>{{0, 0, 0}, {0, 0, 0}}));
    enter(alice, 0);
    CHECK(Next(alice, payload) == MsgError && ErrorOf(payload) == ErrChannelOffline);  // and the link stays open

    Client one(gamePort), two(gamePort);
    gameHello(one, linkKey);
    gameHello(two, linkKey);
    const uint16_t firstId = registerGame(one, 1, 7778, 2);
    const uint16_t secondId = registerGame(two, 2, 7779, 7);
    CHECK(firstId != 0 && secondId != 0 && firstId != secondId);
    Client bad(gamePort);
    gameHello(bad, linkKey);
    CHECK(registerGame(bad, 3, 7780, 7) == 0);  // only channels 1..2 exist
    heartbeat(one, 1);
    heartbeat(two, 3);

    // Enter: a named channel, or 0 for the emptiest with room. The ticket is for exactly that server.
    Entered first, second;
    enter(alice, 1);
    CHECK(Next(alice, payload) == MsgEnterResult && entered(payload, first) && Next(alice, payload) == 0);
    CHECK(first.address == "127.0.0.1" && first.port == 7778 && first.channel == 1 && first.map == "town" && first.ticket.size() == Ticket::kChars);
    Ticket opened;
    CHECK(OpenTicket(first.ticket, key, Ticket::Entry, UnixNow(), opened) && opened.account == 11 && opened.server == firstId);
    CHECK(opened.expires <= UnixNow() + Ticket::kEntrySeconds);

    // Channel 1 now has one player and one promised place of two: full. 0 leads to channel 2.
    Client bob(port);
    session(bob, 12, "Bob", key);
    CHECK(Next(bob, payload, true) == MsgChannels);
    enter(bob, 1);
    CHECK(Next(bob, payload) == MsgError && ErrorOf(payload) == ErrChannelFull);
    enter(bob, 0);
    CHECK(Next(bob, payload) == MsgEnterResult && entered(payload, second) && second.channel == 2 && second.port == 7779);

    // Admit: the right server, once.
    Admission wrongServer = admit(two, 5, first.ticket);
    CHECK(wrongServer.code == ErrTicket && wrongServer.player == 5);
    Admission ok = admit(one, 2, first.ticket);
    CHECK(ok.code == 0 && ok.player == 2 && ok.account == 11 && ok.name == "Alice");
    CHECK(admit(one, 3, first.ticket).code == ErrTicket);  // used
    CHECK(admit(one, 4, "not a ticket").code == ErrTicket);
    Ticket invented = opened;
    invented.nonce ^= 1;
    CHECK(admit(one, 4, SealTicket(invented, key)).code == ErrTicket);  // well signed, but never issued
    CHECK(admit(one, 4, SealTicket(opened, wrongKey)).code == ErrTicket);
    Admission other = admit(two, 9, second.ticket);
    CHECK(other.code == 0 && other.account == 12 && other.name == "Bob");

    // The list players watch follows the heartbeats, and a game server that goes away is offline.
    heartbeat(one, 2);
    heartbeat(two, 4);
    Client watcher(port);
    session(watcher, 13, "Carol", key);
    CHECK(Next(watcher, payload, true) == MsgChannels);
    bool seen = false;
    for (int i = 0; i < 3 && !seen; ++i)
        seen = Next(watcher, payload, true) == MsgChannels && list(payload) == (std::vector<std::array<int, 3>>{{2, 2, 1}, {4, 7, 1}});
    CHECK(seen);
    one.Shut();
    seen = false;
    for (int i = 0; i < 3 && !seen; ++i)
        seen = Next(watcher, payload, true) == MsgChannels && list(payload) == (std::vector<std::array<int, 3>>{{0, 0, 0}, {4, 7, 1}});
    CHECK(seen);
    enter(watcher, 1);
    CHECK(Next(watcher, payload) == MsgError && ErrorOf(payload) == ErrChannelOffline);

    // A restarted game server takes its place again (new id); a second one for the same place replaces the first.
    Client again(gamePort), replacement(gamePort);
    gameHello(again, linkKey);
    const uint16_t againId = registerGame(again, 1, 7778, 7);
    CHECK(againId != 0 && againId != firstId);
    gameHello(replacement, linkKey);
    const uint16_t replacementId = registerGame(replacement, 1, 7790, 7);
    CHECK(replacementId != 0 && replacementId != againId);
    uint16_t type = 0;
    CHECK(!again.Receive(type, payload));  // the old link was closed
    Entered third;
    enter(watcher, 1);
    CHECK(Next(watcher, payload) == MsgEnterResult && entered(payload, third) && third.port == 7790);
    CHECK(admit(replacement, 2, third.ticket).code == 0);
    CHECK(admit(replacement, 2, first.ticket).code == ErrTicket);  // a ticket for the server that was there before
}

}  // namespace

int main() {
    struct { const char* name; void (*run)(); } tests[] = {
        {"Sha256", TestSha256}, {"Hmac", TestHmac}, {"Pbkdf2", TestPbkdf2}, {"Encodings", TestEncodings}, {"Tickets", TestTickets},
        {"Wire", TestWire}, {"Frames", TestFrames}, {"Store", TestStore}, {"Workers", TestWorkers}, {"FrameServer", TestFrameServer},
        {"LoginServer", TestLoginServer}, {"ChannelServer", TestChannelServer}};
    for (const auto& test : tests) {
        const int before = g_failed;
        test.run();
        std::printf("%s %s\n", g_failed == before ? "PASS" : "FAIL", test.name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
