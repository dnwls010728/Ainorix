#pragma once
#include <cstdint>

namespace rpg {

// Message types and codes of the back end's links. Every link starts with Hello from the side
// that connected; a wrong version or role is answered with Error and a close.
//
// Field notation: u8/u16/u32 little endian, s1 = u8 length + bytes (UTF-8), which is
// string.pack("<s1") in the game's Lua. "json" = the engine's bounded binary Json (link.encode).
constexpr uint16_t kProtocolVersion = 1;

enum Role : uint8_t {
    RoleClient = 1,  // a player's game
    RoleGame = 2,    // a game server (oe serve-game)
};

enum Msg : uint16_t {
    MsgHello = 1,  // u16 version, u8 role
    MsgError = 2,  // u16 code (Err)

    // Link 1: client -> login server. One request, one answer, then the server closes.
    MsgLogin = 10,        // s1 name, s1 password
    MsgRegister = 11,     // s1 name, s1 password
    MsgGuest = 12,        // (empty)
    MsgLoginResult = 13,  // u32 account, s1 name, s1 session ticket, s1 name proof

    // Link 2: client -> channel server, open while the player chooses a channel.
    MsgSession = 20,      // s1 session ticket, s1 name, s1 name proof  -> Channels (now and every 2 s)
    MsgChannels = 21,     // u8 count, then per channel: u8 channel, u16 players, u16 capacity, u8 online
    MsgEnter = 22,        // u8 channel (0 = the emptiest that has room)  -> EnterResult, or Error (the link stays open)
    MsgEnterResult = 23,  // s1 address, u16 port, s1 entry ticket, u8 channel, s1 map; then the server closes
    MsgPing = 24,         // (empty) at least every 10 s while choosing: a silent client is closed after 30 s

    // Link 4: game server -> channel server, for as long as the game server runs.
    // Its Hello is: u16 version, u8 role, s1 link key (the game key file's text).
    MsgGameRegister = 30, // u8 channel, s1 map, s1 address, u16 port, u16 capacity  -> Registered
    MsgGameRegistered = 31,  // u16 server id
    MsgHeartbeat = 32,    // u16 players; every second (5 s of silence = offline)
    MsgAdmit = 33,        // u32 player (the game server's id for it), s1 entry ticket  -> AdmitResult
    MsgAdmitResult = 34,  // u32 player, u16 code (0 = admitted, else Err), u32 account, s1 name
    MsgLeft = 35,         // u32 account
};

enum Err : uint16_t {
    ErrVersion = 1,      // protocol version or role not accepted
    ErrBadRequest = 2,   // malformed or unexpected message
    ErrBusy = 3,         // the server's queues are full: try again
    ErrRateLimited = 4,  // too many attempts from this address
    ErrCredentials = 5,  // unknown name or wrong password (never says which)
    ErrNameTaken = 6,
    ErrBadName = 7,      // 2..16 characters, no spaces or control characters, not "Guest..."
    ErrBadPassword = 8,  // 8..128 bytes
    ErrInternal = 9,
    ErrChannelFull = 10,
    ErrChannelOffline = 11,  // no game server runs that channel (or none at all)
    ErrTicket = 12,          // missing, forged, expired, used or meant for another server
    ErrNotRegistered = 13,   // a game server asked before GameRegister
};

constexpr int kDefaultLoginPort = 7001;
constexpr int kDefaultChannelPort = 7002;      // clients
constexpr int kDefaultGameLinkPort = 7003;     // game servers (bind to an internal address)

}  // namespace rpg
