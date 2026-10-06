#include "net/Link.h"

#include <utility>

namespace oe {

uint32_t LinkSet::Open(const NetAddress& address, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return 0u; };
    if (!ValidNetAddress(address)) return fail("link address must be a numeric IPv4 address with a port 1..65535");
    if (links_.size() >= kMaxLinks) {
        auto closed = links_.begin();
        while (closed != links_.end() && closed->second.state != "closed") ++closed;
        if (closed == links_.end()) return fail("at most 8 links can be open");
        links_.erase(closed);
    }
    if (!next_) return fail("link ids are exhausted");
    std::string reason;
    if (!wire_.Connect(next_, address, &reason)) return fail(reason.empty() ? "cannot open a TCP connection" : reason);
    Entry entry;
    entry.address = address;
    links_.emplace(next_, std::move(entry));
    return next_++;
}

void LinkSet::Fail(uint32_t id, Entry& entry, const std::string& reason) {
    if (entry.state == "closed") return;
    wire_.Disconnect(id);
    entry.state = "closed";
    entry.error = reason;
}

bool LinkSet::Send(uint32_t id, uint16_t type, const uint8_t* payload, size_t size, std::string* error) {
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    auto it = links_.find(id);
    if (it == links_.end()) return fail("unknown link");
    if (it->second.state == "closed") return fail("link is closed");
    if (size > kMaxPayloadBytes || (!payload && size)) return fail("link payload exceeds 65534 bytes");
    if (wire_.QueuedBytes(id) + size + 6 > kMaxQueuedBytes) {
        Fail(id, it->second, "send queue overflow");
        return fail("send queue overflow; the link was closed");
    }
    std::vector<uint8_t> frame;
    frame.reserve(size + 2);
    frame.push_back(static_cast<uint8_t>(type & 0xff));
    frame.push_back(static_cast<uint8_t>(type >> 8));
    if (size) frame.insert(frame.end(), payload, payload + size);
    if (!wire_.Send(id, frame.data(), frame.size())) {
        Fail(id, it->second, "send queue overflow");
        return fail("send queue overflow; the link was closed");
    }
    return true;
}

std::vector<LinkSet::Message> LinkSet::Take(uint32_t id) {
    std::vector<Message> out;
    auto it = links_.find(id);
    if (it != links_.end()) out.swap(it->second.inbox);
    return out;
}

std::string LinkSet::State(uint32_t id) const {
    auto it = links_.find(id);
    return it == links_.end() ? std::string() : it->second.state;
}

std::string LinkSet::Error(uint32_t id) const {
    auto it = links_.find(id);
    return it == links_.end() ? std::string() : it->second.error;
}

bool LinkSet::Close(uint32_t id) {
    auto it = links_.find(id);
    if (it == links_.end()) return false;
    if (it->second.state != "closed") wire_.Disconnect(id);
    links_.erase(it);
    return true;
}

void LinkSet::Clear() {
    for (auto& link : links_) if (link.second.state != "closed") wire_.Disconnect(link.first);
    links_.clear();
}

void LinkSet::Pump() {
    if (links_.empty()) return;
    std::vector<TransportEvent> events;
    wire_.Poll(++tick_, events);
    for (TransportEvent& event : events) {
        auto it = links_.find(event.peer);
        if (it == links_.end() || it->second.state == "closed") continue;  // closed here: its notice
        Entry& entry = it->second;
        if (event.type == TransportEvent::Type::Connected) entry.state = "open";
        else if (event.type == TransportEvent::Type::Disconnected) {
            entry.state = "closed";
            entry.error = event.error.empty() ? "closed by the other side" : event.error;
        } else if (event.bytes.size() < 2) Fail(event.peer, entry, "frame without a message type");
        else if (entry.inbox.size() >= kMaxInboxMessages) Fail(event.peer, entry, "unread messages exceed their limit");
        else {
            Message message;
            message.type = static_cast<uint16_t>(event.bytes[0] | (event.bytes[1] << 8));
            message.payload.assign(event.bytes.begin() + 2, event.bytes.end());
            entry.inbox.push_back(std::move(message));
        }
    }
}

Json LinkSet::Status() const {
    Json out = Json::MakeArray();
    for (const auto& link : links_) {
        Json item = Json::MakeObject();
        item["id"] = link.first;
        item["address"] = link.second.address.host;
        item["port"] = link.second.address.port;
        item["state"] = link.second.state;
        item["error"] = link.second.error;
        item["pending"] = static_cast<uint64_t>(link.second.inbox.size());
        item["queuedBytes"] = static_cast<uint64_t>(wire_.QueuedBytes(link.first));
        out.push(item);
    }
    return out;
}

}  // namespace oe
