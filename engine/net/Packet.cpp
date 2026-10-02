#include "net/Packet.h"

#include <algorithm>
#include <utility>

#include "net/Bytes.h"

namespace oe {
namespace {
constexpr uint32_t kPacketMagic = 0x504e454f;  // "OENP" in little-endian byte order
constexpr uint8_t kPacketVersion = 1;

bool Valid(const NetPacket& p) {
    if (static_cast<uint8_t>(p.channel) > 3 || static_cast<uint8_t>(p.kind) > 1) return false;
    if (p.ack == 0 ? p.ackMask != 0 : (p.ackMask & 1) == 0) return false;
    if (p.ack > 0 && p.ack < 64 && (p.ackMask >> p.ack) != 0) return false;
    if (p.kind == PacketKind::Ack) {
        return p.sequence == 0 && p.fragment == 0 && p.fragments == 0 && p.totalBytes == 0 &&
               p.payload.empty() && (p.message == 0 || IsReliable(p.channel));
    }
    if (p.sequence == 0 || p.sequence == UINT64_MAX || p.message == 0 || p.message == UINT64_MAX ||
        p.totalBytes > kNetMaxMessageBytes) return false;
    const size_t count = std::max<size_t>(1, (p.totalBytes + kNetFragmentBytes - 1) / kNetFragmentBytes);
    if (p.fragments != count || p.fragments > kNetMaxFragments || p.fragment >= p.fragments) return false;
    const size_t offset = static_cast<size_t>(p.fragment) * kNetFragmentBytes;
    return p.payload.size() == std::min(kNetFragmentBytes, static_cast<size_t>(p.totalBytes) - offset);
}
}

bool IsReliable(NetChannel channel) {
    return channel == NetChannel::ReliableOrdered || channel == NetChannel::ReliableUnordered;
}

bool AckWindow::Contains(uint64_t sequence) const {
    return sequence != 0 && sequence <= highest_ && highest_ - sequence < 64 &&
           (mask_ & (UINT64_C(1) << (highest_ - sequence))) != 0;
}
bool AckWindow::Accept(uint64_t sequence) {
    if (sequence == 0 || Contains(sequence)) return false;
    if (sequence > highest_) {
        const uint64_t distance = sequence - highest_;
        mask_ = distance >= 64 ? 1 : (mask_ << distance) | 1;
        highest_ = sequence;
        return true;
    }
    if (highest_ - sequence >= 64) return false;
    mask_ |= UINT64_C(1) << (highest_ - sequence);
    return true;
}

bool EncodePacket(const NetPacket& p, std::vector<uint8_t>& bytes) {
    if (!Valid(p)) return false;
    ByteWriter writer(kNetPacketBytes);
    writer.WriteU32(kPacketMagic);
    writer.WriteU8(kPacketVersion);
    writer.WriteU8(static_cast<uint8_t>(p.kind));
    writer.WriteU8(static_cast<uint8_t>(p.channel));
    writer.WriteU8(0);  // reserved, must be zero in v1
    writer.WriteU64(p.sequence);
    writer.WriteU64(p.ack);
    writer.WriteU64(p.ackMask);
    writer.WriteU64(p.message);
    writer.WriteU16(p.fragment);
    writer.WriteU16(p.fragments);
    writer.WriteU32(p.totalBytes);
    if (!writer.WriteBlob(p.payload.data(), p.payload.size())) return false;
    bytes = writer.Data();
    return true;
}

bool DecodePacket(const uint8_t* bytes, size_t size, NetPacket& packet) {
    if (size < kNetHeaderBytes || size > kNetPacketBytes || !bytes) return false;
    ByteReader reader(bytes, size);
    NetPacket p;
    uint32_t magic = 0;
    uint8_t version = 0, kind = 0, channel = 0, reserved = 0;
    if (!reader.ReadU32(magic) || magic != kPacketMagic || !reader.ReadU8(version) || version != kPacketVersion ||
        !reader.ReadU8(kind) || kind > 1 || !reader.ReadU8(channel) || channel > 3 ||
        !reader.ReadU8(reserved) || reserved != 0 || !reader.ReadU64(p.sequence) ||
        !reader.ReadU64(p.ack) || !reader.ReadU64(p.ackMask) || !reader.ReadU64(p.message) ||
        !reader.ReadU16(p.fragment) || !reader.ReadU16(p.fragments) || !reader.ReadU32(p.totalBytes) ||
        !reader.ReadBlob(p.payload, kNetFragmentBytes) || reader.Remaining() != 0) return false;
    p.kind = static_cast<PacketKind>(kind);
    p.channel = static_cast<NetChannel>(channel);
    if (!Valid(p)) return false;
    packet = std::move(p);
    return true;
}

}  // namespace oe
