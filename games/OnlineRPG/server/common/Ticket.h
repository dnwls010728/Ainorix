#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "common/Crypto.h"

namespace rpg {

// A signed statement the servers hand to a client, which another server checks with the shared
// key alone (no lookup, no call to the issuer). 30 bytes, sent as 40 base64url characters:
//
//   u8 kind   u32 account   u16 server   u32 expires (unix seconds)   u32 nonce
//   u8[15] HMAC-SHA256(key, the 15 bytes before) truncated
//
// little endian. A session ticket (login -> channel server) has server 0; an entry ticket
// (channel -> game server) names the one game server it is good for and is used once.
struct Ticket {
    enum Kind : uint8_t { Session = 1, Entry = 2 };
    uint8_t kind = 0;
    uint32_t account = 0;
    uint16_t server = 0;
    uint32_t expires = 0;
    uint32_t nonce = 0;

    static constexpr size_t kBytes = 30;
    static constexpr size_t kChars = 40;
    static constexpr uint32_t kSessionSeconds = 12 * 60 * 60;
    static constexpr uint32_t kEntrySeconds = 30;
};

// 32 bytes shared by the login server, the channel server and nobody else.
using TicketKey = std::vector<uint8_t>;

// Reads the key file (64 hexadecimal digits). With `create`, a missing file is written with a
// fresh random key first, so the first server to start makes it.
bool LoadTicketKey(const std::string& path, bool create, TicketKey& key, std::string* error);

std::string SealTicket(const Ticket& ticket, const TicketKey& key);

// True when `text` is a ticket signed with `key`, of the wanted kind, not expired at `now`.
// Who may use it (server id, nonce not seen before) is the caller's check.
bool OpenTicket(const std::string& text, const TicketKey& key, Ticket::Kind kind, uint32_t now, Ticket& ticket);

// A ticket carries no name. The login server signs the account's name separately, so the channel
// server can believe the name a client presents with its session ticket: 20 base64url characters
// of HMAC-SHA256(key, 0x03, u32 account, name).
std::string NameProof(const TicketKey& key, uint32_t account, const std::string& name);
bool CheckNameProof(const TicketKey& key, uint32_t account, const std::string& name, const std::string& proof);

uint32_t UnixNow();

}  // namespace rpg
