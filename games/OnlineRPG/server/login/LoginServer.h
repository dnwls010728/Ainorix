#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "common/Net.h"
#include "common/Store.h"
#include "common/Ticket.h"
#include "common/Workers.h"

namespace rpg {

struct LoginOptions {
    std::string database = "accounts.db";
    size_t workers = 0;            // password-hashing threads; 0 = one per core
    uint32_t iterations = 100000;  // PBKDF2 rounds for new passwords (about 50 ms on one core)
    uint32_t loginLimit = 5;       // Login + Register attempts per address per minute; 0 = unlimited
    uint32_t guestLimit = 20;      // Guest logins per address per minute; 0 = unlimited
};

// The login server's rules (link 1): Register, Login, Guest -> a signed session ticket.
//
// Threads: the I/O thread (FrameServer) parses and answers; the store thread is the only one that
// touches SQLite; the worker pool only hashes passwords. A request hops between them as tasks and
// its answer comes back to the I/O thread with FrameServer::Post, so the I/O thread never waits.
class LoginServer final : public FrameServer::Handler {
public:
    LoginServer(FrameServer& server, TicketKey key, const LoginOptions& options);
    ~LoginServer() override;
    bool Open(std::string* error);
    // Finishes queued work and joins the threads; call after the I/O loop stopped.
    void Stop();

    void OnOpen(FrameServer::ConnId id, const std::string& address) override;
    void OnFrame(FrameServer::ConnId id, uint16_t type, const uint8_t* payload, size_t size) override;
    void OnClose(FrameServer::ConnId id, const char* reason) override;
    void OnTick(double now) override;

    struct Counters {
        uint64_t logins = 0, registrations = 0, guests = 0, refused = 0, limited = 0, busy = 0;
    };
    const Counters& Stats() const { return counters_; }

private:
    struct Client {
        bool hello = false, pending = false;
    };
    struct Window {
        double start = 0;
        uint32_t count = 0;
    };
    struct Account {
        bool found = false;
        uint32_t id = 0, iterations = 0;
        std::string name;
        std::vector<uint8_t> salt, hash;
    };
    bool Allow(std::unordered_map<std::string, Window>& windows, const std::string& address, uint32_t limit);
    void Fail(FrameServer::ConnId id, uint16_t code);
    void Succeed(FrameServer::ConnId id, uint32_t account, const std::string& name);
    // Answers from another thread.
    void PostFail(FrameServer::ConnId id, uint16_t code);
    void PostSucceed(FrameServer::ConnId id, uint32_t account, const std::string& name);
    void Login(FrameServer::ConnId id, std::string name, std::string password);
    void Register(FrameServer::ConnId id, std::string name, std::string password);
    void Guest(FrameServer::ConnId id);
    Account Lookup(const std::string& name);  // store thread

    FrameServer& server_;
    TicketKey key_;
    LoginOptions options_;
    Db db_;
    Statement find_, insert_, insertGuest_, nameGuest_;
    std::unique_ptr<TaskQueue> store_, workers_;
    std::unordered_map<FrameServer::ConnId, Client> clients_;
    std::unordered_map<std::string, Window> loginWindows_, guestWindows_;
    Counters counters_;
    double now_ = 0, lastPrune_ = 0;
};

}  // namespace rpg
