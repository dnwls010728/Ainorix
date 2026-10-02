#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace oe {

// Four independent message sequence spaces; reliable channels retransmit whole messages.
enum class NetChannel : uint8_t { ReliableOrdered, ReliableUnordered, Unreliable, Sequenced };
enum class PacketKind : uint8_t { Data, Ack };

// A 64-packet receipt window. Sequence zero means no receipt; sequences never wrap.
class AckWindow {
public:
    // Returns false for duplicates and sequences older than the window.
    bool Accept(uint64_t sequence);
    bool Contains(uint64_t sequence) const;
    uint64_t Highest() const { return highest_; }
    uint64_t Mask() const { return mask_; }
private:
    uint64_t highest_ = 0;
    uint64_t mask_ = 0;
};

// Protocol v1, little-endian, with an explicit 52-byte header (including blob length).
// ACK-only packets have sequence/fragment fields zero; message zero means packet ACK only.
struct NetPacket {
    PacketKind kind = PacketKind::Data;
    NetChannel channel = NetChannel::ReliableOrdered;
    uint64_t sequence = 0;
    uint64_t ack = 0;
    uint64_t ackMask = 0;
    uint64_t message = 0;
    uint16_t fragment = 0;
    uint16_t fragments = 0;
    uint32_t totalBytes = 0;
    std::vector<uint8_t> payload;
};

// Fixed MTU avoids platform fragmentation; larger messages split into at most 64 parts.
constexpr size_t kNetPacketBytes = 1176;  // reserve 24 bytes for the M4 connection-cookie envelope
constexpr size_t kNetHeaderBytes = 52;
constexpr size_t kNetFragmentBytes = kNetPacketBytes - kNetHeaderBytes;
constexpr size_t kNetMaxMessageBytes = 64 * 1024;
constexpr size_t kNetMaxFragments = 64;

// Validates exact fragment geometry, enum/version/reserved fields and ACK masks.
// Failure leaves output unchanged and never allocates based on an unchecked length.
bool EncodePacket(const NetPacket& packet, std::vector<uint8_t>& bytes);
bool DecodePacket(const uint8_t* bytes, size_t size, NetPacket& packet);
// True for either reliable channel; invalid enum values return false.
bool IsReliable(NetChannel channel);

}  // namespace oe
