#include "common/Ticket.h"

#include <cstdio>
#include <ctime>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace rpg {
namespace {
constexpr size_t kSigned = 15, kMac = 15;

void Put(uint8_t* out, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}
uint32_t Get(const uint8_t* in, size_t count) {
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= static_cast<uint32_t>(in[i]) << (8 * i);
    return value;
}
bool ReadKey(const std::string& path, TicketKey& key) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char text[128] = {};
    const size_t size = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    std::string hex(text, size);
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' ')) hex.pop_back();
    TicketKey bytes;
    if (!HexDecode(hex, bytes) || bytes.size() != 32) return false;
    key = std::move(bytes);
    return true;
}
}  // namespace

bool LoadTicketKey(const std::string& path, bool create, TicketKey& key, std::string* error) {
    if (ReadKey(path, key)) return true;
    std::FILE* existing = std::fopen(path.c_str(), "rb");
    if (existing) {
        std::fclose(existing);
        if (error) *error = "key file " + path + " must hold 64 hexadecimal digits";
        return false;
    }
    if (!create) {
        if (error) *error = "cannot read key file " + path;
        return false;
    }
    uint8_t bytes[32];
    if (!RandomBytes(bytes, sizeof(bytes))) {
        if (error) *error = "no system entropy for a new key";
        return false;
    }
    // Written aside and moved into place without replacing: when another server created the file
    // meanwhile, the move fails and its key is the one both read.
    const std::string temp = path + "." + Hex(bytes, 4) + ".tmp";
    std::FILE* file = std::fopen(temp.c_str(), "wb");
    if (file) {
        const std::string hex = Hex(bytes, sizeof(bytes)) + "\n";
        std::fwrite(hex.data(), 1, hex.size(), file);
        std::fclose(file);
#ifdef _WIN32
        std::rename(temp.c_str(), path.c_str());  // fails when the target exists
#else
        if (link(temp.c_str(), path.c_str()) != 0) { /* somebody else made it */ }
#endif
        std::remove(temp.c_str());
    }
    if (ReadKey(path, key)) return true;
    if (error) *error = "cannot write key file " + path;
    return false;
}

std::string SealTicket(const Ticket& ticket, const TicketKey& key) {
    uint8_t bytes[Ticket::kBytes];
    bytes[0] = ticket.kind;
    Put(bytes + 1, ticket.account, 4);
    Put(bytes + 5, ticket.server, 2);
    Put(bytes + 7, ticket.expires, 4);
    Put(bytes + 11, ticket.nonce, 4);
    const Digest mac = HmacKey(key.data(), key.size()).Mac(bytes, kSigned);
    for (size_t i = 0; i < kMac; ++i) bytes[kSigned + i] = mac[i];
    return Base64Url(bytes, sizeof(bytes));
}

bool OpenTicket(const std::string& text, const TicketKey& key, Ticket::Kind kind, uint32_t now, Ticket& ticket) {
    if (text.size() != Ticket::kChars) return false;
    std::vector<uint8_t> bytes;
    if (!Base64UrlDecode(text, bytes) || bytes.size() != Ticket::kBytes) return false;
    const Digest mac = HmacKey(key.data(), key.size()).Mac(bytes.data(), kSigned);
    if (!ConstantTimeEqual(mac.data(), bytes.data() + kSigned, kMac)) return false;
    Ticket parsed;
    parsed.kind = bytes[0];
    parsed.account = Get(bytes.data() + 1, 4);
    parsed.server = static_cast<uint16_t>(Get(bytes.data() + 5, 2));
    parsed.expires = Get(bytes.data() + 7, 4);
    parsed.nonce = Get(bytes.data() + 11, 4);
    if (parsed.kind != kind || parsed.expires < now) return false;
    ticket = parsed;
    return true;
}

std::string NameProof(const TicketKey& key, uint32_t account, const std::string& name) {
    std::vector<uint8_t> message(5 + name.size());
    message[0] = 3;  // not a ticket kind: a proof can never be read as a ticket's signed part
    Put(message.data() + 1, account, 4);
    for (size_t i = 0; i < name.size(); ++i) message[5 + i] = static_cast<uint8_t>(name[i]);
    const Digest mac = HmacKey(key.data(), key.size()).Mac(message.data(), message.size());
    return Base64Url(mac.data(), kMac);
}

bool CheckNameProof(const TicketKey& key, uint32_t account, const std::string& name, const std::string& proof) {
    const std::string expected = NameProof(key, account, name);
    return proof.size() == expected.size() && ConstantTimeEqual(proof.data(), expected.data(), expected.size());
}

uint32_t UnixNow() { return static_cast<uint32_t>(std::time(nullptr)); }

}  // namespace rpg
