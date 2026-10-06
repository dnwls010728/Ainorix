#include "login/LoginServer.h"

#include <cstdio>
#include <thread>

#include "common/Crypto.h"
#include "common/Log.h"
#include "common/Protocol.h"
#include "common/Wire.h"

namespace rpg {
namespace {
constexpr size_t kSaltBytes = 16, kHashBytes = 32;
constexpr size_t kMinPassword = 8, kMaxPassword = 128;

bool ReservedName(const std::string& name) {
    static const char kGuest[] = "guest";
    if (name.size() < 5) return false;
    for (size_t i = 0; i < 5; ++i)
        if ((name[i] | 0x20) != kGuest[i]) return false;
    return true;
}
}  // namespace

LoginServer::LoginServer(FrameServer& server, TicketKey key, const LoginOptions& options) : server_(server), key_(std::move(key)), options_(options) {
    if (!options_.workers) options_.workers = std::thread::hardware_concurrency() ? std::thread::hardware_concurrency() : 1;
    if (!options_.iterations) options_.iterations = 1;
}

LoginServer::~LoginServer() { Stop(); }

bool LoginServer::Open(std::string* error) {
    if (!db_.Open(options_.database, error)) return false;
    if (!db_.Exec("CREATE TABLE IF NOT EXISTS accounts ("
                  " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                  " name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
                  " salt BLOB, hash BLOB, iterations INTEGER NOT NULL DEFAULT 0,"
                  " guest INTEGER NOT NULL DEFAULT 0,"
                  " created INTEGER NOT NULL)", error)) return false;
    if (!find_.Prepare(db_, "SELECT id, name, salt, hash, iterations FROM accounts WHERE name = ?1 AND guest = 0", error) ||
        !insert_.Prepare(db_, "INSERT INTO accounts (name, salt, hash, iterations, guest, created) VALUES (?1, ?2, ?3, ?4, 0, ?5)", error) ||
        !insertGuest_.Prepare(db_, "INSERT INTO accounts (name, guest, created) VALUES (?1, 1, ?2)", error) ||
        !nameGuest_.Prepare(db_, "UPDATE accounts SET name = ?1 WHERE id = ?2", error)) return false;
    store_ = std::make_unique<TaskQueue>(1, 4096);
    workers_ = std::make_unique<TaskQueue>(options_.workers, options_.workers * 64);
    return true;
}

void LoginServer::Stop() {
    // A worker may still hand its result to the store: workers first.
    if (workers_) workers_->Stop();
    if (store_) store_->Stop();
}

void LoginServer::OnOpen(FrameServer::ConnId id, const std::string&) { clients_[id]; }
void LoginServer::OnClose(FrameServer::ConnId id, const char*) { clients_.erase(id); }

void LoginServer::OnTick(double now) {
    now_ = now;
    if (now - lastPrune_ < 30) return;
    lastPrune_ = now;
    for (auto* windows : {&loginWindows_, &guestWindows_})
        for (auto it = windows->begin(); it != windows->end();) it = now - it->second.start >= 60 ? windows->erase(it) : std::next(it);
}

bool LoginServer::Allow(std::unordered_map<std::string, Window>& windows, const std::string& address, uint32_t limit) {
    if (!limit) return true;
    const double now = FrameServer::Now();
    Window& window = windows[address];
    if (now - window.start >= 60) { window.start = now; window.count = 0; }
    if (window.count >= limit) return false;
    ++window.count;
    return true;
}

void LoginServer::Fail(FrameServer::ConnId id, uint16_t code) {
    if (code == ErrBusy) ++counters_.busy;
    else if (code == ErrRateLimited) ++counters_.limited;
    else ++counters_.refused;
    Writer out;
    out.U16(code);
    server_.Send(id, MsgError, out.Data(), out.Size());
    server_.Close(id, "answered", true);
}

void LoginServer::Succeed(FrameServer::ConnId id, uint32_t account, const std::string& name) {
    Ticket ticket;
    ticket.kind = Ticket::Session;
    ticket.account = account;
    ticket.expires = UnixNow() + Ticket::kSessionSeconds;
    RandomBytes(&ticket.nonce, sizeof(ticket.nonce));
    Writer out;
    out.U32(account).Str8(name).Str8(SealTicket(ticket, key_)).Str8(NameProof(key_, account, name));
    server_.Send(id, MsgLoginResult, out.Data(), out.Size());
    server_.Close(id, "answered", true);
}

void LoginServer::PostFail(FrameServer::ConnId id, uint16_t code) {
    server_.Post([this, id, code] { Fail(id, code); });
}
void LoginServer::PostSucceed(FrameServer::ConnId id, uint32_t account, const std::string& name) {
    server_.Post([this, id, account, name] { Succeed(id, account, name); });
}

void LoginServer::OnFrame(FrameServer::ConnId id, uint16_t type, const uint8_t* payload, size_t size) {
    Client& client = clients_[id];
    Reader in(payload, size);
    if (!client.hello) {
        uint16_t version = 0;
        uint8_t role = 0;
        in.U16(version);
        in.U8(role);
        if (type != MsgHello || !in.End() || version != kProtocolVersion || role != RoleClient) { Fail(id, ErrVersion); return; }
        client.hello = true;
        return;
    }
    if (client.pending) { Fail(id, ErrBadRequest); return; }  // one request per connection
    const std::string address = server_.Address(id);
    if (type == MsgGuest) {
        if (size != 0) { Fail(id, ErrBadRequest); return; }
        if (!Allow(guestWindows_, address, options_.guestLimit)) { Fail(id, ErrRateLimited); return; }
        client.pending = true;
        Guest(id);
    } else if (type == MsgLogin || type == MsgRegister) {
        std::string name, password;
        in.Str8(name, 64);
        in.Str8(password, 255);
        if (!in.End()) { Fail(id, ErrBadRequest); return; }
        // The limit comes before any hashing: a flood costs a table lookup per attempt.
        if (!Allow(loginWindows_, address, options_.loginLimit)) { Fail(id, ErrRateLimited); return; }
        const bool validPassword = password.size() >= kMinPassword && password.size() <= kMaxPassword;
        if (type == MsgRegister) {
            if (!ValidName(name) || ReservedName(name)) { Fail(id, ErrBadName); return; }
            if (!validPassword) { Fail(id, ErrBadPassword); return; }
            client.pending = true;
            Register(id, std::move(name), std::move(password));
        } else {
            if (!ValidName(name) || !validPassword) { Fail(id, ErrCredentials); return; }
            client.pending = true;
            Login(id, std::move(name), std::move(password));
        }
    } else {
        Fail(id, ErrBadRequest);
    }
}

LoginServer::Account LoginServer::Lookup(const std::string& name) {
    Account account;
    find_.Bind(1, name);
    if (find_.Step()) {
        account.found = true;
        account.id = static_cast<uint32_t>(find_.Int(0));
        account.name = find_.Text(1);
        account.salt = find_.Blob(2);
        account.hash = find_.Blob(3);
        account.iterations = static_cast<uint32_t>(find_.Int(4));
    }
    find_.Reset();
    return account;
}

void LoginServer::Login(FrameServer::ConnId id, std::string name, std::string password) {
    const bool queued = store_->Push([this, id, name = std::move(name), password = std::move(password)]() mutable {
        Account account = Lookup(name);
        const bool hashing = workers_->Push([this, id, account = std::move(account), password = std::move(password)] {
            // An unknown name costs the same hash as a wrong password, so timing does not tell them apart.
            static const uint8_t kNoSalt[kSaltBytes] = {};
            const bool usable = account.found && account.salt.size() == kSaltBytes && account.hash.size() == kHashBytes && account.iterations > 0;
            uint8_t derived[kHashBytes];
            Pbkdf2(password.data(), password.size(), usable ? account.salt.data() : kNoSalt, kSaltBytes,
                   usable ? account.iterations : options_.iterations, derived, sizeof(derived));
            if (usable && ConstantTimeEqual(derived, account.hash.data(), kHashBytes)) {
                server_.Post([this, id, account] { ++counters_.logins; Succeed(id, account.id, account.name); });
            } else {
                PostFail(id, ErrCredentials);
            }
        });
        if (!hashing) PostFail(id, ErrBusy);
    });
    if (!queued) Fail(id, ErrBusy);
}

void LoginServer::Register(FrameServer::ConnId id, std::string name, std::string password) {
    const bool queued = store_->Push([this, id, name = std::move(name), password = std::move(password)]() mutable {
        // A taken name is refused before the hash is paid for; the UNIQUE constraint still decides below.
        if (Lookup(name).found) { PostFail(id, ErrNameTaken); return; }
        const bool hashing = workers_->Push([this, id, name = std::move(name), password = std::move(password)] {
            std::vector<uint8_t> salt(kSaltBytes), hash(kHashBytes);
            if (!RandomBytes(salt.data(), salt.size())) { PostFail(id, ErrInternal); return; }
            Pbkdf2(password.data(), password.size(), salt.data(), salt.size(), options_.iterations, hash.data(), hash.size());
            const bool storing = store_->Push([this, id, name, salt = std::move(salt), hash = std::move(hash)] {
                insert_.Bind(1, name);
                insert_.BindBlob(2, salt.data(), salt.size());
                insert_.BindBlob(3, hash.data(), hash.size());
                insert_.Bind(4, static_cast<int64_t>(options_.iterations));
                insert_.Bind(5, static_cast<int64_t>(UnixNow()));
                insert_.Step();
                const bool inserted = insert_.Done();
                insert_.Reset();
                if (!inserted) { PostFail(id, ErrNameTaken); return; }
                const uint32_t account = static_cast<uint32_t>(db_.LastInsertId());
                server_.Post([this, id, account, name] { ++counters_.registrations; Succeed(id, account, name); });
            });
            if (!storing) PostFail(id, ErrBusy);
        });
        if (!hashing) PostFail(id, ErrBusy);
    });
    if (!queued) Fail(id, ErrBusy);
}

void LoginServer::Guest(FrameServer::ConnId id) {
    const bool queued = store_->Push([this, id] {
        // The row first (its id is the account), then the name made from that id. Registered names
        // cannot start with "Guest", so the name is free.
        uint8_t random[8];
        RandomBytes(random, sizeof(random));
        db_.Exec("BEGIN");
        insertGuest_.Bind(1, "Guest#" + Hex(random, sizeof(random)));
        insertGuest_.Bind(2, static_cast<int64_t>(UnixNow()));
        insertGuest_.Step();
        const bool inserted = insertGuest_.Done();
        insertGuest_.Reset();
        if (!inserted) { db_.Exec("ROLLBACK"); PostFail(id, ErrInternal); return; }
        const uint32_t account = static_cast<uint32_t>(db_.LastInsertId());
        char text[32];
        std::snprintf(text, sizeof(text), "Guest%04u", account);
        const std::string name = text;
        nameGuest_.Bind(1, name);
        nameGuest_.Bind(2, static_cast<int64_t>(account));
        nameGuest_.Step();
        const bool named = nameGuest_.Done();
        nameGuest_.Reset();
        if (!named || !db_.Exec("COMMIT")) { db_.Exec("ROLLBACK"); PostFail(id, ErrInternal); return; }
        server_.Post([this, id, account, name] { ++counters_.guests; Succeed(id, account, name); });
    });
    if (!queued) Fail(id, ErrBusy);
}

}  // namespace rpg
