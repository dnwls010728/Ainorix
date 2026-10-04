// Engine self tests. Run: build/bin/oe_tests  (exit code 0 = all passed)
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

#include "api/HttpServer.h"
#include "app/AndroidPackage.h"
#include "app/Engine.h"
#include "app/Project.h"
#include "assets/Assets.h"
#include "audio/Wav.h"
#include "core/FileSystem.h"
#include "core/Image.h"
#include "core/Json.h"
#include "core/Log.h"
#include "core/Zip.h"
#include "editor/EditorMath.h"
#include "net/Bytes.h"
#include "net/Channels.h"
#include "net/FallbackTransport.h"
#include "net/JsonCodec.h"
#include "net/LoopbackTransport.h"
#include "net/SocketTransports.h"
#include "net/WebSocketServer.h"
#include "platform/Platform.h"
#include "platform/GamepadInput.h"
#include "platform/Process.h"
#include "platform/TouchInput.h"
#include "render/Font.h"
#include "render/GpuRenderer.h"
#include "render/RenderScene.h"
#include "render/ShaderGraph.h"
#include "render/UI.h"
#include "physics/Physics2D.h"
#include "scene/Components.h"
#include "scene/TileGrid.h"
#include "script/ScriptHost.h"
#if OE_TEAM
#include "team/TeamCommands.h"
#include "team/TeamStore.h"
#endif
#if OE_NATIVE_EDITOR
#include "editor/Editor.h"
#include "editor/EditorText.h"
#include "imgui_internal.h"
#endif

using namespace oe;

namespace {

// Android test binaries cannot read the build host's source directory.
// Set OE_TEST_SOURCE_DIR to the staged fixtures; other platforms use the source tree.
std::string TestSourceDir() {
    const char* dir = std::getenv("OE_TEST_SOURCE_DIR");
    return dir && *dir ? dir : OE_SOURCE_DIR;
}

int g_failures = 0;
std::string g_current;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                  \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& Tests() {
    static std::vector<TestCase> t;
    return t;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { Tests().push_back({name, std::move(fn)}); }
};

#define TEST(name)                                   \
    static void name();                              \
    static Registrar reg_##name(#name, &name);       \
    static void name()

TEST(JsonRoundTrip) {
    std::string err;
    Json j = Json::parse(R"J({"a": 1, "b": [true, null, 2.5, "x\"y"], "c": {"d": "\u00e9\ud83d\ude00"}, "e": -3e2,})J", &err);
    CHECK(err.empty());
    CHECK(j["a"].asInt() == 1);
    CHECK(j["b"].size() == 4);
    CHECK(j["b"][3].asString() == "x\"y");
    CHECK(j["c"]["d"].asString() == "\xc3\xa9\xf0\x9f\x98\x80");
    CHECK(j["e"].asNumber() == -300);
    Json again = Json::parse(j.dump(2), &err);
    CHECK(err.empty());
    CHECK(again == j);
    Json bad = Json::parse("{\"a\": }", &err);
    CHECK(!err.empty());
    CHECK(err.find("line 1") != std::string::npos);
}

TEST(NetworkBytesBoundsAndEndian) {
    ByteWriter writer(64);
    CHECK(writer.WriteU8(0x12));
    CHECK(writer.WriteU16(0x3456));
    CHECK(writer.WriteU32(0x789abcde));
    CHECK(writer.WriteU64(UINT64_C(0xfedcba9876543210)));
    const uint8_t payload[] = {1, 0, 255};
    CHECK(writer.WriteBlob(payload, sizeof(payload)));
    CHECK(writer.WriteBlob(nullptr, 0));
    CHECK(writer.Data()[1] == 0x56 && writer.Data()[2] == 0x34);
    CHECK(writer.Data()[3] == 0xde && writer.Data()[14] == 0xfe);
    ByteReader reader(writer.Data().data(), writer.Data().size());
    uint8_t u8 = 0;
    uint16_t u16 = 0;
    uint32_t u32 = 0;
    uint64_t u64 = 0;
    std::vector<uint8_t> blob;
    CHECK(reader.ReadU8(u8) && u8 == 0x12);
    CHECK(reader.ReadU16(u16) && u16 == 0x3456);
    CHECK(reader.ReadU32(u32) && u32 == 0x789abcde);
    CHECK(reader.ReadU64(u64) && u64 == UINT64_C(0xfedcba9876543210));
    CHECK(reader.ReadBlob(blob, 3) && blob == std::vector<uint8_t>(payload, payload + 3));
    CHECK(reader.ReadBlob(blob, 0) && blob.empty());
    CHECK(reader.Ok() && reader.Remaining() == 0);
    u8 = 77;
    CHECK(!reader.ReadU8(u8) && u8 == 77 && !reader.Ok());

    // Every truncated scalar leaves the caller's value and cursor untouched.
    for (size_t size = 0; size < 8; ++size) {
        ByteReader shortReader(writer.Data().data(), size);
        u64 = 99;
        CHECK(!shortReader.ReadU64(u64) && u64 == 99 && shortReader.Remaining() == size);
    }
    const uint8_t oversized[] = {255, 255, 255, 255};
    ByteReader large(oversized, 4);
    blob = {42};
    CHECK(!large.ReadBlob(blob, 64) && blob == std::vector<uint8_t>{42} && large.Remaining() == 4);
    const uint8_t truncated[] = {3, 0, 0, 0, 1, 2};
    ByteReader shortBlob(truncated, sizeof(truncated));
    CHECK(!shortBlob.ReadBlob(blob, 3) && blob[0] == 42 && shortBlob.Remaining() == sizeof(truncated));
    ByteReader nullReader(nullptr, 1);
    CHECK(!nullReader.Ok() && !nullReader.ReadU8(u8));
    ByteReader emptyReader(nullptr, 0);
    CHECK(emptyReader.Ok() && !emptyReader.ReadU8(u8));
    ByteWriter small(5);
    CHECK(small.WriteU8(9));
    CHECK(!small.WriteBlob(payload, 3) && small.Data() == std::vector<uint8_t>{9});
    CHECK(!small.WriteU8(1));
    ByteWriter nullWriter(8);
    CHECK(!nullWriter.WriteBlob(nullptr, 1) && nullWriter.Data().empty());
    ByteWriter zeroWriter(0);
    CHECK(!zeroWriter.WriteU64(1) && zeroWriter.Data().empty());
    ByteWriter exact(7);
    CHECK(exact.WriteBlob(payload, 3) && exact.Data().size() == 7);
    ByteReader limited(exact.Data().data(), exact.Data().size());
    CHECK(!limited.ReadBlob(blob, 2) && limited.Remaining() == 7);

    // Deterministic malformed corpus: arbitrary prefixes and lengths never escape the limit.
    uint32_t seed = 123;
    for (size_t i = 0; i < 2000; ++i) {
        std::vector<uint8_t> bytes(i % 65);
        for (auto& byte : bytes) { seed = seed * 1664525u + 1013904223u; byte = static_cast<uint8_t>(seed >> 24); }
        ByteReader fuzz(bytes.data(), bytes.size());
        blob = {42};
        bool ok = fuzz.ReadBlob(blob, 32);
        CHECK(blob.size() <= 32 && fuzz.Remaining() <= bytes.size());
        if (!ok) CHECK(blob == std::vector<uint8_t>{42} && fuzz.Remaining() == bytes.size());
    }
}

TEST(NetworkLoopbackDeliveryAndLimits) {
    LoopbackConfig config;
    config.latencyFrames = 3;
    auto wire = std::make_shared<LoopbackNetwork>(config);
    LoopbackTransport sender(wire, 1), receiver(wire, 2);
    ITransport& transport = sender;
    uint8_t byte = 7;
    CHECK(transport.Send(2, &byte, 1));
    byte = 9;  // Send owns a copy, never the caller's memory.
    std::vector<TransportEvent> events;
    CHECK(receiver.Poll(2, events) && events.empty());
    CHECK(receiver.Poll(3, events) && events.size() == 1);
    CHECK(events[0].peer == 1 && events[0].bytes == std::vector<uint8_t>{7});
    CHECK(!receiver.Poll(2, events) && events.size() == 1);
    CHECK(wire->PendingMessages() == 0 && wire->PendingBytes() == 0);
    CHECK(!sender.Send(99, &byte, 1));
    CHECK(!sender.Send(2, nullptr, 1));
    CHECK(!sender.Send(2, &byte, ITransport::kMaxMessageBytes + 1));
    CHECK(wire->DroppedMessages() == 3);

    for (size_t i = 0; i < LoopbackNetwork::kMaxQueuedMessages; ++i) CHECK(sender.Send(2, nullptr, 0));
    CHECK(!sender.Send(2, nullptr, 0));
    CHECK(wire->PendingMessages() == LoopbackNetwork::kMaxQueuedMessages);
    events.clear();
    CHECK(receiver.Poll(4, events) && events.size() == LoopbackNetwork::kMaxQueuedMessages);
    std::vector<uint8_t> large(ITransport::kMaxMessageBytes, 1);
    for (size_t i = 0; i < LoopbackNetwork::kMaxQueuedBytes / large.size(); ++i)
        CHECK(sender.Send(2, large.data(), large.size()));
    CHECK(!sender.Send(2, &byte, 1));
    CHECK(wire->PendingBytes() == LoopbackNetwork::kMaxQueuedBytes);
    {
        LoopbackTransport temporary(wire, 3);
        CHECK(temporary.Send(2, nullptr, 0));
    }
    CHECK(wire->PendingMessages() == LoopbackNetwork::kMaxQueuedBytes / large.size());
    events.clear();
    CHECK(receiver.Poll(5, events));
    CHECK(wire->PendingBytes() == 0);
    {
        LoopbackTransport temporary(wire, 3);
        CHECK(sender.Send(3, &byte, 1));
        CHECK(temporary.Send(2, &byte, 1));
    }
    CHECK(wire->PendingMessages() == 0 && wire->PendingBytes() == 0);
    CHECK(sender.Poll(UINT64_MAX, events));
    CHECK(!sender.Send(2, &byte, 1) && wire->PendingMessages() == 0);

    bool rejected = false;
    try { LoopbackTransport duplicate(wire, 1); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    rejected = false;
    try { LoopbackTransport invalid(wire, 0); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    std::vector<std::unique_ptr<LoopbackTransport>> peers;
    for (PeerId id = 3; id <= LoopbackNetwork::kMaxPeers; ++id)
        peers.push_back(std::make_unique<LoopbackTransport>(wire, id));
    rejected = false;
    try { LoopbackTransport extra(wire, 65); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    config.lossPermille = 1001;
    rejected = false;
    try { LoopbackNetwork invalid(config); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

TEST(NetworkLoopbackSeededFaults) {
    auto run = [](uint32_t seed) {
        LoopbackConfig config;
        config.seed = seed;
        config.latencyFrames = 4;
        config.jitterFrames = 3;
        config.lossPermille = 250;
        config.duplicatePermille = 300;
        config.reorderFrames = 8;
        auto wire = std::make_shared<LoopbackNetwork>(config);
        LoopbackTransport a(wire, 1), b(wire, 2), c(wire, 3);
        std::vector<uint64_t> trace;
        for (uint64_t frame = 0; frame < 150; ++frame) {
            std::vector<TransportEvent> events;
            CHECK(a.Poll(frame, events));
            CHECK(b.Poll(frame, events));
            CHECK(c.Poll(frame, events));
            for (const auto& event : events) {
                CHECK(event.bytes.size() == 1);
                CHECK(frame >= static_cast<uint64_t>(event.bytes[0]) + 1);
                CHECK(frame <= static_cast<uint64_t>(event.bytes[0]) + 15);
                trace.push_back((frame << 32) | (static_cast<uint64_t>(event.peer) << 16) | event.bytes[0]);
            }
            if (frame < 100) {
                const uint8_t value = static_cast<uint8_t>(frame);
                CHECK(a.Send(2, &value, 1));
                CHECK(b.Send(3, &value, 1));
                CHECK(c.Send(1, &value, 1));
            }
        }
        CHECK(wire->PendingMessages() == 0 && wire->PendingBytes() == 0);
        CHECK(wire->DroppedMessages() > 0);
        return trace;
    };
    auto trace = run(42);
    CHECK(trace == run(42));
    CHECK(trace != run(43));
    std::set<uint32_t> seen;
    bool duplicate = false, reordered = false;
    std::map<PeerId, uint8_t> previous;
    for (uint64_t entry : trace) {
        const uint32_t message = static_cast<uint32_t>(entry);
        duplicate |= !seen.insert(message).second;
        const PeerId peer = (message >> 16) & 255;
        const uint8_t value = static_cast<uint8_t>(message);
        if (previous.count(peer)) reordered |= value < previous[peer];
        previous[peer] = value;
    }
    CHECK(duplicate && reordered);
    LoopbackConfig config;
    config.lossPermille = 1000;
    auto wire = std::make_shared<LoopbackNetwork>(config);
    LoopbackTransport a(wire, 1), b(wire, 2);
    CHECK(a.Send(2, nullptr, 0));
    std::vector<TransportEvent> events;
    CHECK(b.Poll(100, events) && events.empty() && wire->DroppedMessages() == 1);

    config.lossPermille = 0;
    config.duplicatePermille = 1000;
    auto duplicates = std::make_shared<LoopbackNetwork>(config);
    LoopbackTransport c(duplicates, 1), d(duplicates, 2);
    // Fewer than two payloads fit: reject without enqueuing either duplicate.
    std::vector<uint8_t> payload(65535, 1);
    for (int i = 0; i < 32; ++i) CHECK(c.Send(2, payload.data(), payload.size()));
    CHECK(duplicates->PendingBytes() == 4194240);
    CHECK(!c.Send(2, payload.data(), 33));
    CHECK(duplicates->PendingBytes() == 4194240 && duplicates->PendingMessages() == 64);
    CHECK(d.Poll(0, events) && events.size() == 64);
    CHECK(duplicates->PendingBytes() == 0);
}

TEST(NetworkPacketCodecAndAckWindow) {
    AckWindow window;
    CHECK(!window.Accept(0));
    CHECK(window.Accept(1) && window.Mask() == 1);
    CHECK(window.Accept(3) && window.Mask() == 5);
    CHECK(window.Accept(2) && window.Mask() == 7);
    CHECK(!window.Accept(2));
    CHECK(window.Accept(64) && window.Contains(1));
    CHECK(window.Accept(65) && !window.Contains(1) && window.Contains(2));
    CHECK(!window.Accept(1));
    CHECK(window.Accept(129) && window.Mask() == 1);
    CHECK(window.Accept(128) && window.Mask() == 3);
    CHECK(window.Accept(UINT64_MAX) && window.Mask() == 1);
    CHECK(!window.Accept(UINT64_MAX) && !window.Accept(UINT64_MAX - 64));

    NetPacket packet;
    packet.sequence = 1;
    packet.message = 2;
    packet.fragments = 1;
    packet.totalBytes = 3;
    packet.payload = {1, 2, 3};
    packet.ack = 3;
    packet.ackMask = 5;
    std::vector<uint8_t> bytes;
    CHECK(EncodePacket(packet, bytes) && bytes.size() == kNetHeaderBytes + 3);
    CHECK(bytes[0] == 'O' && bytes[1] == 'E' && bytes[2] == 'N' && bytes[3] == 'P');
    CHECK(bytes[4] == 1 && bytes[8] == 1 && bytes[16] == 3 && bytes[24] == 5 && bytes[32] == 2);
    NetPacket decoded;
    CHECK(DecodePacket(bytes.data(), bytes.size(), decoded));
    CHECK(decoded.sequence == 1 && decoded.message == 2 && decoded.payload == packet.payload && decoded.ackMask == 5);
    decoded.message = 99;
    for (size_t length = 0; length < bytes.size(); ++length) {
        CHECK(!DecodePacket(bytes.data(), length, decoded));
        CHECK(decoded.message == 99);
    }
    // Header fields must be validated before any reassembly allocation.
    for (size_t offset : {size_t(0), size_t(4), size_t(5), size_t(6), size_t(7), size_t(40), size_t(42), size_t(44), size_t(48)}) {
        auto corrupt = bytes;
        corrupt[offset] = 255;
        CHECK(!DecodePacket(corrupt.data(), corrupt.size(), decoded) && decoded.message == 99);
    }
    auto extra = bytes;
    extra.push_back(0);
    CHECK(!DecodePacket(extra.data(), extra.size(), decoded));
    CHECK(!DecodePacket(nullptr, kNetHeaderBytes, decoded));
    packet.ackMask = 8;
    auto unchanged = bytes;
    CHECK(!EncodePacket(packet, bytes) && bytes == unchanged);
    packet.ackMask = 1;
    packet.fragment = 1;
    CHECK(!EncodePacket(packet, bytes));

    uint32_t seed = 17;
    for (size_t trial = 0; trial < 3000; ++trial) {
        std::vector<uint8_t> noise(trial % 1250);
        for (uint8_t& byte : noise) { seed = seed * 1664525u + 1013904223u; byte = static_cast<uint8_t>(seed >> 24); }
        decoded.message = 99;
        const bool ok = DecodePacket(noise.data(), noise.size(), decoded);
        CHECK(ok ? decoded.payload.size() <= kNetFragmentBytes : decoded.message == 99);
    }
}

// Raw packet injection isolates channel order, replay and malformed-packet behavior.
std::vector<uint8_t> NetworkDataPacket(NetChannel channel, uint64_t message, uint64_t sequence,
                                      const std::vector<uint8_t>& payload, uint16_t fragment = 0,
                                      uint32_t total = 0) {
    NetPacket packet;
    packet.channel = channel;
    packet.message = message;
    packet.sequence = sequence;
    packet.totalBytes = total ? total : static_cast<uint32_t>(payload.size());
    packet.fragment = fragment;
    packet.fragments = static_cast<uint16_t>(std::max<size_t>(1, (packet.totalBytes + kNetFragmentBytes - 1) / kNetFragmentBytes));
    packet.payload = payload;
    std::vector<uint8_t> bytes;
    CHECK(EncodePacket(packet, bytes));
    return bytes;
}

TEST(NetworkChannelOrderingAndReplay) {
    auto wire = std::make_shared<LoopbackNetwork>();
    LoopbackTransport sender(wire, 1), receiver(wire, 2);
    ChannelEndpoint endpoint(receiver);
    CHECK(endpoint.AddPeer(1));
    CHECK(!endpoint.AddPeer(1) && !endpoint.AddPeer(0));
    uint64_t sequence = 1;
    auto send = [&](NetChannel channel, uint64_t message, uint8_t value) {
        auto bytes = NetworkDataPacket(channel, message, sequence++, {value});
        CHECK(sender.Send(2, bytes.data(), bytes.size()));
    };
    std::vector<ChannelEvent> events;
    send(NetChannel::ReliableOrdered, 2, 22);
    CHECK(endpoint.Poll(0, events) && events.empty() && endpoint.BufferedBytes(1) == 1);
    send(NetChannel::ReliableOrdered, 1, 11);
    CHECK(endpoint.Poll(1, events) && events.size() == 2);
    CHECK(events[0].sequence == 1 && events[1].sequence == 2);
    CHECK(events[0].bytes == std::vector<uint8_t>{11} && events[1].bytes == std::vector<uint8_t>{22});
    CHECK(endpoint.BufferedBytes(1) == 0);
    events.clear();
    send(NetChannel::ReliableUnordered, 2, 22);
    CHECK(endpoint.Poll(2, events) && events.size() == 1 && events[0].sequence == 2);
    send(NetChannel::ReliableUnordered, 1, 11);
    CHECK(endpoint.Poll(3, events) && events.size() == 2 && events[1].sequence == 1);
    events.clear();
    send(NetChannel::Sequenced, 2, 22);
    send(NetChannel::Sequenced, 1, 11);
    CHECK(endpoint.Poll(4, events) && events.size() == 1 && events[0].sequence == 2);
    events.clear();
    send(NetChannel::Unreliable, 2, 22);
    send(NetChannel::Unreliable, 1, 11);
    CHECK(endpoint.Poll(5, events) && events.size() == 2 && events[0].sequence == 2 && events[1].sequence == 1);
    events.clear();
    send(NetChannel::ReliableOrdered, 1, 11);
    send(NetChannel::ReliableUnordered, 2, 22);
    send(NetChannel::Unreliable, 2, 22);
    CHECK(endpoint.Poll(6, events) && events.empty());
    CHECK(endpoint.Stats(1)->duplicatePackets == 3);
    CHECK(!endpoint.Poll(5, events) && endpoint.Poll(6, events) && events.empty());
    CHECK(endpoint.RemovePeer(1) && !endpoint.RemovePeer(1));
    CHECK(endpoint.Stats(1) == nullptr && endpoint.BufferedBytes(1) == 0);
}

TEST(NetworkChannelFragmentValidationAndExpiry) {
    auto wire = std::make_shared<LoopbackNetwork>();
    LoopbackTransport sender(wire, 1), receiver(wire, 2), stranger(wire, 3);
    ChannelConfig config;
    config.assemblyFrames = 3;
    ChannelEndpoint endpoint(receiver, config);
    CHECK(endpoint.AddPeer(1));
    std::vector<uint8_t> body(kNetFragmentBytes, 7);
    auto partial = NetworkDataPacket(NetChannel::Unreliable, 1, 1, body, 0,
                                     static_cast<uint32_t>(kNetFragmentBytes + 1));
    CHECK(sender.Send(2, partial.data(), partial.size()));
    std::vector<ChannelEvent> events;
    CHECK(endpoint.Poll(0, events) && events.empty());
    CHECK(endpoint.BufferedBytes(1) == kNetFragmentBytes + 1);
    // Conflicting total size for an existing assembly is rejected without touching its payload.
    auto conflict = NetworkDataPacket(NetChannel::Unreliable, 1, 2, body, 0,
                                      static_cast<uint32_t>(kNetFragmentBytes + 2));
    CHECK(sender.Send(2, conflict.data(), conflict.size()));
    CHECK(endpoint.Poll(1, events) && endpoint.RejectedPackets() == 1);
    CHECK(endpoint.BufferedBytes(1) == kNetFragmentBytes + 1);
    CHECK(endpoint.Poll(3, events) && endpoint.BufferedBytes(1) == 0);
    CHECK(endpoint.Stats(1)->droppedPackets == 1);
    auto bytes = NetworkDataPacket(NetChannel::Unreliable, 2, 3, {8});
    CHECK(stranger.Send(2, bytes.data(), bytes.size()));
    CHECK(sender.Send(2, nullptr, 0));
    CHECK(endpoint.Poll(4, events) && events.empty() && endpoint.RejectedPackets() == 3);
    // Incomplete assemblies consume slots independently of the byte budget.
    for (uint64_t message = 3; message < 3 + ChannelEndpoint::kMaxAssemblies; ++message) {
        auto fragment = NetworkDataPacket(NetChannel::Unreliable, message, message + 1, body, 0,
                                         static_cast<uint32_t>(kNetFragmentBytes + 1));
        CHECK(sender.Send(2, fragment.data(), fragment.size()));
    }
    auto overflow = NetworkDataPacket(NetChannel::Unreliable, 100, 100, body, 0,
                                     static_cast<uint32_t>(kNetFragmentBytes + 1));
    CHECK(sender.Send(2, overflow.data(), overflow.size()));
    CHECK(endpoint.Poll(5, events));
    CHECK(endpoint.BufferedBytes(1) == ChannelEndpoint::kMaxAssemblies * (kNetFragmentBytes + 1));
    CHECK(endpoint.Poll(8, events) && endpoint.BufferedBytes(1) == 0);
    for (int i = 0; i < 200; ++i) CHECK(sender.Send(2, bytes.data(), bytes.size()));
    CHECK(endpoint.Poll(9, events));
    CHECK(endpoint.RejectedPackets() == 3 + 200 - ChannelEndpoint::kIncomingPerFrame);
    events.clear();
    auto first = NetworkDataPacket(NetChannel::Unreliable, 200, 200, body, 0,
                                   static_cast<uint32_t>(kNetFragmentBytes + 1));
    auto last = NetworkDataPacket(NetChannel::Unreliable, 200, 201, {9}, 1,
                                  static_cast<uint32_t>(kNetFragmentBytes + 1));
    CHECK(sender.Send(2, first.data(), first.size()));
    CHECK(endpoint.Poll(10, events) && events.empty());
    CHECK(sender.Send(2, last.data(), last.size()));
    // Expiry precedes incoming fragments even when both occur on the same poll.
    CHECK(endpoint.Poll(13, events) && events.empty() && endpoint.BufferedBytes(1) == kNetFragmentBytes + 1);
    CHECK(endpoint.Poll(16, events) && endpoint.BufferedBytes(1) == 0);
}

TEST(NetworkChannelsReliableUnderLoss) {
    auto run = [](uint32_t seed) {
        LoopbackConfig faults;
        faults.seed = seed;
        faults.latencyFrames = 3;
        faults.jitterFrames = 2;
        faults.lossPermille = 250;
        faults.duplicatePermille = 200;
        faults.reorderFrames = 6;
        auto wire = std::make_shared<LoopbackNetwork>(faults);
        LoopbackTransport ta(wire, 1), tb(wire, 2);
        ChannelConfig config;
        config.timeoutFrames = 6000;
        ChannelEndpoint a(ta, config), b(tb, config);
        CHECK(a.AddPeer(2) && b.AddPeer(1));
        std::map<std::pair<NetChannel, uint64_t>, std::vector<uint8_t>> expected;
        std::map<std::pair<NetChannel, uint64_t>, std::vector<uint8_t>> received;
        for (NetChannel channel : {NetChannel::ReliableOrdered, NetChannel::ReliableUnordered}) {
            for (uint64_t message = 1; message <= 24; ++message) {
                size_t size = message == 24 ? kNetMaxMessageBytes : static_cast<size_t>(message * 153);
                std::vector<uint8_t> payload(size, static_cast<uint8_t>(message));
                if (message == 1) payload.clear();
                expected[{channel, message}] = payload;
                CHECK(a.Send(2, channel, payload.data(), payload.size()));
            }
        }
        const uint8_t reply[] = {9, 8, 7};
        CHECK(b.Send(1, NetChannel::ReliableOrdered, reply, sizeof(reply)));
        std::vector<uint64_t> trace;
        uint64_t ordered = 1;
        size_t replyCount = 0;
        for (uint64_t frame = 0; frame < 6000; ++frame) {
            std::vector<ChannelEvent> ea, eb;
            CHECK(a.Poll(frame, ea) && b.Poll(frame, eb));
            for (const auto& event : ea) {
                CHECK(event.type == ChannelEvent::Type::Message && event.bytes == std::vector<uint8_t>(reply, reply + 3));
                ++replyCount;
            }
            for (const auto& event : eb) {
                CHECK(event.type == ChannelEvent::Type::Message);
                const auto key = std::make_pair(event.channel, event.sequence);
                CHECK(received.emplace(key, event.bytes).second);
                if (event.channel == NetChannel::ReliableOrdered) CHECK(event.sequence == ordered++);
                trace.push_back((frame << 16) | (static_cast<uint64_t>(event.channel) << 8) | event.sequence);
            }
            if (received.size() == expected.size() && replyCount == 1 && a.PendingMessages(2) == 0 && b.PendingMessages(1) == 0)
                break;
        }
        CHECK(received == expected && replyCount == 1);
        CHECK(a.PendingMessages(2) == 0 && b.PendingMessages(1) == 0);
        CHECK(a.BufferedBytes(2) == 0 && b.BufferedBytes(1) == 0);
        CHECK(a.Stats(2)->retransmittedPackets > 0 && a.Stats(2)->lostPackets > 0);
        CHECK(a.Stats(2)->rttFrames > 0 && a.Stats(2)->jitterFrames > 0);
        CHECK(a.Stats(2)->LossPermille() > 0 && a.Stats(2)->LossPermille() < 1000);
        CHECK(a.Stats(2)->invalidPackets == 0 && b.Stats(1)->invalidPackets == 0);
        return trace;
    };
    CHECK(run(45) == run(45));
    CHECK(run(46) != run(45));
}

TEST(NetworkChannelQueueBudgetAndTimeout) {
    LoopbackConfig faults;
    faults.lossPermille = 1000;
    auto wire = std::make_shared<LoopbackNetwork>(faults);
    LoopbackTransport ta(wire, 1), tb(wire, 2);
    ChannelConfig config;
    config.retryFrames = 2;
    config.timeoutFrames = 6;
    ChannelEndpoint a(ta, config);
    CHECK(a.AddPeer(2));
    const uint8_t byte = 1;
    CHECK(!a.Send(3, NetChannel::Unreliable, &byte, 1));
    CHECK(!a.Send(2, static_cast<NetChannel>(4), &byte, 1));
    CHECK(!a.Send(2, NetChannel::Unreliable, nullptr, 1));
    CHECK(!a.Send(2, NetChannel::Unreliable, &byte, kNetMaxMessageBytes + 1));
    for (size_t i = 0; i < ChannelEndpoint::kMaxQueuedMessages; ++i)
        CHECK(a.Send(2, NetChannel::ReliableOrdered, &byte, 1));
    CHECK(!a.Send(2, NetChannel::ReliableOrdered, &byte, 1));
    std::vector<ChannelEvent> events;
    CHECK(a.Poll(0, events) && a.Stats(2)->packetsSent == ChannelEndpoint::kPacketsPerFrame);
    CHECK(a.Poll(0, events) && a.Stats(2)->packetsSent == ChannelEndpoint::kPacketsPerFrame);
    for (uint64_t frame = 1; frame <= 6; ++frame) CHECK(a.Poll(frame, events));
    CHECK(events.size() == 1 && events[0].type == ChannelEvent::Type::TimedOut && events[0].peer == 2);
    CHECK(a.PendingMessages(2) == 0 && a.BufferedBytes(2) == 0);
    CHECK(!a.Send(2, NetChannel::ReliableOrdered, &byte, 1));
    CHECK(a.Poll(7, events) && events.size() == 1);
    CHECK(a.Stats(2)->LossPermille() == 1000);
    CHECK(a.RemovePeer(2) && a.AddPeer(2));
    for (PeerId id = 3; id <= 65; ++id) CHECK(a.AddPeer(id));
    CHECK(!a.AddPeer(66));
}

// Target individual protocol failures independently of the seeded random wire.
class NetworkFaultTransport final : public ITransport {
public:
    explicit NetworkFaultTransport(ITransport& transport) : transport_(transport) {}
    bool blocked = false;
    std::function<bool(const NetPacket&)> drop;
    uint64_t accepted = 0;
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override {
        if (blocked) return false;
        NetPacket packet;
        CHECK(DecodePacket(bytes, size, packet));
        CHECK(size <= kNetPacketBytes);
        ++accepted;
        if (drop && drop(packet)) return true;
        return transport_.Send(peer, bytes, size);
    }
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override { return transport_.Poll(frame, events); }
private:
    ITransport& transport_;
};

TEST(NetworkChannelsAckLossAndAssemblyRecovery) {
    auto wire = std::make_shared<LoopbackNetwork>();
    LoopbackTransport ta(wire, 1), tb(wire, 2);
    NetworkFaultTransport fa(ta), fb(tb);
    bool fragmentDropped = false, ackDropped = false;
    fa.drop = [&](const NetPacket& packet) {
        if (packet.kind == PacketKind::Data && packet.message == 1 && packet.fragment == 1 && !fragmentDropped) {
            fragmentDropped = true;
            return true;
        }
        return false;
    };
    fb.drop = [&](const NetPacket& packet) {
        if (packet.kind == PacketKind::Ack && packet.message == 1 && !ackDropped) {
            ackDropped = true;
            return true;
        }
        return false;
    };
    ChannelConfig config;
    config.retryFrames = 6;
    config.assemblyFrames = 2;
    ChannelEndpoint a(fa, config), b(fb, config);
    CHECK(a.AddPeer(2) && b.AddPeer(1));
    std::vector<uint8_t> body(kNetFragmentBytes + 7, 123);
    const uint8_t next = 2;
    CHECK(a.Send(2, NetChannel::ReliableOrdered, body.data(), body.size()));
    for (int i = 0; i < 63; ++i) CHECK(a.Send(2, NetChannel::ReliableOrdered, &next, 1));
    std::vector<ChannelEvent> received;
    for (uint64_t frame = 0; frame < 40; ++frame) {
        std::vector<ChannelEvent> ea, eb;
        CHECK(a.Poll(frame, ea) && b.Poll(frame, eb));
        CHECK(ea.empty());
        received.insert(received.end(), eb.begin(), eb.end());
        // The first reliable window is complete except for its oldest message. ACKed ordered
        // payloads must survive expiry; the second window must not overtake the unacknowledged gap.
        if (frame == 3) CHECK(b.BufferedBytes(1) == 31 && received.empty());
    }
    CHECK(fragmentDropped && ackDropped);
    CHECK(received.size() == 64);
    if (received.size() == 64) {
        CHECK(received[0].sequence == 1 && received[0].bytes == body);
        for (size_t i = 1; i < received.size(); ++i)
            CHECK(received[i].sequence == i + 1 && received[i].bytes == std::vector<uint8_t>{2});
    }
    CHECK(a.PendingMessages(2) == 0 && b.BufferedBytes(1) == 0);
    CHECK(a.Stats(2)->retransmittedPackets >= 4 && b.Stats(1)->messagesReceived == 64);
    CHECK(b.Stats(1)->duplicatePackets >= 2);
}

TEST(NetworkChannelsUnreliableAndBackpressure) {
    auto wire = std::make_shared<LoopbackNetwork>();
    LoopbackTransport ta(wire, 1), tb(wire, 2);
    NetworkFaultTransport fa(ta);
    fa.blocked = true;
    ChannelEndpoint a(fa), b(tb);
    CHECK(a.AddPeer(2) && b.AddPeer(1));
    const uint8_t byte = 7;
    CHECK(a.Send(2, NetChannel::Unreliable, &byte, 1));
    std::vector<ChannelEvent> events;
    CHECK(a.Poll(0, events) && a.PendingMessages(2) == 1 && a.Stats(2)->packetsSent == 0);
    CHECK(b.Poll(0, events) && events.empty());
    fa.blocked = false;
    CHECK(a.Poll(1, events) && a.PendingMessages(2) == 0);
    CHECK(b.Poll(1, events) && events.size() == 1 && events[0].sequence == 1);
    events.clear();
    fa.drop = [](const NetPacket& packet) { return packet.kind == PacketKind::Data; };
    CHECK(a.Send(2, NetChannel::Unreliable, &byte, 1));
    CHECK(a.Send(2, NetChannel::Sequenced, &byte, 1));
    for (uint64_t frame = 2; frame < 70; ++frame) CHECK(a.Poll(frame, events) && b.Poll(frame, events));
    CHECK(events.empty() && a.PendingMessages(2) == 0);
    CHECK(a.Stats(2)->packetsSent == 3 && a.Stats(2)->retransmittedPackets == 0);
    CHECK(a.Stats(2)->ackedPackets == 1 && a.Stats(2)->lostPackets == 2);
    CHECK(a.Stats(2)->LossPermille() == 666);

    // Sequenced completion discards an older partial message's reserved storage.
    fa.drop = {};
    std::vector<uint8_t> first(kNetFragmentBytes, 1);
    auto old = NetworkDataPacket(NetChannel::Sequenced, 2, 100, first, 0,
                                 static_cast<uint32_t>(kNetFragmentBytes + 1));
    auto newest = NetworkDataPacket(NetChannel::Sequenced, 3, 101, {3});
    CHECK(ta.Send(2, old.data(), old.size()));
    CHECK(ta.Send(2, newest.data(), newest.size()));
    CHECK(b.Poll(70, events) && events.size() == 1 && events[0].sequence == 3);
    CHECK(b.BufferedBytes(1) == 0);
    CHECK(a.RemovePeer(2) && a.AddPeer(2));
    std::vector<uint8_t> max(kNetMaxMessageBytes, 1);
    for (size_t i = 0; i < 64; ++i) CHECK(a.Send(2, NetChannel::Unreliable, max.data(), max.size()));
    CHECK(a.BufferedBytes(2) == ChannelEndpoint::kMaxBufferedBytes);
    CHECK(!a.Send(2, NetChannel::Unreliable, &byte, 1));
    CHECK(a.RemovePeer(2) && a.BufferedBytes(2) == 0);
    fa.blocked = true;
    ChannelConfig timing;
    timing.retryFrames = 2;
    timing.timeoutFrames = 6;
    ChannelEndpoint blocked(fa, timing);
    CHECK(blocked.AddPeer(2) && blocked.Poll(70, events));
    events.clear();
    CHECK(blocked.Send(2, NetChannel::ReliableOrdered, &byte, 1));
    for (uint64_t frame = 71; frame <= 76; ++frame) CHECK(blocked.Poll(frame, events));
    CHECK(events.size() == 1 && events[0].type == ChannelEvent::Type::TimedOut);
    CHECK(blocked.PendingMessages(2) == 0 && blocked.Stats(2)->packetsSent == 0);
}

TEST(NetworkTcpFramingAndBounds) {
    ByteWriter writer(100);
    const uint8_t body[] = {7, 0, 9};
    CHECK(writer.WriteBlob(body, 3) && writer.WriteBlob(nullptr, 0) && writer.WriteBlob(body, 3));
    for (size_t split = 0; split <= writer.Data().size(); ++split) {
        TcpFrameReader reader;
        std::vector<std::vector<uint8_t>> frames;
        CHECK(reader.Feed(writer.Data().data(), split, frames));
        CHECK(reader.Feed(writer.Data().data() + split, writer.Data().size() - split, frames));
        CHECK(frames.size() == 3 && reader.BufferedBytes() == 0);
        if (frames.size() == 3) CHECK(frames[0] == std::vector<uint8_t>(body, body + 3) && frames[1].empty() && frames[2] == frames[0]);
    }
    TcpFrameReader bytewise;
    std::vector<std::vector<uint8_t>> frames;
    for (uint8_t byte : writer.Data()) CHECK(bytewise.Feed(&byte, 1, frames));
    CHECK(frames.size() == 3);
    const uint8_t huge[] = {1, 0, 1, 0};  // 65537, one byte above the hard maximum
    TcpFrameReader invalid;
    frames.clear();
    CHECK(!invalid.Feed(huge, 4, frames) && invalid.BufferedBytes() == 0 && !invalid.Ok());
    CHECK(!invalid.Feed(body, 3, frames) && frames.empty());
    TcpFrameReader flood;
    std::vector<uint8_t> zeroFrames(4 * 129, 0);
    CHECK(!flood.Feed(zeroFrames.data(), zeroFrames.size(), frames) && frames.size() == 128);
    CHECK(ValidNetAddress({"127.0.0.1", 1}));
    CHECK(!ValidNetAddress({"localhost", 1}) && !ValidNetAddress({"127.00.0.1", 1}));
    CHECK(!ValidNetAddress({"256.0.0.1", 1}) && !ValidNetAddress({"127.0.0.1.", 1}));
    CHECK(!ValidNetAddress({"127.0.0.1", 0}) && ValidNetAddress({"127.0.0.1", 0}, true));
}

class NetworkDropTransport final : public ITransport {
public:
    explicit NetworkDropTransport(ITransport& transport) : transport_(transport) {}
    bool drop = false;
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override { return drop || transport_.Send(peer, bytes, size); }
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override { return transport_.Poll(frame, events); }
private:
    ITransport& transport_;
};

TEST(NetworkUdpTcpFallbackReachability) {
    LoopbackConfig faults;
    faults.latencyFrames = 2;
    faults.jitterFrames = 1;
    faults.duplicatePermille = 100;
    auto udp = std::make_shared<LoopbackNetwork>(faults), tcp = std::make_shared<LoopbackNetwork>();
    LoopbackTransport ua(udp, 1), ub(udp, 2), ta(tcp, 1), tb(tcp, 2);
    NetworkDropTransport da(ua), db(ub);
    FallbackTransport a(da, ta), b(db, tb);
    CHECK(a.AddPeer(2, 100) && b.AddPeer(1, 200));
    CHECK(!a.AddPeer(2, 101) && !a.AddPeer(3, 0));
    const uint8_t byte = 7;
    CHECK(a.Send(2, &byte, 1));
    size_t received = 0;
    auto advance = [&](uint64_t first, uint64_t last) {
        for (uint64_t frame = first; frame < last; ++frame) {
            std::vector<TransportEvent> ea, eb;
            CHECK(a.Poll(frame, ea) && b.Poll(frame, eb));
            CHECK(ea.empty());
            for (const auto& event : eb) {
                CHECK(event.bytes == std::vector<uint8_t>{7});
                ++received;
            }
        }
    };
    advance(0, 20);
    CHECK(received == 1 && a.Selected(2) == FallbackTransport::Route::Udp && b.Selected(1) == FallbackTransport::Route::Udp);
    CHECK(a.Send(2, &byte, 1));
    advance(20, 30);
    CHECK(received >= 2);  // UDP may duplicate; the channel layer owns logical duplicate rejection.
    da.drop = db.drop = true;
    advance(30, 100);
    CHECK(a.Selected(2) == FallbackTransport::Route::Tcp && b.Selected(1) == FallbackTransport::Route::Tcp);
    const size_t before = received;
    CHECK(a.Send(2, &byte, 1));
    advance(100, 103);
    CHECK(received == before + 1);
    std::vector<TransportEvent> events;
    CHECK(!a.Poll(99, events) && a.Poll(102, events));
}

#ifndef __EMSCRIPTEN__
TEST(NetworkNativeUdpSourcesAndTruncation) {
    const uint64_t live = PlatformNetSocketsLive();
    {
        UdpTransport a, b;
        std::string error;
        CHECK(a.Bind({}, &error) && b.Bind({}, &error));
        CHECK(a.LocalAddress().host == "127.0.0.1" && a.LocalAddress().port != 0);
        CHECK(a.AddPeer(2, b.LocalAddress()) && b.AddPeer(1, a.LocalAddress()));
        CHECK(!b.AddPeer(3, a.LocalAddress()) && !a.Bind({}, &error));
        const uint8_t byte = 7;
        CHECK(a.Send(2, &byte, 1) && a.Send(2, nullptr, 0));
        CHECK(!a.Send(2, &byte, 1201) && !a.Send(99, &byte, 1));
        auto stranger = CreateNetSocket(SocketKind::Udp, &error);
        CHECK(stranger != nullptr);
        if (!stranger) return;
        CHECK(stranger->Bind({}, &error));
        CHECK(stranger->SendTo(b.LocalAddress(), &byte, 1) == SocketIo::Progress);
        auto raw = CreateNetSocket(SocketKind::Udp, &error);
        CHECK(raw != nullptr);
        if (!raw) return;
        CHECK(raw->Bind({}, &error) && b.AddPeer(3, raw->LocalAddress()));
        std::vector<uint8_t> oversized(1500, 1);
        CHECK(raw->SendTo(b.LocalAddress(), oversized.data(), oversized.size()) == SocketIo::Progress);
        std::vector<TransportEvent> events;
        for (uint64_t frame = 0; frame < 100 && (events.size() < 2 || b.DroppedPackets() < 2); ++frame) {
            CHECK(b.Poll(frame, events));
            PlatformSleep(0.001);
        }
        CHECK(events.size() == 2 && b.DroppedPackets() == 2);
        if (events.size() == 2) CHECK(events[0].peer == 1 && events[0].bytes == std::vector<uint8_t>{7} && events[1].bytes.empty());
        CHECK(b.RemovePeer(1) && !b.RemovePeer(1));
    }
    CHECK(PlatformNetSocketsLive() == live);
}

TEST(NetworkNativeTcpMalformedAndClose) {
    TcpTransport server;
    std::string error;
    CHECK(server.Listen({}, &error));
    auto client = CreateNetSocket(SocketKind::Tcp, &error);
    CHECK(client != nullptr);
    if (!client) return;
    CHECK(client->Connect(server.LocalAddress(), &error));
    uint64_t frame = 0;
    std::vector<TransportEvent> events;
    for (; frame < 100 && events.empty(); ++frame) {
        CHECK(server.Poll(frame, events));
        client->State();
        PlatformSleep(0.001);
    }
    CHECK(events.size() == 1 && events[0].type == TransportEvent::Type::Connected && events[0].peer == 1);
    CHECK(server.PeerAddress(1).host == "127.0.0.1" && server.PeerAddress(1).port == client->LocalAddress().port);
    auto sendAll = [&](const uint8_t* bytes, size_t size) {
        size_t offset = 0;
        for (int attempt = 0; attempt < 100 && offset < size; ++attempt) {
            size_t sent = 0;
            SocketIo result = client->Send(bytes + offset, size - offset, sent);
            CHECK(result == SocketIo::Progress || result == SocketIo::WouldBlock);
            offset += sent;
            PlatformSleep(0.001);
        }
        CHECK(offset == size);
    };
    ByteWriter writer(7);
    const uint8_t body[] = {1, 2, 3};
    CHECK(writer.WriteBlob(body, 3));
    events.clear();
    sendAll(writer.Data().data(), 2);
    CHECK(server.Poll(frame++, events) && events.empty());
    sendAll(writer.Data().data() + 2, 5);
    for (int i = 0; i < 100 && events.empty(); ++i) { CHECK(server.Poll(frame++, events)); PlatformSleep(0.001); }
    CHECK(events.size() == 1 && events[0].bytes == std::vector<uint8_t>(body, body + 3));
    events.clear();
    const uint8_t invalid[] = {255, 255, 255, 255};
    sendAll(invalid, 4);
    for (int i = 0; i < 100 && events.empty(); ++i) { CHECK(server.Poll(frame++, events)); PlatformSleep(0.001); }
    CHECK(events.size() == 1 && events[0].type == TransportEvent::Type::Disconnected && !events[0].error.empty());
    CHECK(!server.Send(1, body, 3));
    CHECK(server.PeerAddress(1).port == 0);
    WebSocketTransport web;
    CHECK(!web.Connect(1, "ws://127.0.0.1:1234", &error) && !error.empty());
}

TEST(NetworkChannelsOverLocalhostSockets) {
    auto run = [](ITransport& ta, ITransport& tb) {
        ChannelEndpoint a(ta), b(tb);
        CHECK(a.AddPeer(1) && b.AddPeer(1));
        std::vector<uint8_t> large(65536, 42);
        const uint8_t small[] = {9, 0, 8};
        CHECK(a.Send(1, NetChannel::ReliableOrdered, large.data(), large.size()));
        CHECK(a.Send(1, NetChannel::ReliableOrdered, small, sizeof(small)));
        CHECK(a.Send(1, NetChannel::ReliableUnordered, nullptr, 0));
        CHECK(b.Send(1, NetChannel::ReliableOrdered, small, sizeof(small)));
        size_t replies = 0;
        std::vector<ChannelEvent> received;
        for (uint64_t frame = 0; frame < 600; ++frame) {
            std::vector<ChannelEvent> ea, eb;
            CHECK(a.Poll(frame, ea) && b.Poll(frame, eb));
            for (const auto& event : ea) {
                CHECK(event.type == ChannelEvent::Type::Message && event.bytes == std::vector<uint8_t>(small, small + 3));
                ++replies;
            }
            received.insert(received.end(), eb.begin(), eb.end());
            if (received.size() == 3 && replies == 1 && a.PendingMessages(1) == 0 && b.PendingMessages(1) == 0) break;
            PlatformSleep(0.001);
        }
        CHECK(received.size() == 3 && replies == 1 && a.PendingMessages(1) == 0 && b.PendingMessages(1) == 0);
        uint64_t ordered = 1;
        for (const auto& event : received) {
            CHECK(event.type == ChannelEvent::Type::Message);
            if (event.channel == NetChannel::ReliableOrdered) {
                CHECK(event.sequence == ordered++);
                CHECK(event.sequence == 1 ? event.bytes == large : event.bytes == std::vector<uint8_t>(small, small + 3));
            } else CHECK(event.channel == NetChannel::ReliableUnordered && event.bytes.empty());
        }
        CHECK(a.Stats(1)->invalidPackets == 0 && b.Stats(1)->invalidPackets == 0);
    };
    const uint64_t live = PlatformNetSocketsLive();
    {
        UdpTransport ua, ub;
        std::string error;
        CHECK(ua.Bind({}, &error) && ub.Bind({}, &error));
        CHECK(ua.AddPeer(1, ub.LocalAddress()) && ub.AddPeer(1, ua.LocalAddress()));
        run(ua, ub);
    }
    {
        TcpTransport ta, tb;
        std::string error;
        CHECK(tb.Listen({}, &error) && ta.Connect(1, tb.LocalAddress(), &error));
        run(ta, tb);
        CHECK(ta.Disconnect(1));
        std::vector<TransportEvent> events;
        for (uint64_t frame = 700; frame < 750 && events.empty(); ++frame) { CHECK(tb.Poll(frame, events)); PlatformSleep(0.001); }
        CHECK(events.size() == 1 && events[0].type == TransportEvent::Type::Disconnected);
    }
    CHECK(PlatformNetSocketsLive() == live);
}

TEST(NetworkNativeFallbackAndChannelDisconnect) {
    TcpTransport ta, tb;
    UdpTransport ua, ub, blackhole;
    std::string error;
    CHECK(tb.Listen({}, &error) && ta.Connect(1, tb.LocalAddress(), &error));
    CHECK(ua.Bind({}, &error) && ub.Bind({}, &error) && blackhole.Bind({}, &error));
    CHECK(ua.AddPeer(1, blackhole.LocalAddress()) && ub.AddPeer(1, ua.LocalAddress()));
    FallbackTransport fa(ua, ta, 10), fb(ub, tb, 10);
    CHECK(fa.AddPeer(1, 1) && fb.AddPeer(1, 2));
    ChannelEndpoint a(fa), b(fb);
    CHECK(a.AddPeer(1) && b.AddPeer(1));
    const uint8_t byte = 7;
    CHECK(a.Send(1, NetChannel::ReliableOrdered, &byte, 1));
    size_t received = 0;
    uint64_t frame = 0;
    for (; frame < 30; ++frame) {
        std::vector<ChannelEvent> ea, eb;
        CHECK(a.Poll(frame, ea) && b.Poll(frame, eb));
        CHECK(ea.empty());
        for (const auto& event : eb) { CHECK(event.bytes == std::vector<uint8_t>{7}); ++received; }
        PlatformSleep(0.001);
    }
    CHECK(fa.Selected(1) == FallbackTransport::Route::Tcp && fb.Selected(1) == FallbackTransport::Route::Tcp);
    CHECK(received == 1 && a.PendingMessages(1) == 0);
    CHECK(b.Send(1, NetChannel::ReliableOrdered, &byte, 1));
    CHECK(ta.Disconnect(1));
    std::vector<ChannelEvent> ea, eb;
    for (; frame < 80 && eb.empty(); ++frame) { CHECK(a.Poll(frame, ea) && b.Poll(frame, eb)); PlatformSleep(0.001); }
    CHECK(ea.size() == 1 && ea[0].type == ChannelEvent::Type::Disconnected);
    CHECK(eb.size() == 1 && eb[0].type == ChannelEvent::Type::Disconnected);
    CHECK(a.PendingMessages(1) == 0 && b.PendingMessages(1) == 0);
    CHECK(!b.Send(1, NetChannel::ReliableOrdered, &byte, 1));
}

TEST(NetworkNativeTcpQueueAndLargeFrames) {
    TcpTransport client, server;
    std::string error;
    CHECK(server.Listen({}, &error) && client.Connect(1, server.LocalAddress(), &error));
    std::vector<uint8_t> body(65536, 42);
    for (int i = 0; i < 63; ++i) CHECK(client.Send(1, body.data(), body.size()));
    CHECK(!client.Send(1, body.data(), body.size()));
    CHECK(client.QueuedBytes(1) == 63 * (body.size() + 4));
    size_t received = 0;
    for (uint64_t frame = 0; frame < 500 && (received < 63 || client.QueuedBytes(1) != 0); ++frame) {
        std::vector<TransportEvent> ea, eb;
        CHECK(client.Poll(frame, ea) && server.Poll(frame, eb));
        for (const auto& event : eb) {
            if (event.type == TransportEvent::Type::Data) { CHECK(event.bytes == body); ++received; }
            else CHECK(event.type == TransportEvent::Type::Connected);
        }
        PlatformSleep(0.001);
    }
    CHECK(received == 63 && client.QueuedBytes(1) == 0);
    for (size_t i = 0; i < TcpTransport::kMaxQueuedFrames; ++i) CHECK(client.Send(1, nullptr, 0));
    CHECK(!client.Send(1, nullptr, 0));
    CHECK(client.Disconnect(1) && client.QueuedBytes(1) == 0);
}
#endif

TEST(SceneRoundTrip) {
    Json sceneJson = MakeSampleScene("Test");
    Scene s;
    std::string err;
    CHECK(s.FromJson(sceneJson, &err));
    CHECK(s.Entities().size() == 16);
    CHECK(s.ToJson() == sceneJson);
    EntityId player = s.FindByName("Player");
    CHECK(player != kNullEntity);
    CHECK(s.Get<Script>(player) != nullptr);
}

TEST(ReflectionErrors) {
    Scene s;
    EntityId e = s.Create("E");
    const ComponentType* t = TypeRegistry::Find("MeshRenderer");
    CHECK(t != nullptr);
    void* c = s.AddComponent(e, *t);
    std::string err;
    CHECK(!ApplyComponentJson(*t, c, Json::parse(R"J({"shading": "glossy"})J"), &err));
    CHECK(err.find("smooth") != std::string::npos);
    CHECK(!ApplyComponentJson(*t, c, Json::parse(R"J({"colour": [1,0,0]})J"), &err));
    CHECK(err.find("Valid fields") != std::string::npos);
    CHECK(ApplyComponentJson(*t, c, Json::parse(R"J({"color": "#ff8000"})J"), &err));
    CHECK(static_cast<MeshRenderer*>(c)->color.r == 1.0f);
}

TEST(CommandsAndUndo) {
    Engine engine;
    engine.Call("scene.new", Json());
    size_t before = engine.GetScene().Entities().size();
    Json r = engine.Call("entity.create", Json::parse(R"J({"name": "Box", "components": {"MeshRenderer": {"color": [1,0,0]}}})J"));
    CHECK(r["ok"].asBool());
    CHECK(engine.GetScene().Entities().size() == before + 1);
    Json bad = engine.Call("component.set", Json::parse(R"J({"id": "Box", "type": "MeshRenderer", "values": {"shading": "nope"}})J"));
    CHECK(!bad["ok"].asBool());
    CHECK(bad["error"]["code"].asString() == "invalid_component_values");
    Json unknown = engine.Call("entity.creat", Json());
    CHECK(unknown["error"]["hint"].asString().find("entity.create") != std::string::npos);
    Json missing = engine.Call("entity.get", Json());
    CHECK(missing["error"]["code"].asString() == "missing_argument");
    CHECK(engine.Call("history.undo", Json())["ok"].asBool());
    CHECK(engine.GetScene().Entities().size() == before);
    CHECK(engine.Call("history.redo", Json())["ok"].asBool());
    CHECK(engine.GetScene().FindByName("Box") != kNullEntity);
}

TEST(UndoMergeKey) {
    // Editor drags send many component.set calls with one merge key; they must
    // undo as a single step, and a new key starts a new step.
    Engine engine;
    engine.Call("scene.new", Json());
    CHECK(engine.Call("entity.create", Json::parse(R"J({"name": "Box", "components": {"Transform": {}}})J"))["ok"].asBool());
    size_t base = engine.UndoDepth();
    for (int i = 1; i <= 5; ++i) {
        Json a = Json::parse(R"J({"id": "Box", "type": "Transform", "values": {"position": [0, 0, 0]}, "merge": "drag1"})J");
        a["values"]["position"][0] = static_cast<double>(i);
        CHECK(engine.Call("component.set", a)["ok"].asBool());
    }
    CHECK(engine.UndoDepth() == base + 1);
    CHECK(engine.Call("component.set", Json::parse(R"J({"id": "Box", "type": "Transform", "values": {"position": [9, 0, 0]}, "merge": "drag2"})J"))["ok"].asBool());
    CHECK(engine.UndoDepth() == base + 2);
    CHECK(engine.Call("history.undo", Json())["ok"].asBool());
    CHECK(engine.Call("history.undo", Json())["ok"].asBool());
    Json t = engine.Call("entity.get", Json::parse(R"J({"id": "Box"})J"));
    CHECK(t["result"]["components"]["Transform"]["position"][0].asNumber() == 0.0);
}

TEST(FileCopyAndRemove) {
    // Used by `oe package` to assemble the game folder.
    std::string root = "build/test_fs";
    RemoveAll(root);
    CHECK(WriteTextFile(root + "/src/a.bin", std::string("x\0y", 3)));
    CHECK(CopyFileTo(root + "/src/a.bin", root + "/out/deep/a.bin"));
    std::vector<unsigned char> data;
    CHECK(ReadBinaryFile(root + "/out/deep/a.bin", data) && data.size() == 3 && data[1] == 0);
    CHECK(!CopyFileTo(root + "/missing.bin", root + "/out/m.bin"));
    CHECK(RemoveAll(root));
    CHECK(!FileExists(root + "/src/a.bin"));
    CHECK(AbsolutePath("C:/ownengine/game/../assets") == "C:/ownengine/assets");
    CHECK(RelativePath("C:/ownengine/assets/model.glb", "C:/ownengine") == "assets/model.glb");
}

std::string TempProject(const char* name);  // defined below

TEST(SimulationDeterminismAndInput) {
    Json sceneJson = MakeSampleScene("Sim");
    auto run = [&](bool pressW) {
        Engine engine;
        std::string err;
        CHECK(engine.Open(TempProject("sim_determinism"), &err));  // the player moves through a project script
        engine.GetScene().FromJson(sceneJson, &err);
        if (pressW) engine.Call("input.key", Json::parse(R"J({"key": "W"})J"));
        engine.Call("sim.step", Json::parse(R"J({"frames": 60})J"));
        const Transform* t = engine.GetScene().Get<Transform>(engine.GetScene().FindByName("Player"));
        Json out = engine.Call("render.screenshot", Json::parse(R"J({"width": 64, "height": 36, "inline": false})J"));
        return std::make_pair(t->position, out["result"]["hash"].asString());
    };
    auto a = run(false), b = run(false), c = run(true);
    CHECK(a.second == b.second);          // same inputs -> identical frame
    CHECK(a.first == b.first);
    CHECK(c.first.z < a.first.z - 3.5f);  // W moves the player ~4 m forward in 1 s
    // sim.stop restores the edit-time scene.
    Engine engine;
    std::string err;
    engine.GetScene().FromJson(sceneJson, &err);
    engine.Call("sim.step", Json::parse(R"J({"frames": 30})J"));
    engine.Call("sim.stop", Json());
    CHECK(engine.GetScene().ToJson() == sceneJson);
}

TEST(RenderPickAndPng) {
    Engine engine;
    std::string err;
    engine.GetScene().FromJson(MakeSampleScene("Pick"), &err);
    // Look straight down at the player sphere from above.
    Json pick = engine.Call("render.pick", Json::parse(R"J({"x": 32, "y": 32, "width": 64, "height": 64, "camera": {"eye": [0, 6, 2.01], "target": [0, 0.5, 2], "fov": 30}})J"));
    CHECK(pick["ok"].asBool());
    CHECK(pick["result"]["name"].asString() == "Player");
    RenderTarget rt;
    rt.Resize(32, 16);
    engine.RenderGameView(rt);
    std::vector<uint8_t> png = EncodePng(rt.ToImage());
    CHECK(png.size() > 60);
    CHECK(png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
}

TEST(PathSandbox) {
    Engine engine;
    Json r = engine.Call("scene.save", Json::parse(R"J({"path": "../../escape.scene.json"})J"));
    CHECK(!r["ok"].asBool());
    CHECK(r["error"]["code"].asString() == "path_outside_project");
}


// ----- scripting --------------------------------------------------------------

// Fresh project (with the template scripts) in a temp directory.
std::string TempProject(const char* name) {
    namespace fs = std::filesystem;
    // Keep fixtures under the staged source tree: Android shell and Node on
    // Windows do not necessarily have a writable POSIX /tmp directory.
    fs::path dir = fs::path(TestSourceDir()) / "build/test_projects" / (std::string("oe_tests_") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::string d = dir.generic_string();
    std::string err;
    CHECK(CreateProject(d, name, &err));
    if (!err.empty()) std::printf("  project fixture: %s\n", err.c_str());
    return d;
}

Json Call(Engine& e, const char* cmd, const char* args = "{}") { return e.Call(cmd, Json::parse(args)); }

TEST(NetworkInactiveZeroCost) {
    const uint64_t syncInstances = FrameSync::InstancesCreated(), authorityInstances = Authority::InstancesCreated();
    const uint64_t sessionInstances = Session::InstancesCreated(), sessionPolls = Session::PollCalls();
    const uint64_t instances = LoopbackNetwork::InstancesCreated();
    const uint64_t polls = LoopbackNetwork::PollCalls();
    const uint64_t channelInstances = ChannelEndpoint::InstancesCreated();
    const uint64_t channelPolls = ChannelEndpoint::PollCalls();
    const uint64_t socketsCreated = PlatformNetSocketsCreated(), socketsLive = PlatformNetSocketsLive();
    const std::string project = TempProject("network_inactive");
    std::string text;
    CHECK(ReadTextFile(JoinPath(project, "project.json"), text));
    Json settings = Json::parse(text);
    CHECK(settings["network"].isNull());
    auto run = [&]() {
        Engine e;
        std::string error;
        CHECK(e.Open(project, &error));
        CHECK(Call(e, "net.state", "{}")["result"]["state"].asString() == "none");
        CHECK(Call(e, "net.players", "{}")["result"].size() == 1);
        CHECK(Call(e, "net.stats", "{}")["result"]["peers"].size() == 0);
        CHECK(!Call(e, "net.host", "{}")["ok"].asBool());
        CHECK(Call(e, "net.leave", "{}")["ok"].asBool());
        std::vector<std::string> hashes;
        for (int step = 0; step < 4; ++step) {
            CHECK(Call(e, "input.key", step % 2 == 0 ? R"({"key":"W","down":true})" :
                                                     R"({"key":"W","down":false})")["ok"].asBool());
            CHECK(Call(e, "sim.step", R"({"frames":30})")["ok"].asBool());
            Json shot = Call(e, "render.screenshot", R"({"width":64,"height":36,"inline":false})");
            CHECK(shot["ok"].asBool());
            hashes.push_back(shot["result"]["hash"].asString());
            hashes.push_back(e.GetScene().ToJson().dump());
        }
        CHECK(Call(e, "sim.stop", "{}")["ok"].asBool());
        return hashes;
    };
    const auto absent = run();
    CHECK(absent == run());
    settings["network"] = Json::parse(R"({"mode":"none"})");
    CHECK(WriteTextFile(JoinPath(project, "project.json"), settings.dump(2)));
    CHECK(absent == run());
    CHECK(LoopbackNetwork::InstancesCreated() == instances);
    CHECK(LoopbackNetwork::PollCalls() == polls);
    CHECK(ChannelEndpoint::InstancesCreated() == channelInstances);
    CHECK(ChannelEndpoint::PollCalls() == channelPolls);
    CHECK(PlatformNetSocketsCreated() == socketsCreated && PlatformNetSocketsLive() == socketsLive);
    CHECK(FrameSync::InstancesCreated() == syncInstances);
    CHECK(Authority::InstancesCreated() == authorityInstances);
    CHECK(Session::InstancesCreated() == sessionInstances && Session::PollCalls() == sessionPolls);
    CHECK(RemoveAll(project));
}

Json NetworkEval(Engine& engine, const std::string& code, EntityId entity = 0) {
    return engine.Scripts().Eval(code, entity)["value"];
}
TEST(NetworkNoneLuaRpcAndLimits) {
    Engine e;
    const uint64_t sockets = PlatformNetSocketsCreated(), sessions = Session::InstancesCreated();
    CHECK(NetworkEval(e, "return {net.isServer(), net.isClient(), net.isHost(), net.localPlayer(), net.players()}", 0) ==
          Json::parse("[true,false,false,1,[1]]"));
    CHECK(NetworkEval(e, "got=0; net.on('add',function(n) got=got+n; sender=net.sender() end); net.rpc('server','add',3); return {got,sender}", 0) ==
          Json::parse("[3,1]"));
    CHECK(Call(e, "net.rpc", R"({"target":"all","name":"add","args":[4]})")["ok"].asBool());
    CHECK(NetworkEval(e, "return got", 0).asInt() == 7);
    CHECK(Call(e, "net.rpc", R"({"target":"others","name":"add","args":[100]})")["ok"].asBool());
    CHECK(NetworkEval(e, "return got", 0).asInt() == 7);
    CHECK(!Call(e, "net.rpc", R"({"target":"owner","name":"add"})")["ok"].asBool());
    CHECK(!Call(e, "net.rpc", R"({"name":"","args":[]})")["ok"].asBool());
    CHECK(!Call(e, "net.rpc", R"({"name":"add","args":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]})")["ok"].asBool());
    Json invalid = Json::MakeObject(); invalid["seed"] = std::numeric_limits<double>::infinity();
    CHECK(e.Call("net.host", invalid)["error"]["code"].asString() == "invalid_argument");
    invalid["seed"] = 1e100;
    CHECK(e.Call("net.host", invalid)["error"]["code"].asString() == "invalid_argument");
    CHECK(NetworkEval(e, "net.on('add',nil); net.rpc('server','add',9); return got", 0).asInt() == 7);
    CHECK(NetworkEval(e, "local ok=pcall(function() for i=1,65 do net.on('h'..i,function() end) end end); return ok", 0).asBool() == false);
    CHECK(PlatformNetSocketsCreated() == sockets && Session::InstancesCreated() == sessions);
    CHECK(!NetworkEval(e, "local t={}; t.self=t; return pcall(function() net.rpc('all','bad',t) end)")[0].asBool());
}

std::string NetworkProject(const char* name, const char* transport, int maxPlayers = 4) {
    std::string project = TempProject(name), text;
    CHECK(ReadTextFile(JoinPath(project, "project.json"), text));
    Json settings = Json::parse(text);
    settings["network"] = Json::parse(R"({"mode":"lockstep","port":0,"gameId":"test-game"})");
    settings["network"]["transport"] = transport;
    settings["network"]["maxPlayers"] = maxPlayers;
    CHECK(WriteTextFile(JoinPath(project, "project.json"), settings.dump(2)));
    return project;
}
void NetworkReceiver(Engine& e) {
    CHECK(Call(e, "script.write", R"({"path":"scripts/net_test.lua","source":"local M={}\nfunction M:onStart() received={} joined={} left={} states={} net.on('ping',function(n) received[#received+1]={net.sender(),n} end) end\nfunction M:onPlayerJoined(id) joined[#joined+1]=id end\nfunction M:onPlayerLeft(id) left[#left+1]=id end\nfunction M:onNetState(s) states[#states+1]=s end\nreturn M"})")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"({"name":"NetTest","components":{"Script":{"path":"scripts/net_test.lua"}}})")["ok"].asBool());
}
void NetworkSteps(Engine& a, Engine& b, int count, Engine* c = nullptr) {
    for (int frame = 0; frame < count; ++frame) {
        a.Step(1); b.Step(1); if (c) c->Step(1);
    }
}


Session::Random SessionTestRandom(uint64_t seed);
std::string SyncProject(const char* name, const char* transport, bool rollback = false) {
    std::string project = NetworkProject(name, transport), text;
    CHECK(ReadTextFile(JoinPath(project, "project.json"), text)); Json config = Json::parse(text);
    config["network"]["mode"] = rollback ? "rollback" : "lockstep";
    config["network"]["actions"] = Json::parse("[\"W\",\"Space\"]");
    config["network"]["axes"] = Json::parse("[\"LeftX\"]");
    config["network"]["hashInterval"] = 10;
    CHECK(WriteTextFile(JoinPath(project, "project.json"), config.dump(2)));
    CHECK(WriteTextFile(JoinPath(project, "scripts/sync_test.lua"), R"(
local M={}
function M:onStart() self.n=0; game.set('total',0) end
function M:onUpdate()
 local total=game.get('total',0)
 for _,id in ipairs(net.players()) do
  local p=input.player(id)
  if p.down('W') then total=total+id end
  if p.pressed('Space') then total=total+10 end
  total=total+p.axis('LeftX')
 end
 game.set('total',total); self.n=self.n+1
 self:set('Transform',{position={total,0,0}})
end
return M
)"));
    return project;
}
void SyncPrepare(Engine& host, Engine& client, const std::string& project, const char* transport) {
    std::string error; CHECK(host.Open(project, &error)); CHECK(client.Open(project, &error));
    for (Engine* engine : {&host, &client}) {
        engine->GetScene().Clear();
        CHECK(Call(*engine, "entity.create", R"({"name":"Counter","components":{"Transform":{},"Script":{"path":"scripts/sync_test.lua"}}})")["ok"].asBool());
    }
    CHECK(Call(host, "net.host", R"({"seed":71,"room":"sync-room"})")["ok"].asBool());
    Json join=Json::MakeObject(); join["address"]=std::string(transport)=="loopback"?"sync-room":"127.0.0.1";
    if (std::string(transport)!="loopback") join["port"]=host.Network()->State()["port"];
    CHECK(client.Call("net.join",join)["ok"].asBool());
    NetworkSteps(host,client,30);
    CHECK(Call(host,"net.ready",R"({"ready":true})")["ok"].asBool());
    CHECK(Call(client,"net.ready",R"({"ready":true})")["ok"].asBool());
    NetworkSteps(host,client,20);
    CHECK(Call(host,"net.start")["ok"].asBool());
}
std::string AuthorityProject(const char* name, const char* transport, bool prefab = false) {
    std::string project = SyncProject(name, transport), text;
    CHECK(ReadTextFile(JoinPath(project, "project.json"), text)); Json config = Json::parse(text);
    config["network"]["mode"] = "authoritative"; config["network"]["snapshotRate"] = 20;
    if (prefab) config["network"]["playerPrefab"] = "prefabs/network_player.prefab.json";
    CHECK(WriteTextFile(JoinPath(project, "project.json"), config.dump(2)));
    CHECK(WriteTextFile(JoinPath(project, "scripts/authority.lua"), R"(
local M={}
function M:onStart()
 net.on('owner-message',function(id,n) game.set('owner-message',(game.get('owner-message') or 0)+n) end)
 if net.isServer() and self.name=='P2' then
  timer.after(0.3,function() scene.create('Spawned',{Transform={position={3,0,0}},NetSync={fields={['Transform.position']={onChange=true}}}}) end)
  timer.after(0.8,function() local id=scene.find('Spawned'); if id then scene.destroy(id) end end)
 end
end
function M:onUpdate()
 local n=self:get('NetPlayer'); local p=input.player(n.player); local t=self:get('Transform')
 if p.down('W') then t.position.x=t.position.x+1 end
 if p.pressed('Space') then t.position.y=t.position.y+1 end
 self:set('Transform',t)
end
return M
)"));
    CHECK(WriteTextFile(JoinPath(project,"scripts/drift.lua"), "local M={}\nfunction M:onUpdate(dt) self:translate(dt,0,0) end\nreturn M\n"));
    CHECK(WriteTextFile(JoinPath(project,"scripts/authority_physics.lua"), R"(
local M={}
function M:onUpdate()
 local p=input.player(self:get('NetPlayer').player)
 self:set('RigidBody2D',{velocity={x=p.down('W') and 6 or 0,y=0,z=0}})
end
return M
)"));
    Json player = Json::parse(R"({"format":"ownengine.prefab","version":1,"name":"NetworkPlayer","entities":[{"id":1,"name":"Player","components":{"Transform":{},"Script":{"path":"scripts/authority.lua"},"NetPlayer":{},"NetSync":{"fields":{"Transform.position":{"onChange":true},"Tag.tags":{"ownerOnly":true}}},"Tag":{"tags":"secret"}}}]})");
    CHECK(WriteTextFile(JoinPath(project,"prefabs/network_player.prefab.json"),player.dump())); return project;
}
void AuthorityPrepare(Engine& host, Engine& client, const std::string& project, const char* transport, bool prefab = false, bool physics = false) {
    std::string error; CHECK(host.Open(project,&error)); CHECK(client.Open(project,&error));
    for (Engine* e : {&host,&client}) {
        e->GetScene().Clear();
        if (!prefab) {
            CHECK(Call(*e,"entity.create",R"({"name":"P1","components":{"Transform":{},"Script":{"path":"scripts/authority.lua"},"NetPlayer":{"player":1,"team":1},"NetSync":{"owner":1,"fields":{"Transform.position":{"onChange":true},"Tag.tags":{"ownerOnly":true}}},"Tag":{"tags":"secret-host"}}})")["ok"].asBool());
            CHECK(Call(*e,"entity.create",R"({"name":"P2","components":{"Transform":{},"Script":{"path":"scripts/authority.lua"},"NetPlayer":{"player":2,"team":2},"NetSync":{"owner":2,"fields":{"Transform.position":{"onChange":true},"Tag.tags":{"ownerOnly":true}}},"Tag":{"tags":"secret-client"}}})")["ok"].asBool());
            CHECK(Call(*e,"entity.create",R"({"name":"Remote","components":{"Transform":{},"Script":{"path":"scripts/drift.lua"},"NetSync":{"fields":{"Transform.position":{"onChange":true,"quantize":0.1}}}}})")["ok"].asBool());
            CHECK(Call(*e,"entity.create",R"({"name":"Far","components":{"Transform":{"position":[5000,0,0]},"NetSync":{"distance":10}}})")["ok"].asBool());
            CHECK(Call(*e,"entity.create",R"({"name":"Team1","components":{"Transform":{},"NetSync":{"teamOnly":true,"team":1}}})")["ok"].asBool());
        }
    }
    if (physics) {
        for (Engine* e : {&host,&client}) {
            for (const char* name : {"P1","P2"}) {
                Json collider=Json::MakeObject();collider["id"]=name;collider["type"]="Collider2D"; CHECK(e->Call("component.add",collider)["ok"].asBool());
                Json body=Json::MakeObject();body["id"]=name;body["type"]="RigidBody2D";body["values"]=Json::parse(R"({"gravityScale":0,"fixedRotation":true})");CHECK(e->Call("component.add",body)["ok"].asBool());
                Json script=Json::MakeObject();script["id"]=name;script["type"]="Script";script["values"]=Json::parse(R"({"path":"scripts/authority_physics.lua"})");CHECK(e->Call("component.set",script)["ok"].asBool());
                Json fields=Json::MakeObject();fields["id"]=name;fields["type"]="NetSync";fields["values"]=Json::parse(R"({"fields":{"Transform.position":{"onChange":true},"RigidBody2D.velocity":{"onChange":true}}})");CHECK(e->Call("component.set",fields)["ok"].asBool());
            }
            e->GetScene().Get<Transform>(e->GetScene().FindByName("P1"))->position.y=10;
        }
    }
    CHECK(Call(host,"net.host",R"({"seed":71,"room":"authority-room"})")["ok"].asBool());
    Json join = Json::MakeObject(); join["address"] = std::string(transport)=="loopback"?"authority-room":"127.0.0.1";
    if (std::string(transport)!="loopback") join["port"] = host.Network()->State()["port"];
    CHECK(client.Call("net.join",join)["ok"].asBool()); NetworkSteps(host,client,30);
    CHECK(Call(host,"net.ready",R"({"ready":true})")["ok"].asBool()); CHECK(Call(client,"net.ready",R"({"ready":true})")["ok"].asBool()); NetworkSteps(host,client,20);
    NetworkSteps(host,client,320);
    CHECK(Call(host,"net.start")["ok"].asBool()); NetworkSteps(host,client,10);
}
TEST(NetworkAuthoritativeOwnershipPredictionAndRelevance) {
    std::string project = AuthorityProject("authority","loopback"); Engine host,client;
    AuthorityPrepare(host,client,project,"loopback");
    CHECK(host.NetworkCall("state",Json())["sync"]["state"].asString()=="running");
    CHECK(client.NetworkCall("state",Json())["sync"]["state"].asString()=="running");
    EntityId host1=host.GetScene().FindByName("P1"),host2=host.GetScene().FindByName("P2");
    host.Input().down.insert("W"); client.Input().down.insert("W"); client.Input().pressedThisFrame.insert("Space");
    client.Audio().StartCapture(); NetworkSteps(host,client,24);
    CHECK(host.GetScene().Get<Transform>(host1)->position.x>10 && host.GetScene().Get<Transform>(host2)->position.x>10);
    CHECK(host.GetScene().Get<Transform>(host1)->position.y==0 && host.GetScene().Get<Transform>(host2)->position.y==1);
    EntityId own=client.GetScene().FindByName("P2"); CHECK(own!=0);
    Json replicated=Call(client,"net.entities")["result"];
    for (const auto& entity:replicated.items()) {
        if (entity["fields"]["name"].asString()=="P1") CHECK(!entity["fields"].has("Tag.tags"));
        if (entity["fields"]["name"].asString()=="P2") CHECK(entity["fields"]["Tag.tags"].asString()=="secret-client");
        if (entity["fields"]["name"].asString()=="Remote") {
            float x=client.GetScene().Get<Transform>(static_cast<EntityId>(entity["id"].asNumber()))->position.x;
            CHECK(x<=entity["fields"]["Transform.position"][0].asNumber()+0.001);
        }
    }
    CHECK(client.GetScene().Get<Transform>(own)->position.y==1);
    CHECK(client.NetworkCall("stats",Json())["corrections"].asNumber()>0);
    CHECK(Call(host,"net.rpc",R"({"target":"owner","entity":"P2","name":"owner-message","args":[2,7]})")["ok"].asBool());
    NetworkSteps(host,client,3);
    CHECK(client.GameData()["owner-message"].asNumber()==7);
    CHECK(host.GameData()["owner-message"].isNull());
    CHECK(Call(client,"net.rpc",R"({"target":"owner","entity":"P1","name":"owner-message","args":[1,5]})")["ok"].asBool());
    NetworkSteps(host,client,3); CHECK(host.GameData()["owner-message"].asNumber()==5);
    CHECK(!Call(client,"component.set",R"({"id":"P1","type":"Transform","values":{"position":[999,0,0]}})")["ok"].asBool());
    CHECK(!Call(host,"script.eval",R"({"code":"1"})")["ok"].asBool());
    // Client holds prediction input while the server is temporarily not advancing.
    uint64_t before=client.Frame(); client.Step(100); CHECK(client.Frame()<=before+32);
    NetworkSteps(host,client,50); CHECK(client.NetworkCall("state",Json())["sync"]["state"].asString()=="running");
    CHECK(client.GetScene().FindByName("Far")==0 && client.GetScene().FindByName("Team1")==0);
    CHECK(host.GetScene().FindByName("Spawned")==0 && client.GetScene().FindByName("Spawned")==0);
    CHECK(host.Scripts().Errors().empty() && client.Scripts().Errors().empty());
    uint64_t ack=static_cast<uint64_t>(client.NetworkCall("state",Json())["sync"]["acknowledgedInput"].asNumber());
    CHECK(client.Audio().SaveState()->capture.size()<=ack*1600);
    // Built-in controller uses the controlling authenticated player's input.
    host.Stop(); client.Stop(); RemoveAll(project);
}
TEST(NetworkAuthoritativePrefabAndNativeTcpUdp) {
#ifndef __EMSCRIPTEN__
    for (const char* transport : {"tcp","udp"}) {
        std::string project=AuthorityProject(transport,transport,true); Engine host,client;
        Engine namespaceCheck; std::string namespaceError; CHECK(namespaceCheck.Open(project,&namespaceError));
        namespaceCheck.GetScene().Clear(); namespaceCheck.GetScene().Create("Existing");
        CHECK(WriteTextFile(JoinPath(project,"prefabs/namespace.prefab.json"),R"({"format":"ownengine.prefab","version":1,"entities":[{"id":1,"name":"Owned","components":{"Transform":{},"NetSync":{"owner":2},"NetPlayer":{"player":2}}},{"id":2,"name":"Child","parent":1,"components":{"Transform":{}}}]})"));
        EntityId root=namespaceCheck.InstantiatePrefabFile("prefabs/namespace.prefab.json",0);
        CHECK(namespaceCheck.GetScene().Get<NetSync>(root)->owner==2 && namespaceCheck.GetScene().Get<NetPlayer>(root)->player==2);

        AuthorityPrepare(host,client,project,transport,true); client.Input().down.insert("W");
        NetworkSteps(host,client,150);
        CHECK(host.GetScene().Pool<NetPlayer>().size()==2 && client.GetScene().Pool<NetPlayer>().size()==2);
        CHECK(client.NetworkCall("state",Json())["sync"]["state"].asString()=="running");
        CHECK(client.NetworkCall("stats",Json())["corrections"].asNumber()>10);
        for (const auto& player:host.GetScene().Pool<NetPlayer>()) if(player.second.player==2) CHECK(host.GetScene().Get<Transform>(player.first)->position.x>50);
        CHECK(host.Scripts().Errors().empty() && client.Scripts().Errors().empty()); RemoveAll(project);
    }
#endif
}

TEST(NetworkAuthoritativePhysicsPrediction) {
    std::string project=AuthorityProject("authority_physics","loopback"); Engine host,client;
    AuthorityPrepare(host,client,project,"loopback",false,true); client.Input().down.insert("W");
    NetworkSteps(host,client,150); client.Input().down.erase("W"); NetworkSteps(host,client,40);
    EntityId serverPlayer=host.GetScene().FindByName("P2"),localPlayer=client.GetScene().FindByName("P2");
    CHECK(serverPlayer && localPlayer);
    CHECK(std::fabs(host.GetScene().Get<Transform>(serverPlayer)->position.x-client.GetScene().Get<Transform>(localPlayer)->position.x)<0.01f);
    CHECK(host.GetScene().Get<Transform>(serverPlayer)->position.x>10);
    CHECK(client.NetworkCall("state",Json())["sync"]["state"].asString()=="running");
    CHECK(host.Scripts().Errors().empty() && client.Scripts().Errors().empty()); RemoveAll(project);
}

std::vector<uint8_t> AuthorityPacket(uint8_t kind, const Json& body, uint64_t epoch = 1) {
    std::vector<uint8_t> payload; CHECK(EncodeJson(body,payload,56000)); ByteWriter writer(60000); writer.WriteU8(kind); writer.WriteU64(epoch);
    writer.WriteBlob(payload.data(),payload.size()); return writer.Data();
}
TEST(NetworkAuthoritativeDeltaAckAndBounds) {
    SessionConfig config; config.mode="authoritative"; config.sync.keys={"W"};
    std::vector<std::vector<uint8_t>> outbound, replies;
    Authority host(config,true,1,77,[&](uint32_t,const std::vector<uint8_t>& bytes){outbound.push_back(bytes);return true;});
    Authority client(config,false,2,77,[&](uint32_t,const std::vector<uint8_t>& bytes){replies.push_back(bytes);return true;});
    auto exchange=[&] {
        for(int n=0;n<4;++n) {
            auto sent=std::move(outbound); outbound.clear(); for(const auto& bytes:sent) client.Receive(1,bytes);
            auto returned=std::move(replies); replies.clear(); for(const auto& bytes:returned) host.Receive(2,bytes);
        }
    };
    std::string error; CHECK(host.Start({1,2},&error)); exchange(); CHECK(host.TakeStart() && client.TakeStart());
    FrameInput held; held.down=1; CHECK(client.Submit(held)==1); exchange(); auto inputs=host.Consume(FrameInput{});
    CHECK(inputs.at(1).down==0 && inputs.at(2).down==1);
    Json world=Json::parse(R"({"1":{"source":1,"owner":2,"name":"Player","parent":0,"predict":true,"prefab":"","always":["Transform.rotation"],"Transform.position":[1,0,0],"Transform.rotation":[0,0,0],"Tag.tags":"secret"}})");
    host.Snapshot(1,{{2,world}}); exchange(); auto updates=client.DrainUpdates();
    CHECK(updates.size()==1 && updates.back().world==world && updates.back().acknowledged==1);
    world["1"]["Transform.position"]=Json::parse("[2,0,0]"); host.Snapshot(2,{{2,world}});
    CHECK(outbound.size()==1);
    ByteReader reader(outbound[0].data(),outbound[0].size()); uint8_t kind; uint64_t epoch; std::vector<uint8_t> blob;
    CHECK(reader.ReadU8(kind) && reader.ReadU64(epoch) && reader.ReadBlob(blob,56000));
    Json delta; CHECK(DecodeJson(blob.data(),blob.size(),delta));
    CHECK(delta["base"].asNumber()==1 && delta["delta"]["set"]["1"].has("Transform.position") && delta["delta"]["set"]["1"].has("Transform.rotation"));
    CHECK(!delta["delta"]["set"]["1"].has("Tag.tags")); exchange(); updates=client.DrainUpdates(); CHECK(updates.size()==1 && updates.back().world==world);
    host.Snapshot(3,{{2,Json::MakeObject()}}); exchange(); CHECK(client.DrainUpdates().back().world.size()==0);
    Json spoof=Json::parse(R"({"frame":2,"player":1,"input":{"down":"1","pulse":"0","axes":[0,0,0,0,0,0]}})");
    host.Receive(2,AuthorityPacket(35,spoof)); spoof.erase("player"); spoof["frame"]=1000; host.Receive(2,AuthorityPacket(35,spoof));
    spoof["frame"]=2; spoof["input"]["down"]="2"; host.Receive(2,AuthorityPacket(35,spoof));
    host.Receive(2,AuthorityPacket(36,Json::MakeObject())); host.Receive(99,AuthorityPacket(35,spoof));
    CHECK(host.State()["rejected"].asNumber()==5);
    client.Receive(2,AuthorityPacket(36,Json::MakeObject()));
    std::vector<uint8_t> deep(17,'['); deep.insert(deep.end(),17,']'); ByteWriter malformed(100); malformed.WriteU8(36); malformed.WriteU64(1); malformed.WriteBlob(deep.data(),deep.size()); client.Receive(1,malformed.Data());
    CHECK(client.State()["rejected"].asNumber()==2);
    SessionConfig bad; for(const char* value:{R"({"network":{"mode":"authoritative","snapshotRate":0}})",R"({"network":{"mode":"authoritative","predictionFrames":65}})",R"({"network":{"mode":"authoritative","interpolationFrames":31}})",R"({"network":{"mode":"authoritative","playerPrefab":"../escape.json"}})"}) CHECK(!SessionConfig::Parse(Json::parse(value),bad,&error));
}
TEST(NetworkAuthoritativeSeededSessionsTenThousandFrames) {
    LoopbackConfig faults; faults.seed=931; faults.lossPermille=60; faults.duplicatePermille=80; faults.latencyFrames=1; faults.reorderFrames=2;
    auto wire=std::make_shared<LoopbackNetwork>(faults); SessionConfig config; config.mode="authoritative"; config.transport="loopback"; config.sync.keys={"W"};
    Session server(config,std::make_unique<LoopbackTransport>(wire,1),true,1,"Server",71,SessionTestRandom(810));
    Session client(config,std::make_unique<LoopbackTransport>(wire,2),false,1,"Client",0,SessionTestRandom(811));
    uint64_t tick=0; for(;tick<200;++tick) {server.Advance(tick);client.Advance(tick);}
    CHECK(server.Connected() && client.Connected());
    Authority host(config,true,1,99,[&](uint32_t player,const std::vector<uint8_t>& b){return server.SendSync(player,b);});
    Authority guest(config,false,2,99,[&](uint32_t player,const std::vector<uint8_t>& b){return client.SendSync(player,b);});
    std::string error; CHECK(host.Start({1,2},&error)); uint64_t total=0,frames=0; Json last;
    for(;tick<200000 && frames<10000;++tick) {
        server.Advance(tick);client.Advance(tick);
        for(const auto& msg:server.DrainSync()) host.Receive(msg.first,msg.second);
        for(const auto& msg:client.DrainSync()) guest.Receive(msg.first,msg.second);
        host.TakeStart();guest.TakeStart();
        if(guest.Running() && guest.CanPredict()) {FrameInput input;input.down=(tick/7)%2;guest.Submit(input);}
        if(host.Running()) {
            auto inputs=host.Consume(FrameInput{});total+=inputs[2].down; ++frames;
            if(frames%3==0) {Json world=Json::MakeObject();world["1"]=Json::MakeObject();world["1"]["Transform.position"]=Json::parse("[0,0,0]");world["1"]["Transform.position"][0]=total;host.Snapshot(frames,{{2,world}});}
        }
        for(const auto& update:guest.DrainUpdates()) last=update.world;
        if(host.State()["state"].asString()=="stopped" || guest.State()["state"].asString()=="stopped") break;
    }
    CHECK(frames==10000 && host.Running() && guest.Running() && wire->DroppedMessages()>0 && last.size()==1);
    Json world=Json::MakeObject();world["1"]=Json::MakeObject();world["1"]["Transform.position"]=Json::parse("[0,0,0]");world["1"]["Transform.position"][0]=total;host.Snapshot(frames,{{2,world}});
    for(int n=0;n<500;++n,++tick) {server.Advance(tick);client.Advance(tick);for(const auto& msg:server.DrainSync())host.Receive(msg.first,msg.second);for(const auto& msg:client.DrainSync())guest.Receive(msg.first,msg.second);for(const auto& update:guest.DrainUpdates())last=update.world;}
    CHECK(last==world);
}


TEST(NetworkLoopbackFaultReconfiguration) {
    auto wire = std::make_shared<LoopbackNetwork>(); LoopbackTransport a(wire, 1), b(wire, 2);
    LoopbackConfig config; config.latencyFrames = 3; config.seed = 45; wire->Configure(config);
    uint8_t old = 1, fresh = 2; CHECK(a.Send(2, &old, 1));
    config.latencyFrames = 0; wire->Configure(config); CHECK(a.Send(2, &fresh, 1));
    std::vector<TransportEvent> events; CHECK(b.Poll(0, events)); CHECK(events.size() == 1 && events[0].bytes[0] == fresh);
    events.clear(); CHECK(b.Poll(2, events)); CHECK(events.empty()); CHECK(wire->PendingMessages() == 1);
    CHECK(b.Poll(3, events)); CHECK(events.size() == 1 && events[0].bytes[0] == old);
    config.lossPermille = 1001;
    bool refused = false; try { wire->Configure(config); } catch (const std::invalid_argument&) { refused = true; }
    CHECK(refused && wire->Configuration().lossPermille == 0);
}

TEST(NetworkLocalPreviewAndFaultControls) {
    uint64_t sockets = PlatformNetSocketsCreated();
    for (bool rollback : {false, true}) {
        std::string project = SyncProject(rollback ? "preview_rollback" : "preview_lockstep", "tcp", rollback), error;
        Engine host; CHECK(host.Open(project, &error)); host.GetScene().Clear();
        CHECK(Call(host, "entity.create", R"({"name":"Counter","components":{"Transform":{},"Script":{"path":"scripts/sync_test.lua"}}})")["ok"].asBool());
        Json edit = host.GetScene().ToJson();
        CHECK(!Call(host, "net.spawn_local_peers", R"({"count":4})")["ok"].asBool());
        CHECK(!Call(host, "net.spawn_local_peers", R"({"count":1.5})")["ok"].asBool());
        CHECK(Call(host, "net.spawn_local_peers", R"({"count":2,"seed":71})")["ok"].asBool());
        CHECK(Call(host, "net.simulate", R"({"seed":45,"latencyFrames":1,"jitterFrames":1,"lossPermille":50,"duplicatePermille":100,"reorderFrames":1})")["ok"].asBool());
        CHECK(!Call(host, "net.simulate", R"({"latencyFrames":3601})")["ok"].asBool());
        host.Step(160);
        Json peers = Call(host, "net.local_peers")["result"]; CHECK(peers.size() == 3);
        for (const Json& peer : peers.items()) CHECK(peer["state"]["sync"]["state"].asString() == "running");
        CHECK(!Call(host, "net.peer_call", R"({"peer":1,"command":"sim.stop"})")["ok"].asBool());
        CHECK(Call(host, "net.peer_call", R"({"peer":1,"command":"input.key","args":{"key":"W","down":true}})")["result"]["ok"].asBool());
        host.Step(120);
        CHECK(host.GameData()["total"].asNumber() > 30);
        CHECK(Call(host, "net.peer_call", R"({"peer":1,"command":"input.key","args":{"key":"W","down":false}})")["result"]["ok"].asBool());
        // Drain old deadlines before asserting steady held-input agreement.
        CHECK(Call(host, "net.simulate", R"({"latencyFrames":0,"jitterFrames":0,"lossPermille":0,"duplicatePermille":0,"reorderFrames":0})")["ok"].asBool());
        host.Step(160);
        for (size_t i = 1; i < 3; ++i) {
            CHECK(host.LocalPeer(i)->GameData().dump() == host.GameData().dump());
            CHECK(Call(*host.LocalPeer(i), "net.desync_report")["result"].size() == 0);
        }
        CHECK(Call(host, "net.stats")["result"]["simulation"]["dropped"].asNumber() > 0);
        host.Stop(); CHECK(!host.LocalPeer(1)); CHECK(host.GetScene().ToJson().dump() == edit.dump());
        CHECK(Call(host, "net.state")["result"]["transport"].asString() == "tcp");
        CHECK(Call(host, "net.spawn_local_peers", R"({"count":1})")["ok"].asBool());
        host.Step(100); CHECK(host.LocalPeer(1)); CHECK(host.Open(project, &error)); CHECK(!host.LocalPeer(1));
        RemoveAll(project);
    }
    CHECK(PlatformNetSocketsCreated() == sockets);
    std::string project = AuthorityProject("preview_authority", "tcp", true), error;
    Engine host; CHECK(host.Open(project, &error)); host.GetScene().Clear();
    CHECK(Call(host, "net.spawn_local_peers", R"({"count":1})")["ok"].asBool()); host.Step(100);
    CHECK(Call(host, "net.state")["result"]["sync"]["state"].asString() == "running");
    CHECK(host.GetScene().Pool<NetPlayer>().size() == 2);
    CHECK(host.LocalPeer(1)->GetScene().Pool<NetPlayer>().size() == 2);
    host.Stop(); RemoveAll(project);
}


TEST(NetworkSampleGamesOfflineAndMultiplayer) {
    for (const char* name : {"NetCoop", "NetDuel", "NetArena"}) {
        std::string project = JoinPath(TestSourceDir(), std::string("samples/") + name), error;
        Engine host; CHECK(host.Open(project, &error));
        Json edit = host.GetScene().ToJson();
        // The packaged player uses the same ordinary play path for offline practice.
        CHECK(Call(host, "sim.step", R"({"frames":2})")["ok"].asBool());
        CHECK(host.GetScene().Pool<NetPlayer>().size() == 1);
        CHECK(Call(host, "script.errors")["result"].size() == 0);
        for (int crystal = 0; crystal < 5; ++crystal) {
            EntityId id = host.GetScene().Pool<NetPlayer>().begin()->first;
            for (int tick = 0; tick < 150; ++tick) {
                Vec3 p = host.GetScene().Get<Transform>(id)->position;
                Vec3 t = host.GetScene().Get<Transform>(host.GetScene().FindByName("Target"))->position;
                if (Length(p - t) < 0.8f) break;
                for (const auto& key : std::vector<std::pair<const char*, bool>>{
                         {"W", p.z > t.z + 0.1f}, {"S", p.z < t.z - 0.1f},
                         {"A", p.x > t.x + 0.1f}, {"D", p.x < t.x - 0.1f}}) {
                    Json input = Json::MakeObject(); input["key"] = key.first; input["down"] = key.second;
                    CHECK(host.Call("input.key", input)["ok"].asBool());
                }
                host.Step(1);
            }
            for (const char* key : {"W", "A", "S", "D"}) {
                Json release = Json::MakeObject(); release["key"] = key; release["down"] = false;
                CHECK(host.Call("input.key", release)["ok"].asBool());
            }
            CHECK(Call(host, "input.key", R"({"key":"Space","down":true})")["ok"].asBool()); host.Step(1);
            CHECK(Call(host, "input.key", R"({"key":"Space","down":false})")["ok"].asBool()); host.Step(1);
        }
        CHECK(host.GameData()["winner"].asString() == (std::string(name) == "NetCoop" ? "team" : "1"));
        host.Stop(); CHECK(host.GetScene().ToJson().dump() == edit.dump());
        CHECK(Call(host, "net.spawn_local_peers", R"({"count":1,"seed":71})")["ok"].asBool());
        host.Step(180);
        Engine* client = host.LocalPeer(1); CHECK(client != nullptr); if (!client) continue;
        CHECK(Call(host, "net.state")["result"]["sync"]["state"].asString() == "running");
        CHECK(host.GetScene().Pool<NetPlayer>().size() == 2);
        CHECK(client->GetScene().Pool<NetPlayer>().size() == 2);
        CHECK(Call(host, "script.errors")["result"].size() == 0);
        CHECK(Call(*client, "script.errors")["result"].size() == 0);
        CHECK(!host.GetScene().Get<UIButton>(host.GetScene().FindByName("Host"))->visible);
        // Player 1 starts at (-2,2), walks to the first target and collects it.
        CHECK(Call(host, "input.touch", R"({"id":1,"x":132,"y":533,"width":1280,"height":720})")["ok"].asBool()); host.Step(30);
        CHECK(Call(host, "input.touch", R"({"id":1,"down":false})")["ok"].asBool()); host.Step(30);
        CHECK(Call(host, "input.click", R"({"x":1125,"y":603,"width":1280,"height":720})")["ok"].asBool()); host.Step(8);
        CHECK(Call(host, "input.key", R"({"key":"Space","down":false})")["ok"].asBool()); host.Step(100);
        CHECK(host.GameData()["scores"][std::string(name) == "NetCoop" ? "team" : "1"].asNumber() == 1);
        CHECK(host.GetScene().Get<Transform>(host.GetScene().FindByName("Target"))->position.x == 2);
        // A remote input resets the round; authority sends the result through UIText.text.
        CHECK(Call(*client, "input.touch", R"({"id":2,"x":1125,"y":533,"width":1280,"height":720})")["ok"].asBool()); host.Step(8);
        CHECK(Call(*client, "input.touch", R"({"id":2,"down":false})")["ok"].asBool()); host.Step(100);
        CHECK(host.GameData()["scores"].size() == 0);
        CHECK(host.GetScene().Get<UIText>(host.GetScene().FindByName("Score"))->text ==
              client->GetScene().Get<UIText>(client->GetScene().FindByName("Score"))->text);
        for (Engine* peer : {&host, client}) {
            CHECK(Call(*peer, "script.errors")["result"].size() == 0);
            CHECK(Call(*peer, "net.desync_report")["result"].size() == 0);
        }
        host.Stop(); CHECK(host.GetScene().ToJson().dump() == edit.dump());
    }
}

TEST(NetChaseThirdPersonSample) {
    std::string project = JoinPath(TestSourceDir(), "samples/NetChase"), error;
    Engine host; CHECK(host.Open(project, &error));
    Json edit = host.GetScene().ToJson();
    auto avatar = [](Engine& engine, int player) {
        for (const auto& kv : engine.GetScene().Pool<NetPlayer>()) if (kv.second.player == player) return kv.first;
        return kNullEntity;
    };
    auto position = [](Engine& engine, EntityId id) { return engine.GetScene().Get<Transform>(id)->position; };
    auto camera = [&](Engine& engine) { return position(engine, engine.GetScene().FindByName("Camera")); };
    // Offline practice: one avatar facing -Z, the camera behind and above it. The camera
    // script runs before the spawned player's, so it settles a frame after the spawn.
    CHECK(Call(host, "sim.step", R"({"frames":4})")["ok"].asBool());
    CHECK(host.GetScene().Pool<NetPlayer>().size() == 1);
    EntityId p1 = avatar(host, 1); CHECK(p1 != kNullEntity); if (p1 == kNullEntity) return;
    CHECK(std::fabs((position(host, p1).z) - (9.0f)) < 1e-3f);
    CHECK(std::fabs(camera(host).x - position(host, p1).x) < 1e-2f);
    CHECK(std::fabs((camera(host).z - position(host, p1).z) - (5.5f)) < 1e-2f);
    // The player is the toon character: every part and outline hull resolves its assets,
    // and the view-dependent graph changes the picture when only the camera moves.
    CHECK(host.GetScene().FindByName("Part_hair") != kNullEntity && host.GetScene().FindByName("Outline_coat") != kNullEntity);
    CHECK(Call(host, "asset.info", R"({"path":"materials/coat-toon.mat.json"})")["ok"].asBool());
    CHECK(Call(host, "shader.check", R"({"path":"materials/toon.shader.json"})")["ok"].asBool());
    for (const RenderItem& item : GatherRenderItems(host.GetScene(), &host.Assets())) CHECK(!item.error);
    {
        RenderTarget shot; shot.Resize(640, 360);
        host.RenderGameView(shot);
        int player = 0;
        for (EntityId id : shot.ids) player += id == p1;
        CHECK(player > 150);  // the tinted coat on the root entity is on screen
    }
    // Running into the crystal scores and moves it along the fixed route.
    CHECK(Call(host, "component.set", R"({"id":"Target","type":"Transform","values":{"position":[-3,0.7,6]}})")["ok"].asBool());
    CHECK(Call(host, "input.key", R"({"key":"W","down":true})")["ok"].asBool()); host.Step(60);
    CHECK(Call(host, "input.key", R"({"key":"W","down":false})")["ok"].asBool());
    CHECK(position(host, p1).z < 5.0f);
    CHECK(host.GameData()["scores"]["1"].asNumber() == 1);
    CHECK(position(host, host.GetScene().FindByName("Target")).x == 8);
    // Moving captured the mouse; moving it right turns the player and the camera stays behind.
    CHECK(host.Input().mouseLocked);
    CHECK(Call(host, "input.mouse", R"({"dx":450})")["ok"].asBool()); host.Step(2);
    CHECK(std::fabs(host.GetScene().Get<Transform>(p1)->rotation.y - 90.0f) < 0.1f);
    CHECK(std::fabs((camera(host).x - position(host, p1).x) + 5.5f) < 0.05f);
    // A strafes to the player's left (-Z while facing +X) without turning.
    float z = position(host, p1).z;
    CHECK(Call(host, "input.key", R"({"key":"A","down":true})")["ok"].asBool()); host.Step(30);
    CHECK(Call(host, "input.key", R"({"key":"A","down":false})")["ok"].asBool()); host.Step(2);
    CHECK(position(host, p1).z < z - 2.0f);
    CHECK(std::fabs(host.GetScene().Get<Transform>(p1)->rotation.y - 90.0f) < 0.1f);
    CHECK(Call(host, "script.errors")["result"].size() == 0);
    host.Stop(); CHECK(host.GetScene().ToJson().dump() == edit.dump());

    CHECK(Call(host, "net.spawn_local_peers", R"({"count":1,"seed":71})")["ok"].asBool());
    host.Step(180);
    Engine* client = host.LocalPeer(1); CHECK(client != nullptr); if (!client) return;
    CHECK(Call(host, "net.state")["result"]["sync"]["state"].asString() == "running");
    CHECK(host.GetScene().Pool<NetPlayer>().size() == 2);
    CHECK(client->GetScene().Pool<NetPlayer>().size() == 2);
    // Each peer's camera follows its own player.
    EntityId hostP1 = avatar(host, 1), clientP2 = avatar(*client, 2), hostP2 = avatar(host, 2);
    CHECK(hostP1 != kNullEntity && clientP2 != kNullEntity && hostP2 != kNullEntity);
    if (hostP1 == kNullEntity || clientP2 == kNullEntity || hostP2 == kNullEntity) return;
    CHECK(std::fabs((camera(host).x) - (-3.0f)) < 1e-2f);
    CHECK(std::fabs((camera(*client).x) - (-1.0f)) < 1e-2f);
    // The client's input moves its player on the server and in its own prediction.
    CHECK(Call(*client, "input.key", R"({"key":"W","down":true})")["ok"].asBool()); host.Step(60);
    CHECK(Call(*client, "input.key", R"({"key":"W","down":false})")["ok"].asBool()); host.Step(60);
    CHECK(position(host, hostP2).z < 5.5f);
    CHECK(std::fabs(position(*client, clientP2).z - position(host, hostP2).z) < 1e-3f);
    CHECK(std::fabs((camera(*client).z - position(*client, clientP2).z) - (5.5f)) < 1e-2f);
    CHECK(std::fabs((position(host, hostP1).z) - (9.0f)) < 1e-3f);
    // The match captured the mouse. The turn travels in the input stream (LookX), so the
    // server follows it and a prediction replay cannot take it back, even with latency.
    CHECK(client->Input().mouseLocked);
    CHECK(Call(*client, "input.mouse", R"({"dx":-150})")["ok"].asBool()); host.Step(30);
    CHECK(std::fabs(client->GetScene().Get<Transform>(clientP2)->rotation.y - 210.0f) < 0.1f);
    CHECK(std::fabs(host.GetScene().Get<Transform>(hostP2)->rotation.y - 210.0f) < 0.1f);
    CHECK(std::fabs(host.GetScene().Get<Transform>(hostP1)->rotation.y - 180.0f) < 0.1f);
    CHECK(Call(host, "net.simulate", R"({"latencyFrames":6})")["ok"].asBool()); host.Step(30);
    for (int i = 0; i < 6; ++i) {
        CHECK(Call(*client, "input.mouse", R"({"dx":-50})")["ok"].asBool()); host.Step(1);
        // The camera turns on the same frame, before the server has seen the input.
        CHECK(std::fabs(std::fmod(client->GetScene().Get<Transform>(client->GetScene().FindByName("Camera"))->rotation.y, 360.0f) - (30.0f + 10.0f * (i + 1))) < 0.1f);
    }
    host.Step(60);
    CHECK(std::fabs(client->GetScene().Get<Transform>(clientP2)->rotation.y - 270.0f) < 0.1f);
    CHECK(std::fabs(host.GetScene().Get<Transform>(hostP2)->rotation.y - 270.0f) < 0.1f);
    for (Engine* peer : {&host, client}) {
        CHECK(Call(*peer, "script.errors")["result"].size() == 0);
        CHECK(Call(*peer, "net.desync_report")["result"].size() == 0);
    }
    host.Stop(); CHECK(host.GetScene().ToJson().dump() == edit.dump());
}

TEST(NetworkDedicatedReadyBarrierAndCleanup) {
#ifndef __EMSCRIPTEN__
    std::string project = AuthorityProject("dedicated", "tcp", true), error;
    uint64_t live = PlatformNetSocketsLive();
    Engine server, client; CHECK(server.Open(project, &error)); CHECK(client.Open(project, &error));
    for (Engine* engine : {&server, &client}) engine->GetScene().Clear();
    CHECK(Call(server, "net.serve", R"({"port":0,"seed":71})")["ok"].asBool());
    CHECK(!server.Gpu()); CHECK(Call(server, "net.state")["result"]["dedicated"].asBool());
    CHECK(!Call(server, "net.state")["result"]["isHost"].asBool()); CHECK(Call(server, "net.state")["result"]["isServer"].asBool());
    server.Step(10); CHECK(server.Frame() == 0);
    Json join = Json::MakeObject(); join["port"] = server.Network()->State()["port"];
    CHECK(client.Call("net.join", join)["ok"].asBool()); NetworkSteps(server, client, 40);
    CHECK(server.Frame() == 0); CHECK(Call(server, "net.state")["result"]["sync"]["state"].asString() == "idle");
    CHECK(Call(client, "net.ready", R"({"ready":true})")["ok"].asBool()); NetworkSteps(server, client, 120);
    CHECK(server.Frame() > 50); CHECK(Call(server, "net.state")["result"]["sync"]["state"].asString() == "running");
    CHECK(server.GetScene().Pool<NetPlayer>().size() == 1); CHECK(client.GetScene().Pool<NetPlayer>().size() == 1);
    CHECK(server.GetScene().Pool<NetPlayer>().begin()->second.player == 2);
    CHECK(!Call(server, "net.simulate", R"({"lossPermille":10})")["ok"].asBool());
    server.Stop(); client.Stop(); CHECK(PlatformNetSocketsLive() == live); RemoveAll(project);
#endif
}

std::vector<uint8_t> MaskedWebFrame(uint8_t opcode, bool fin, const std::vector<uint8_t>& payload) {
    auto frame = WebSocketServerReader::Encode(opcode, payload.data(), payload.size());
    if (!fin) frame[0] &= 0x7f;
    size_t header = payload.size() < 126 ? 2 : payload.size() <= 65535 ? 4 : 10;
    frame[1] |= 0x80;
    frame.insert(frame.begin() + static_cast<ptrdiff_t>(header), {1, 2, 3, 4});
    for (size_t i = 0; i < payload.size(); ++i) frame[header + 4 + i] ^= static_cast<uint8_t>(1 + i % 4);
    return frame;
}
const std::string kWebUpgrade = "GET /game HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
TEST(NetworkWebSocketServerFramingAndBounds) {
    WebSocketServerReader reader; std::vector<WebSocketServerReader::Frame> frames;
    for (char c : kWebUpgrade) CHECK(reader.Feed(reinterpret_cast<const uint8_t*>(&c), 1, frames));
    CHECK(reader.Ready()); CHECK(reader.TakeUpgrade().find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    auto first = MaskedWebFrame(2, false, {1, 2}), ping = MaskedWebFrame(9, true, {9}), last = MaskedWebFrame(0, true, {3, 4});
    first.insert(first.end(), ping.begin(), ping.end()); first.insert(first.end(), last.begin(), last.end());
    for (uint8_t b : first) CHECK(reader.Feed(&b, 1, frames));
    CHECK(frames.size() == 2); CHECK(frames[0].opcode == 9); CHECK(frames[1].bytes == std::vector<uint8_t>({1,2,3,4}));
    std::vector<uint8_t> large(65536, 42); auto packet = MaskedWebFrame(2, true, large); frames.clear();
    for (size_t offset = 0; offset < packet.size(); offset += 16384)
        CHECK(reader.Feed(packet.data() + offset, std::min<size_t>(16384, packet.size() - offset), frames));
    CHECK(frames.size() == 1 && frames[0].bytes == large);
    auto invalid = WebSocketServerReader::Encode(2, large.data(), 1);
    CHECK(!reader.Feed(invalid.data(), invalid.size(), frames)); CHECK(!reader.Feed(nullptr, 0, frames));
    for (auto bad : {MaskedWebFrame(1, true, {1}), MaskedWebFrame(0, true, {1}), MaskedWebFrame(9, false, {1}), MaskedWebFrame(8, true, {1}), MaskedWebFrame(8, true, {3, 237}), MaskedWebFrame(8, true, {3, 232, 0xc0, 0x80})}) {
        WebSocketServerReader test; CHECK(test.Feed(reinterpret_cast<const uint8_t*>(kWebUpgrade.data()), kWebUpgrade.size(), frames));
        CHECK(!test.Feed(bad.data(), bad.size(), frames));
    }
    WebSocketServerReader overflow; std::vector<uint8_t> headers(8193, 'a');
    CHECK(!overflow.Feed(headers.data(), headers.size(), frames));
    WebSocketServerReader badKey; std::string request = kWebUpgrade; request.replace(request.find("dGhl"), 24, "XXXXXXXXXXXXXXXXXXXXXXXX");
    CHECK(!badKey.Feed(reinterpret_cast<const uint8_t*>(request.data()), request.size(), frames));
}

TEST(NetworkWebSocketNativeListener) {
#ifndef __EMSCRIPTEN__
    uint64_t live = PlatformNetSocketsLive();
    {
        WebSocketServerTransport server; std::string error; CHECK(server.Listen({}, &error));
        auto client = CreateNetSocket(SocketKind::Tcp, &error); CHECK(client != nullptr);
        CHECK(client->Connect(server.LocalAddress(), &error));
        std::vector<uint8_t> send(kWebUpgrade.begin(), kWebUpgrade.end());
        auto message = MaskedWebFrame(2, true, {4, 5, 6}), ping = MaskedWebFrame(9, true, {7});
        send.insert(send.end(), message.begin(), message.end()); send.insert(send.end(), ping.begin(), ping.end());
        size_t offset = 0; std::vector<uint8_t> received; PeerId peer = 0; bool delivered = false;
        for (uint64_t tick = 0; tick < 100; ++tick) {
            if (client->State() == SocketState::Open && offset < send.size()) {
                size_t n = 0; auto result = client->Send(send.data() + offset, send.size() - offset, n);
                CHECK(result == SocketIo::Progress || result == SocketIo::WouldBlock); offset += n;
            }
            std::vector<TransportEvent> events; CHECK(server.Poll(tick, events));
            for (const auto& event : events) {
                if (event.type == TransportEvent::Type::Connected) peer = event.peer;
                if (event.type == TransportEvent::Type::Data) { delivered = event.bytes == std::vector<uint8_t>({4,5,6}); CHECK(server.Send(event.peer, event.bytes.data(), event.bytes.size())); }
            }
            for (size_t i = 0; i < 4; ++i) {
                uint8_t bytes[1024]; size_t n = 0; auto result = client->Receive(bytes, sizeof(bytes), n);
                if (result == SocketIo::WouldBlock) break; CHECK(result == SocketIo::Progress); received.insert(received.end(), bytes, bytes + n);
            }
            if (delivered && received.size() > kWebUpgrade.size()) break;
        }
        CHECK(peer != 0 && delivered);
        std::string text(received.begin(), received.end()); CHECK(text.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
        auto pong = WebSocketServerReader::Encode(10, std::vector<uint8_t>{7}.data(), 1);
        auto echo = WebSocketServerReader::Encode(2, std::vector<uint8_t>{4, 5, 6}.data(), 3);
        CHECK(std::search(received.begin(), received.end(), pong.begin(), pong.end()) != received.end());
        CHECK(std::search(received.begin(), received.end(), echo.begin(), echo.end()) != received.end());
        CHECK(server.Disconnect(peer)); CHECK(!server.Send(peer, send.data(), 1));
    }
    CHECK(PlatformNetSocketsLive() == live);
#endif
}

TEST(NetworkLockstepEngineGatesAndDesync) {
    std::string project=SyncProject("sync_lockstep","loopback");
    Engine host,client; SyncPrepare(host,client,project,"loopback");
    host.Input().down.insert("W"); client.Input().down.insert("W"); client.Input().pressedThisFrame.insert("Space");
    NetworkSteps(host,client,100);
    CHECK(host.Frame()>20 && client.Frame()>20);
    CHECK(host.NetworkCall("state",Json())["sync"]["state"].asString()=="running");
    uint64_t held=host.Frame(); host.Step(20); CHECK(host.Frame() <= held+3);
    NetworkSteps(host,client,40);
    CHECK(host.NetworkCall("desync_report",Json()).size()==0);
    CHECK(Call(host,"input.player",R"({"player":2})")["result"]["down"].size()==1);
    CHECK(!Call(host,"script.eval",R"({"code":"1"})")["ok"].asBool() || !host.InPlaySession());
    host.GetScene().Pool<Transform>().begin()->second.scale.y=100;
    NetworkSteps(host,client,400);
    Json report=host.NetworkCall("desync_report",Json());
    CHECK(report.has("frame") && !report["hostScene"].asString().empty() && !report["peerScene"].asString().empty());
    CHECK(client.NetworkCall("state",Json())["sync"]["state"].asString()=="desync");
    RemoveAll(project);
}
TEST(NetworkRollbackCorrectsPredictions) {
    std::string project=SyncProject("sync_rollback","loopback",true);
    Engine host,client; SyncPrepare(host,client,project,"loopback");
    host.Input().down.insert("W"); client.Input().down.insert("W");
    host.Audio().StartCapture(); client.Audio().StartCapture();
    NetworkSteps(host,client,30);
    uint64_t before=host.Frame(); host.Step(6); CHECK(host.Frame()>before);
    client.Input().down.erase("W"); client.Input().pressedThisFrame.insert("Space");
    NetworkSteps(host,client,100);
    Json hs=host.NetworkCall("state",Json())["sync"], cs=client.NetworkCall("state",Json())["sync"];
    CHECK(hs["state"].asString()=="running" && cs["state"].asString()=="running");
    CHECK(hs["rollbacks"].asNumber()+cs["rollbacks"].asNumber()>0);
    CHECK(host.Audio().SaveState()->capture.size()==static_cast<size_t>(std::min(host.Frame(),static_cast<uint64_t>(hs["confirmed"].asNumber())))*1600);
    CHECK(client.Audio().SaveState()->capture.size()==static_cast<size_t>(std::min(client.Frame(),static_cast<uint64_t>(cs["confirmed"].asNumber())))*1600);
    CHECK(host.NetworkCall("desync_report",Json()).size()==0);
    RemoveAll(project);
}
TEST(SimulationReferenceSnapshotRestoresClosuresPhysicsAndAudio) {
    std::string project=TempProject("state_replay"),error;
    Engine e; CHECK(e.Open(project,&error)); e.GetScene().Clear();
    CHECK(Call(e,"entity.create",R"({"name":"Body","components":{"Transform":{"position":[0,3,0]},"Collider":{},"RigidBody":{}}})")["ok"].asBool());
    CHECK(Call(e,"audio.generate",R"({"path":"assets/test.wav","preset":"coin"})")["ok"].asBool());
    EntityId deleted=e.GetScene().Create("Deleted"); e.GetScene().Destroy(deleted);
    CHECK(Call(e,"sim.record_state")["ok"].asBool());
    CHECK(Call(e,"script.eval",R"J({"code":"local n=0; timer.every(0.02,function() n=n+math.random(1,5); game.set('n',n) end); audio.play('assets/test.wav', {loop=true})"})J")["ok"].asBool());
    CHECK(Call(e,"script.eval",R"J({"code":"timer.after(0.1,function() local id=scene.create('Dynamic',{Transform={position={2,2,0}},Collider2D={},RigidBody2D={}}); timer.after(0.15,function() scene.destroy(id) end) end); timer.after(0.35,function() scene.create('Persist',{Transform={position={2,2,0}},Collider2D={},RigidBody2D={}}) end)"})J")["ok"].asBool());
    e.Audio().StartCapture(); e.Step(120); CHECK(e.Scripts().Errors().empty());
    auto snapshot=e.SaveState(); Json scene=e.GetScene().ToJson(),data=e.GameData(),audio=e.Audio().State();
    e.Step(20); Json future=e.GetScene().ToJson(),futureData=e.GameData(); auto futureAudio=e.Audio().SaveState();
    e.LoadState(*snapshot);
    CHECK(e.Frame()==120 && e.GetScene().ToJson()==scene && e.GameData()==data && e.Audio().State()==audio);
    e.Step(20); CHECK(e.GetScene().ToJson()==future && e.GameData()==futureData);
    CHECK(e.Audio().SaveState()->capture==futureAudio->capture);
    CHECK(Call(e,"sim.save_state",R"({"slot":"one"})")["ok"].asBool()); e.Step(2);
    CHECK(Call(e,"sim.load_state",R"({"slot":"one"})")["ok"].asBool()); CHECK(e.Frame()==140);
    CHECK(WriteTextFile(JoinPath(project,"scripts/changed.lua"),"return {}"));
    bool refused=false; try {e.LoadState(*snapshot);} catch(const ApiError& err) {refused=err.code=="state_resource";} CHECK(refused);
    RemoveAll(project);
}

TEST(SimulationReferenceSnapshotAcrossSceneChanges) {
    std::string project=TempProject("state_scene_change"),error;
    Scene second; second.name="Second"; second.Add<Transform>(second.Create("Marker")).position={1,2,3};
    CHECK(WriteTextFile(JoinPath(project,"scenes/second.scene.json"),second.ToJson().dump()));
    Engine e; CHECK(e.Open(project,&error)); e.GetScene().Clear();
    CHECK(Call(e,"sim.record_state")["ok"].asBool());
    CHECK(Call(e,"script.eval",R"J({"code":"local n=0; timer.every(0.02,function() n=n+1; game.set('n',n) end); timer.after(0.1,function() game.loadScene('scenes/second.scene.json') end)"})J")["ok"].asBool());
    e.Step(30); auto snapshot=e.SaveState(); Json scene=e.GetScene().ToJson(),data=e.GameData();
    CHECK(e.RuntimeScene()=="scenes/second.scene.json" && e.Scripts().Errors().empty());
    e.Step(20); Json futureData=e.GameData(); e.LoadState(*snapshot);
    CHECK(e.GetScene().ToJson()==scene && e.GameData()==data && e.RuntimeScene()=="scenes/second.scene.json");
    e.Step(20); CHECK(e.GameData()==futureData); RemoveAll(project);
}
TEST(SimulationNativeSnapshotBudgetAndBranching) {
    double worst = 0;
    for (const char* sample : {"Hello", "Dungeon", "Platformer", "FPS"}) {
        Engine engine; std::string error;
        CHECK(engine.Open(TestSourceDir() + "/samples/" + sample, &error));
        engine.RecordState(); engine.Step(2000);
        auto state = engine.SaveState(); engine.Step(8);
        Json future = engine.GetScene().ToJson(), data = engine.GameData();
        for (int n = 0; n < 5; ++n) {
            auto start = std::chrono::steady_clock::now(); engine.LoadState(*state);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            worst = std::max(worst, ms); engine.Step(8);
            CHECK(engine.GetScene().ToJson() == future && engine.GameData() == data);
        }
        CHECK(engine.Scripts().Errors().empty());
    }
    std::printf("  Native snapshot worst: %.3f ms (20 restores, four samples)\n", worst);
#ifndef __EMSCRIPTEN__
    CHECK(worst < 16.67);
#endif
    std::string project = TempProject("native_long"), error; Engine engine;
    CHECK(engine.Open(project, &error)); engine.GetScene().Clear(); engine.RecordState();
    CHECK(Call(engine, "script.eval", R"J({"code":"local n=0; timer.every(0.02,function() n=n+math.random(1,3); game.set('n',n) end)"})J")["ok"].asBool());
    engine.Step(13000); auto state = engine.SaveState(); engine.Step(8); Json future = engine.GameData();
    engine.LoadState(*state); CHECK(engine.Frame() == 13000); engine.Step(8); CHECK(engine.GameData() == future);
    Engine other; bool refused = false;
    CHECK(other.Open(project, &error)); other.RecordState();
    try { other.LoadState(*state); } catch (const ApiError& e) { refused = e.code == "state_invalid"; }
    CHECK(refused); RemoveAll(project);
}

TEST(SimulationNativeSnapshotDynamicJoltCharactersAndGc) {
    std::string project=TempProject("native_dynamic_jolt"),error; Engine e;
    CHECK(e.Open(project,&error)); e.GetScene().Clear(); e.RecordState();
    CHECK(Call(e,"script.eval",R"J({"code":"local n=0; timer.every(0.05,function() n=n+1; local id=scene.create('B'..n,{Transform={position={0,3,0}},Collider={},RigidBody={}}); local ch=scene.create('C'..n,{Transform={position={3,3,0}},CharacterBody={}}); timer.after(0.12,function() scene.destroy(id);scene.destroy(ch) end); game.set('n',n); collectgarbage('collect') end)"})J")["ok"].asBool());
    e.Step(60); auto state=e.SaveState(); e.Step(30); Json future=e.GetScene().ToJson(),data=e.GameData();
    for(int n=0;n<4;++n) { e.LoadState(*state); e.Step(30); CHECK(e.GetScene().ToJson()==future && e.GameData()==data); }
    CHECK(e.Scripts().Errors().empty()); RemoveAll(project);
}

TEST(NetworkFrameSyncSeededTenThousandFrames) {
    for (int count : {2,4}) {
        LoopbackConfig faults; faults.seed=193; faults.lossPermille=80; faults.latencyFrames=1; faults.reorderFrames=2;
        faults.duplicatePermille=100;
        auto wire=std::make_shared<LoopbackNetwork>(faults);
        SessionConfig config; config.mode="lockstep"; config.transport="loopback"; config.gameId="sync-faults";
        config.sync.keys={"W"}; config.sync.hashInterval=60;
        std::vector<std::unique_ptr<Session>> sessions;
        for(int i=0;i<count;++i) sessions.push_back(std::make_unique<Session>(config,std::make_unique<LoopbackTransport>(wire,static_cast<PeerId>(i+1)),i==0,1,"Player",71,SessionTestRandom(static_cast<uint64_t>(i+300))));
        uint64_t tick=0;
        for(;tick<250;++tick) for(auto& session:sessions) session->Advance(tick);
        std::vector<std::unique_ptr<FrameSync>> sync;
        std::vector<uint64_t> totals(static_cast<size_t>(count),0);
        std::vector<uint32_t> roster;
        for(int i=0;i<count;++i) roster.push_back(static_cast<uint32_t>(i+1));
        for(int i=0;i<count;++i) {
            Session* session=sessions[static_cast<size_t>(i)].get(); CHECK(session->Players().size()==static_cast<size_t>(count));
            sync.push_back(std::make_unique<FrameSync>(config.sync,i==0,session->LocalPlayer(),99,
                [session](uint32_t player,const std::vector<uint8_t>& bytes){return session->SendSync(player,bytes);}));
        }
        std::string error; CHECK(sync[0]->Start(roster,&error));
        bool done=false;
        for(;tick<250000 && !done;++tick) {
            for(int i=0;i<count;++i) {
                auto index=static_cast<size_t>(i); sessions[index]->Advance(tick);
                for(const auto& message:sessions[index]->DrainSync()) sync[index]->Receive(message.first,message.second);
                sync[index]->Tick(tick); sync[index]->TakeStart();
                if(sync[index]->NeedsInput()) {FrameInput input; input.down=(sync[index]->Frame()/7+index)%2; sync[index]->Submit(input);}
                sync[index]->Tick(tick);
                if(sync[index]->Frame()<10000) if(auto inputs=sync[index]->Next()) {
                    for(const auto& input:*inputs) totals[index]+=input.first*(input.second.down+1);
                    sync[index]->Applied(*inputs); sync[index]->Hash(sync[index]->Frame(),totals[index],std::to_string(totals[index]));
                }
            }
            done=std::all_of(sync.begin(),sync.end(),[](const auto& item){return item->Frame()==10000;});
            if(std::any_of(sync.begin(),sync.end(),[](const auto& item){return item->State()["state"].asString()=="stopped" || item->State()["state"].asString()=="desync";})) break;
        }
        CHECK(done); for(size_t i=1;i<totals.size();++i) CHECK(totals[i]==totals[0]);
        CHECK(wire->DroppedMessages()>0); CHECK(sync[0]->Report().size()==0);
    }
}
TEST(NetworkLockstepNativeTcpUdp) {
#ifndef __EMSCRIPTEN__
    for(const char* transport:{"tcp","udp"}) {
        std::string project=SyncProject(transport,transport); Engine host,client;
        SyncPrepare(host,client,project,transport);
        host.Input().down.insert("W"); client.Input().down.insert("W");
        for (int tick=0; tick<100000 && (host.Frame()<10000 || client.Frame()<10000); ++tick) NetworkSteps(host,client,1);
        CHECK(host.Frame()>=10000 && client.Frame()>=10000);
        CHECK(host.Frame()>20 && client.Frame()>20);
        CHECK(host.NetworkCall("state",Json())["sync"]["state"].asString()=="running");
        CHECK(host.NetworkCall("desync_report",Json()).size()==0); RemoveAll(project);
    }
#endif
}

TEST(NetworkFrameSyncBoundsTimeoutAndEmptyPolicy) {
    SyncConfig config; config.keys={"W"}; config.delay=0; config.waitFrames=30;
    FrameSync host(config,true,1,1,[](uint32_t,const std::vector<uint8_t>&){return true;});
    std::string error; CHECK(host.Start({1,2},&error));
    ByteWriter ready(32); ready.WriteU8(2); ready.WriteU64(1); host.Receive(2,ready.Data()); host.Tick(1); CHECK(host.TakeStart());
    FrameInput invalid; invalid.down=2; CHECK(!host.Submit(invalid)); CHECK(host.Submit(FrameInput{}));
    ByteWriter future(128); future.WriteU8(4); future.WriteU64(1); future.WriteU64(1000000);
    for(int i=0;i<1000;++i) host.Receive(2,future.Data());
    CHECK(host.State()["rejected"].asNumber()==1000); CHECK(!host.Next());
    host.Tick(31); CHECK(host.State()["state"].asString()=="stopped" && host.TakeDropped()==std::vector<uint32_t>{2});
    config.emptyOnTimeout=true;
    FrameSync empty(config,true,1,1,[](uint32_t,const std::vector<uint8_t>&){return true;});
    CHECK(empty.Start({1,2},&error)); empty.Receive(2,ready.Data()); empty.Tick(1); empty.TakeStart();
    CHECK(empty.Submit(FrameInput{})); empty.Tick(31); auto inputs=empty.Next();
    CHECK(inputs && inputs->size()==2 && inputs->at(2)==FrameInput{} && empty.Running());
    SyncConfig parsed;
    for(const char* bad:{R"({"actions":["W","W"]})",R"({"axes":["LeftX","Invalid"]})",R"({"inputDelay":9})",R"({"rollbackFrames":0})",R"({"waitFrames":2})",R"({"dropPolicy":"ignore"})"})
        CHECK(!SyncConfig::Parse(Json::parse(bad),parsed,&error));
}
TEST(NetworkFrameSyncMismatchBarrierAndLargeWorldHash) {
    SyncConfig config; config.delay=0; config.hashInterval=1;
    std::vector<std::pair<uint32_t,std::vector<uint8_t>>> messages;
    FrameSync host(config,true,1,99,[&](uint32_t player,const std::vector<uint8_t>& bytes){messages.emplace_back(player,bytes);return true;});
    FrameSync client(config,false,2,100,[](uint32_t,const std::vector<uint8_t>&){return true;});
    std::string error; CHECK(host.Start({1,2},&error)); host.Tick(1);
    for(const auto& message:messages) client.Receive(1,message.second);
    CHECK(client.State()["state"].asString()=="stopped" && client.State()["error"].asString().find("mismatch")!=std::string::npos);
    messages.clear(); FrameSync good(config,false,2,99,[&](uint32_t player,const std::vector<uint8_t>& bytes){messages.emplace_back(player,bytes);return true;});
    // A fresh matching barrier, then a hash with a diagnostic larger than its scene budget.
    FrameSync large(config,true,1,99,[&](uint32_t player,const std::vector<uint8_t>& bytes){messages.emplace_back(player,bytes);return true;});
    CHECK(large.Start({1,2},&error)); large.Tick(1); auto begin=messages; messages.clear();
    for(const auto& message:begin) good.Receive(1,message.second); good.Tick(2); auto ready=messages; messages.clear();
    for(const auto& message:ready) large.Receive(2,message.second); large.Tick(3); auto go=messages; messages.clear();
    for(const auto& message:go) good.Receive(1,message.second); good.Tick(4); large.TakeStart(); good.TakeStart();
    large.Hash(0,5,std::string(30000,'a')); good.Hash(0,7,std::string(30000,'b')); good.Tick(5);
    auto hashes=messages; messages.clear(); for(const auto& message:hashes) large.Receive(2,message.second);
    CHECK(large.Report()["scenesOmitted"].asBool() && large.State()["state"].asString()=="desync");
    large.Tick(6); for(const auto& message:messages) good.Receive(1,message.second);
    CHECK(good.State()["state"].asString()=="desync");
}
TEST(NetworkEngineLoopbackLobbyAndRpc) {
    std::string project = NetworkProject("network_lobby", "loopback", 3), error;
    {
        Engine host, a, b;
        CHECK(host.Open(project, &error) && a.Open(project, &error) && b.Open(project, &error));
        NetworkReceiver(host); NetworkReceiver(a); NetworkReceiver(b);
        CHECK(Call(host, "net.host", R"({"room":"lobby-test","seed":42,"name":"Host"})")["ok"].asBool());
        CHECK(Call(a, "net.join", R"({"address":"lobby-test","name":"A"})")["ok"].asBool());
        CHECK(Call(b, "net.join", R"({"address":"lobby-test","name":"B"})")["ok"].asBool());
        NetworkSteps(host, a, 40, &b);
        CHECK(host.Network()->Players().size() == 3 && a.Network()->Players().size() == 3 && b.Network()->Players().size() == 3);
        CHECK(a.Network()->LocalPlayer() == 2 && b.Network()->LocalPlayer() == 3);
        CHECK(a.Network()->Seed() == 42 && b.Network()->Seed() == 42);
        CHECK(NetworkEval(a, "return joined", 0) == Json::parse("[1,2,3]"));
        CHECK(NetworkEval(a, "return net.players()", 0) == Json::parse("[1,2,3]"));
        // All Lua states are seeded at the same lobby transition, independently of wall time.
        host.Scripts().SetNetworkSeed(42); a.Scripts().SetNetworkSeed(42); b.Scripts().SetNetworkSeed(42);
        CHECK(NetworkEval(host, "return math.random()", 0) == NetworkEval(a, "return math.random()", 0));
        CHECK(Call(a, "net.ready", R"({"ready":true})")["ok"].asBool());
        CHECK(Call(b, "net.rpc", R"({"name":"ping","target":"all","args":[30]})")["ok"].asBool());
        CHECK(Call(a, "net.rpc", R"({"name":"ping","target":"server","args":[20]})")["ok"].asBool());
        NetworkSteps(host, a, 20, &b);
        CHECK(NetworkEval(host, "return received", 0) == Json::parse("[[2,20],[3,30]]"));
        CHECK(NetworkEval(a, "return received", 0) == Json::parse("[[3,30]]"));
        CHECK(NetworkEval(b, "return received", 0) == Json::parse("[[3,30]]"));
        CHECK(host.Network()->Players()[1]["ready"].asBool());
        CHECK(Call(a, "net.kick", R"({"player":3})")["ok"].asBool() == false);
        CHECK(Call(host, "net.kick", R"({"player":3})")["ok"].asBool());
        NetworkSteps(host, a, 40, &b);
        CHECK(b.Network()->Status() == "offline" && host.Network()->Players().size() == 2 && a.Network()->Players().size() == 2);
        CHECK(NetworkEval(a, "return left", 0) == Json::parse("[3]"));
        CHECK(Call(a, "net.leave", "{}")["ok"].asBool());
        NetworkSteps(host, a, 40);
        CHECK(a.Network()->Status() == "offline" && host.Network()->Players().size() == 1);
        CHECK(Call(b, "net.join", R"({"address":"lobby-test"})")["ok"].asBool());
        NetworkSteps(host, b, 40);
        CHECK(b.Network()->LocalPlayer() == 4);  // ids never reused within a host incarnation
        host.Stop();
        CHECK(host.Network() == nullptr);
        CHECK(!Call(a, "net.join", R"({"address":"lobby-test"})")["ok"].asBool());
    }
    CHECK(RemoveAll(project));
}

TEST(NetworkSessionConfigValidation) {
    SessionConfig config; std::string error;
    CHECK(SessionConfig::Parse(Json::parse(R"({"network":{"mode":"none","port":"ignored"}})"), config, &error));
    for (const char* json : {R"({"network":true})", R"({"network":{"mode":"invalid"}})",
         R"({"network":{"mode":"lockstep","tickRate":30}})", R"({"network":{"mode":"lockstep","maxPlayers":65}})",
         R"({"network":{"mode":"lockstep","port":-1}})", R"({"network":{"mode":"lockstep","bind":"0.00.0.0"}})",
         R"({"network":{"mode":"lockstep","transport":"bad"}})"})
        CHECK(!SessionConfig::Parse(Json::parse(json), config, &error) && !error.empty());
    CHECK(SessionConfig::Parse(Json::parse(R"({"name":"Game","network":{"mode":"authoritative","bind":"0.0.0.0"}})"), config, &error));
    CHECK(config.bind == "0.0.0.0" && config.tickRate == 60);
}

class SessionRecordedWire final : public ITransport {
public:
    SessionRecordedWire(std::shared_ptr<LoopbackNetwork> network, PeerId peer) : wire_(std::move(network), peer) {}
    std::vector<std::vector<uint8_t>> sent;
    bool Send(PeerId peer, const uint8_t* bytes, size_t size) override {
        if (sent.size() < 1024) sent.emplace_back(bytes, bytes + size);
        return wire_.Send(peer, bytes, size);
    }
    bool Poll(uint64_t frame, std::vector<TransportEvent>& events) override { return wire_.Poll(frame, events); }
private:
    LoopbackTransport wire_;
};
Session::Random SessionTestRandom(uint64_t first) {
    auto counter = std::make_shared<uint64_t>(first);
    return [counter](uint8_t* bytes, size_t size) {
        if (size != 8) return false;
        uint64_t value = ++*counter;
        for (size_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
        return true;
    };
}
TEST(NetworkSessionHandshakeLossVersionAndTimeout) {
    auto run = [] {
        LoopbackConfig faults; faults.seed = 91; faults.lossPermille = 150;
        faults.duplicatePermille = 200; faults.reorderFrames = 3; faults.latencyFrames = 1;
        auto wire = std::make_shared<LoopbackNetwork>(faults);
        SessionConfig config; config.mode = "lockstep"; config.transport = "loopback"; config.gameId = "game";
        Session host(config, std::make_unique<LoopbackTransport>(wire, 1), true, 1, "Host", 55, SessionTestRandom(100));
        Session client(config, std::make_unique<LoopbackTransport>(wire, 2), false, 1, "Client", 0, SessionTestRandom(200));
        std::vector<std::string> trace;
        bool sent = false;
        for (uint64_t frame = 0; frame < 500; ++frame) {
            CHECK(host.Advance(frame) && client.Advance(frame));
            for (auto* session : {&host, &client}) for (const auto& event : session->DrainEvents()) {
                trace.push_back(std::to_string(frame) + ":" + std::to_string(event.player) + ":" + event.name);
            }
            if (!sent && host.Players().size() == 2 && client.Players().size() == 2) {
                std::string error;
                CHECK(client.Rpc("server", "ping", Json::parse("[5]"), &error)); sent = true;
            }
        }
        CHECK(sent && client.Connected() && client.Seed() == 55);
        CHECK(std::count_if(trace.begin(), trace.end(), [](const std::string& row) { return row.find(":ping") != std::string::npos; }) == 1);
        CHECK(!client.Advance(10));
        return trace;
    };
    CHECK(run() == run());
    auto wire = std::make_shared<LoopbackNetwork>();
    SessionConfig config; config.mode = "lockstep"; config.transport = "loopback"; config.gameId = "game";
    Session host(config, std::make_unique<LoopbackTransport>(wire, 1), true, 1, "Host", 1, SessionTestRandom(100));
    ++config.version;
    Session bad(config, std::make_unique<LoopbackTransport>(wire, 2), false, 1, "Old", 0, SessionTestRandom(200));
    for (uint64_t frame = 0; frame < 20; ++frame) { CHECK(host.Advance(frame) && bad.Advance(frame)); }
    CHECK(bad.Status() == "error" && bad.State()["error"].asString().find("mismatch") != std::string::npos);
    CHECK(host.Players().size() == 1);
    Session lonely(config, std::make_unique<LoopbackTransport>(wire, 3), false, 9, "Alone", 0, SessionTestRandom(300));
    for (uint64_t frame = 0; frame < 310; ++frame) CHECK(lonely.Advance(frame));
    CHECK(lonely.Status() == "error" && lonely.LocalPlayer() == 0);
    Session noEntropy(config, std::make_unique<LoopbackTransport>(wire, 4), false, 1, "NoEntropy", 0,
                      [](uint8_t*, size_t) { return false; });
    CHECK(noEntropy.Status() == "error" && noEntropy.State()["error"].asString().find("entropy") != std::string::npos);
    CHECK(noEntropy.Advance(0));
    auto limitedWire = std::make_shared<LoopbackNetwork>();
    config.version = 1; config.maxPlayers = 2;
    Session limited(config, std::make_unique<LoopbackTransport>(limitedWire, 1), true, 1, "Host", 1, SessionTestRandom(400));
    Session first(config, std::make_unique<LoopbackTransport>(limitedWire, 2), false, 1, "First", 0, SessionTestRandom(500));
    Session full(config, std::make_unique<LoopbackTransport>(limitedWire, 3), false, 1, "Full", 0, SessionTestRandom(600));
    for (uint64_t frame = 0; frame < 310; ++frame) { limited.Advance(frame); first.Advance(frame); full.Advance(frame); }
    CHECK(first.Connected() && limited.Players().size() == 2 && full.Status() == "error");
}

TEST(NetworkSessionCookieReplayBoundsAndReconnect) {
    auto wire = std::make_shared<LoopbackNetwork>();
    SessionConfig config; config.mode = "lockstep"; config.transport = "loopback"; config.gameId = "game";
    Session host(config, std::make_unique<LoopbackTransport>(wire, 1), true, 1, "Host", 1, SessionTestRandom(100));
    std::vector<uint8_t> oldEnvelope;
    {
        auto transport = std::make_unique<SessionRecordedWire>(wire, 2); auto* recorded = transport.get();
        Session client(config, std::move(transport), false, 1, "Client", 0, SessionTestRandom(200));
        for (uint64_t frame = 0; frame < 30; ++frame) { host.Advance(frame); client.Advance(frame); host.DrainEvents(); client.DrainEvents(); }
        std::string error;
        CHECK(client.Rpc("server", "old", Json::MakeArray(), &error));
        for (uint64_t frame = 30; frame < 50; ++frame) { host.Advance(frame); client.Advance(frame); host.DrainEvents(); client.DrainEvents(); }
        for (const auto& bytes : recorded->sent) {
            ByteReader reader(bytes.data(), bytes.size()); uint32_t magic = 0;
            if (reader.ReadU32(magic) && magic == 0x5344454f) { oldEnvelope = bytes; break; }
        }
        CHECK(!oldEnvelope.empty());
        client.Leave();
        for (uint64_t frame = 50; frame < 90; ++frame) { host.Advance(frame); client.Advance(frame); host.DrainEvents(); client.DrainEvents(); }
        CHECK(host.Players().size() == 1);
    }
    auto transport = std::make_unique<SessionRecordedWire>(wire, 2); auto* recorded = transport.get();
    Session client(config, std::move(transport), false, 1, "New", 0, SessionTestRandom(300));
    for (uint64_t frame = 90; frame < 120; ++frame) { host.Advance(frame); client.Advance(frame); host.DrainEvents(); client.DrainEvents(); }
    CHECK(client.LocalPlayer() == 3 && host.Players().size() == 2);
    uint64_t rejected = static_cast<uint64_t>(host.Stats()["rejected"].asNumber());
    CHECK(recorded->Send(1, oldEnvelope.data(), oldEnvelope.size()));
    std::vector<uint8_t> malicious(300, 0);
    for (int i = 0; i < 200; ++i) CHECK(recorded->Send(1, malicious.data(), malicious.size()));
    for (uint64_t frame = 120; frame < 125; ++frame) { host.Advance(frame); client.Advance(frame); }
    CHECK(host.Stats()["rejected"].asNumber() >= static_cast<double>(rejected + 201));
    CHECK(host.Players().size() == 2 && client.Connected());
    std::string error;
    CHECK(client.Rpc("server", "new", Json::parse("[9]"), &error));
    int received = 0;
    for (uint64_t frame = 125; frame < 145; ++frame) {
        host.Advance(frame); client.Advance(frame);
        for (const auto& event : host.DrainEvents()) if (event.type == SessionEvent::Type::Rpc) { ++received; CHECK(event.player == 3 && event.name == "new"); }
    }
    CHECK(received == 1);
    for (int i = 0; i < 300; ++i) host.Rpc("1", "queue", Json::MakeArray(), &error);
    CHECK(host.DrainEvents().size() <= 256);
}

#ifndef __EMSCRIPTEN__
TEST(NetworkEngineNativeSessionTcpUdp) {
    for (const char* transport : {"tcp", "udp"}) {
        std::string project = NetworkProject(transport, transport), error;
        uint64_t sockets = PlatformNetSocketsLive();
        {
            Engine host, client;
            CHECK(host.Open(project, &error) && client.Open(project, &error));
            NetworkReceiver(host); NetworkReceiver(client);
            Json hosted = Call(host, "net.host", R"({"seed":99})");
            CHECK(hosted["ok"].asBool());
            Json args = Json::MakeObject(); args["port"] = hosted["result"]["port"];
            CHECK(client.Call("net.join", args)["ok"].asBool());
            NetworkSteps(host, client, 180);
            CHECK(client.Network()->Connected() && client.Network()->Seed() == 99 && host.Network()->Players().size() == 2);
            CHECK(Call(client, "net.rpc", R"({"target":"server","name":"ping","args":[123]})")["ok"].asBool());
            NetworkSteps(host, client, 80);
            CHECK(NetworkEval(host, "return received", 0) == Json::parse("[[2,123]]"));
            if (std::string(transport) == "udp") CHECK(host.Network()->Stats()["peers"][0]["route"].asString() == "udp");
            CHECK(host.Scripts().Errors().empty() && client.Scripts().Errors().empty());
            host.Stop(); NetworkSteps(host, client, 40);
            CHECK(client.Network()->Status() == "error");
            client.Stop();
        }
        CHECK(PlatformNetSocketsLive() == sockets);
        CHECK(RemoveAll(project));
    }
}
#endif

bool Near(const Vec3& a, const Vec3& b, float eps) {
    return std::fabs(a.x - b.x) < eps && std::fabs(a.y - b.y) < eps && std::fabs(a.z - b.z) < eps;
}

TEST(SaveSlotsPersistAndRecover) {
    const std::string project = TempProject("save_slots");
    const std::string directory = JoinPath(project, "saves");
    std::string error;
    {
        Engine e;
        CHECK(e.Open(project, &error));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.set", R"J({"key":"score","value":17})J")["ok"].asBool());
        CHECK(Call(e, "save.set", R"J({"key":"score","value":{"n":9,"items":[true,null,"한글"]},"slot":"second"})J")["ok"].asBool());
        CHECK(!FileExists(directory));  // lazy I/O; no directory before flush
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
        CHECK(Call(e, "save.flush", R"J({"slot":"second"})J")["ok"].asBool());
        CHECK(!FileExists(JoinPath(directory, "slot-default.json.tmp")));
        CHECK(Call(e, "save.set", R"J({"key":"score","value":42})J")["ok"].asBool());
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());  // replacement, not only first write
        Json deep = 7;
        for (int depth = 0; depth < 32; ++depth) { Json array = Json::MakeArray(); array.push(deep); deep = std::move(array); }
        Json args = Json::MakeObject(); args["key"] = "deep"; args["value"] = deep;
        CHECK(e.Call("save.set", args)["ok"].asBool());
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
        CHECK(e.UndoDepth() == 0);
    }
    {
        Engine e;
        CHECK(e.Open(project, &error));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"]["score"].asInt() == 42);
        CHECK(Call(e, "save.state", "{}")["result"]["data"]["deep"].isArray());
        CHECK(Call(e, "save.state", R"J({"slot":"second"})J")["result"]["data"]["score"]["n"].asInt() == 9);
        CHECK(!Call(e, "save.state", R"J({"slot":"../escape"})J")["ok"].asBool());
        CHECK(!Call(e, "save.set", R"J({"key":"","value":1})J")["ok"].asBool());
        Call(e, "save.clear", R"J({"key":"score"})J");
        Call(e, "save.flush", "{}");
        e.Saves().Configure(directory);
        CHECK(!Call(e, "save.state", "{}")["result"]["data"].has("score"));
        CHECK(WriteTextFile(JoinPath(directory, "slot-default.json"), "{broken"));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"].size() == 0);
        CHECK(WriteTextFile(JoinPath(directory, "slot-default.json"), "[1,2]"));
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"].size() == 0);
        CHECK(Call(e, "save.set", R"J({"key":"recovered","value":true})J")["ok"].asBool());
        CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
        e.Saves().Configure(directory);
        CHECK(Call(e, "save.state", "{}")["result"]["data"]["recovered"].asBool());
    }
    CHECK(RemoveAll(project));
}

TEST(SaveMemoryAndLuaValidation) {
    const std::string project = TempProject("save_memory");
    Engine e;
    std::string error;
    CHECK(e.Open(project, &error));
    const auto before = ListFiles(project, "", true);
    CHECK(Call(e, "save.state", "{}")["result"]["mode"].asString() == "memory");
    CHECK(Call(e, "script.eval", R"J({"code":"save.set('score', 5) save.set('other', {1, 'two', false}, 'second') save.set('null', nil) save.flush() return save.get('score', 0)"})J")["result"]["value"].asInt() == 5);
    CHECK(Call(e, "script.eval", R"J({"code":"return save.get('absent', 8)"})J")["result"]["value"].asInt() == 8);
    CHECK(Call(e, "script.eval", R"J({"code":"return save.get('null', 8)"})J")["result"]["value"].isNull());
    CHECK(Call(e, "save.state", R"J({"slot":"second"})J")["result"]["data"]["other"].size() == 3);
    for (const char* code : {"save.set('bad', function() end)", "save.set('bad', 0/0)",
                             "save.get('', 0)", "save.set('bad' .. string.char(0), 1)",
                             "save.set('bad', 1, 'default' .. string.char(0))",
                             "local t = {} t.self = t save.set('bad', t)",
                             "save.set('bad', {[1] = 1, named = 2})", "save.set('bad', {[3] = 1})"}) {
        Json args = Json::MakeObject(); args["code"] = code;
        CHECK(!e.Call("script.eval", args)["ok"].asBool());
    }
    CHECK(!Call(e, "save.state", "{}")["result"]["data"].has("bad"));
    e.Step(1);
    e.Stop();
    CHECK(Call(e, "save.state", "{}")["result"]["data"]["score"].asInt() == 5);
    CHECK(Call(e, "script.eval", R"J({"code":"save.delete('score') save.flush()"})J")["ok"].asBool());
    CHECK(!Call(e, "save.state", "{}")["result"]["data"].has("score"));
    CHECK(ListFiles(project, "", true) == before);
    CHECK(RemoveAll(project));
}

TEST(SaveFailedFlushPreservesData) {
    const std::string project = TempProject("save_failure");
    const std::string directory = JoinPath(project, "saves");
    Engine e;
    e.Saves().Configure(directory);
    Call(e, "save.set", R"J({"key":"score","value":7})J");
    CHECK(Call(e, "save.flush", "{}")["ok"].asBool());
    std::string original;
    CHECK(ReadTextFile(JoinPath(directory, "slot-default.json"), original));
    // A directory where the temporary file belongs forces a deterministic write failure.
    CHECK(CreateDirectories(JoinPath(directory, "slot-default.json.tmp")));
    Call(e, "save.set", R"J({"key":"score","value":8})J");
    CHECK(Call(e, "save.flush", "{}")["error"]["code"].asString() == "save_write_failed");
    std::string after;
    CHECK(ReadTextFile(JoinPath(directory, "slot-default.json"), after));
    CHECK(after == original);
    CHECK(Call(e, "save.state", "{}")["result"]["dirty"].asBool());
    CHECK(RemoveAll(project));
}

TEST(TemplateRotatorScript) {
    // Spinning is project code: scripts/rotator.lua of the template.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("rotator"), &err));
    Call(e, "entity.create", R"J({"name": "Lua", "components": {"Script": {"path": "scripts/rotator.lua", "params": {"degreesPerSecond": [20, 60, 0]}}}})J");
    Call(e, "sim.step", R"J({"frames": 400})J");
    Scene& s = e.GetScene();
    Vec3 r = s.Get<Transform>(s.FindByName("Lua"))->rotation;
    CHECK(Near(r, Vec3(20.0f * 400 / 60, 60.0f * 400 / 60 - 360, 0), 1e-2f));  // wrapped to 0..360
    CHECK(e.Scripts().Errors().empty());
}

TEST(TemplatePlayerScript) {
    // Without a CharacterBody scripts/player_controller.lua moves the Transform and lands at half the height.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("player"), &err));
    Call(e, "entity.create", R"J({"name": "Lua", "components": {"Transform": {"position": [10, 0.5, 0]}, "Script": {"path": "scripts/player_controller.lua"}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "input.key", R"J({"key": "D"})J");
    Call(e, "input.key", R"J({"key": "Space"})J");
    Call(e, "sim.step", R"J({"frames": 20})J");
    Scene& s = e.GetScene();
    CHECK(s.Get<Transform>(s.FindByName("Lua"))->position.y > 0.8f);  // jumping
    Call(e, "input.clear", "{}");
    Call(e, "sim.step", R"J({"frames": 60})J");
    Vec3 p = s.Get<Transform>(s.FindByName("Lua"))->position - Vec3(10, 0, 0);
    const float moved = 4.0f * 20 / 60 * 0.70710678f;  // 20 frames at 4 m/s, diagonally
    CHECK(Near(p, Vec3(moved, 0.5f, -moved), 1e-3f));
    CHECK(e.Scripts().Errors().empty());
}

TEST(RemovedComponentsStillLoad) {
    // Rotator, Velocity and PlayerController moved into project scripts: old files load without them
    // (with a warning), and the API no longer knows the types.
    Scene s;
    std::string err;
    Json old = Json::parse(R"J({"format": "ownengine.scene", "version": 1, "name": "Old", "entities": [
      {"id": 1, "name": "Cube", "components": {"Transform": {"position": [1, 2, 3]}, "Rotator": {"degreesPerSecond": [0, 90, 0]},
        "Velocity": {"linear": [1, 0, 0]}, "PlayerController": {"speed": 3}, "MeshRenderer": {}}}]})J");
    CHECK(s.FromJson(old, &err));
    EntityId cube = s.FindByName("Cube");
    CHECK(cube != kNullEntity && s.Get<MeshRenderer>(cube) && Near(s.Get<Transform>(cube)->position, Vec3(1, 2, 3), 1e-6f));
    CHECK(s.ToJson().dump().find("Rotator") == std::string::npos);
    Json typo = Json::parse(R"J({"format": "ownengine.scene", "version": 1, "name": "Typo", "entities": [
      {"id": 1, "name": "Cube", "components": {"Rotater": {}}}]})J");
    CHECK(!s.FromJson(typo, &err) && err.find("unknown component type 'Rotater'") != std::string::npos);
    Engine e;
    CHECK(e.Open(TempProject("removed_components"), &err));
    CHECK(Call(e, "component.add", R"J({"id": "Player", "type": "Rotator"})J")["error"]["code"].asString() == "unknown_component");
    for (const Json& type : Call(e, "component.types", "{}")["result"].items()) {
        const std::string name = type["name"].asString();
        CHECK(name != "Rotator" && name != "Velocity" && name != "PlayerController");
    }
}

TEST(ScriptEvalAndSandbox) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("eval"), &err));
    Json r = Call(e, "script.eval", R"J({"code": "1 + 2"})J");
    CHECK(r["result"]["value"].asInt() == 3);
    r = Call(e, "script.eval", R"J({"code": "print('hi', 5) return scene.find('Player')"})J");
    CHECK(r["result"]["output"][0].asString() == "hi\t5");
    CHECK(r["result"]["value"].asInt() == static_cast<int>(e.GetScene().FindByName("Player")));
    r = Call(e, "script.eval", R"J({"code": "self:get('Transform').position.y", "entity": "Player"})J");
    CHECK(std::fabs(r["result"]["value"].asNumber() - 0.5) < 1e-6);
    r = Call(e, "script.eval", R"J({"code": "io == nil and os == nil and load == nil and debug == nil"})J");
    CHECK(r["result"]["value"].asBool());
    r = Call(e, "script.eval", R"J({"code": "local x = "})J");
    CHECK(r["error"]["code"].asString() == "script_syntax_error");
    r = Call(e, "script.eval", R"J({"code": "while true do end"})J");
    CHECK(r["error"]["message"].asString().find("instruction budget") != std::string::npos);
    r = Call(e, "script.eval", R"J({"code": "scene.set('Player', 'Transform', {position = {y = 3}})"})J");
    CHECK(r["ok"].asBool());
    const Transform* t = e.GetScene().Get<Transform>(e.GetScene().FindByName("Player"));
    CHECK(t->position.y == 3.0f && t->position.z == 2.0f);  // partial update keeps x/z
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());    // eval edits are undoable
    CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Player"))->position.y == 0.5f);
}

TEST(GamepadDeviceLifecycle) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("gamepad_device"), &err));
    GamepadInput device;
    Call(e, "input.axis", R"J({"name":"LeftX","value":0.7})J");
    Call(e, "input.key", R"J({"key":"GamepadB","down":true})J");
    Call(e, "input.key", R"J({"key":"W","down":true})J");
    device.Apply(e.Input(), GamepadSnapshot{});
    CHECK(e.Input().axes["LeftX"] == 0.7f && e.Input().IsDown("GamepadB") && e.Input().IsDown("W"));
    GamepadSnapshot snapshot;
    snapshot.connected = true;
    snapshot.device = 0;
    snapshot.axes = {0.575f, 1, -1, 0, 0.5f, 1};
    snapshot.buttons.fill(true);
    device.Apply(e.Input(), snapshot);
    for (const char* name : GamepadInput::kButtonNames) CHECK(e.Input().IsDown(name));
    CHECK(std::fabs(e.Input().Axis("LeftX") - 0.5f) < 1e-6f);
    e.Input().pressedThisFrame.clear();
    device.Apply(e.Input(), snapshot);
    CHECK(e.Input().pressedThisFrame.empty());  // holds are not repeated edges
    snapshot.device = 1;
    device.Apply(e.Input(), snapshot);
    CHECK(e.Input().pressedThisFrame.count("GamepadA") == 1);  // another controller begins a new press
    snapshot.buttons.fill(false);
    device.Apply(e.Input(), snapshot);
    for (const char* name : GamepadInput::kButtonNames) CHECK(!e.Input().IsDown(name));
    CHECK(e.Input().pressedThisFrame.empty());  // a release cancels an unconsumed edge
    snapshot.buttons[0] = true;
    device.Apply(e.Input(), snapshot);
    device.Reset(e.Input());  // focus loss
    CHECK(!e.Input().IsDown("GamepadA") && e.Input().pressedThisFrame.empty() && e.Input().IsDown("W"));
    for (const char* name : InputState::kAxisNames) CHECK(e.Input().Axis(name) == 0);
    snapshot.axes[0] = 2;
    snapshot.axes[4] = -1;
    snapshot.axes[5] = std::nanf("");
    device.Apply(e.Input(), snapshot);
    CHECK(e.Input().Axis("LeftX") == 1 && e.Input().Axis("LT") == 0 && e.Input().Axis("RT") == 0);
    device.Apply(e.Input(), GamepadSnapshot{});  // disconnect
    CHECK(!e.Input().IsDown("GamepadA") && e.Input().Axis("LeftX") == 0 && e.Input().IsDown("W"));
    Call(e, "input.axis", R"J({"name":"RightX","value":0.9})J");
    device.Apply(e.Input(), GamepadSnapshot{});
    CHECK(e.Input().axes["RightX"] == 0.9f);  // inactive polling still preserves tool input
}

TEST(GamepadAxesAndButtons) {
    auto run = [] {
        Engine e;
        std::string err;
        CHECK(e.Open(TempProject("gamepad_axes"), &err));
        Call(e, "scene.new", R"J({"empty":true})J");
        Call(e, "script.write", R"J({"path":"scripts/pad.lua","source":"local Pad={}\nfunction Pad:onUpdate(dt)\n local p=self:position()\n self:setPosition(p.x+input.axis('LeftX')*6*dt,p.y,0)\n self.presses=(self.presses or 0)+(input.pressed('GamepadA') and 1 or 0)\nend\nreturn Pad\n"})J");
        Call(e, "entity.create", R"J({"name":"Actor","components":{"Transform":{},"Script":{"path":"scripts/pad.lua"}}})J");
        Json initial = Call(e, "sim.state", "{}")["result"];
        CHECK(initial["axes"].size() == 6 && initial["rawAxes"].size() == 6);
        CHECK(initial["axes"]["LT"].asFloat() == 0);
        size_t undo = e.UndoDepth();
        Json state = Call(e, "input.axis", R"J({"name":"LeftX","value":0.15})J")["result"];
        CHECK(state["axes"]["LeftX"].asFloat() == 0 && state["rawAxes"]["LeftX"].asFloat() == 0.15f);
        state = Call(e, "input.axis", R"J({"name":"LeftX","value":0.575})J")["result"];
        CHECK(std::fabs(state["axes"]["LeftX"].asFloat() - 0.5f) < 1e-6f);
        Call(e, "input.axis", R"J({"name":"LeftY","value":-1})J");
        Call(e, "input.axis", R"J({"name":"RightX","value":1})J");
        Call(e, "input.axis", R"J({"name":"RightY","value":-0.575})J");
        Call(e, "input.axis", R"J({"name":"LT","value":0.15})J");
        Call(e, "input.axis", R"J({"name":"RT","value":1})J");
        CHECK(e.Input().Axis("LeftY") == -1 && e.Input().Axis("RightX") == 1 && e.Input().Axis("RT") == 1);
        CHECK(std::fabs(e.Input().Axis("RightY") + 0.5f) < 1e-6f && e.Input().Axis("LT") == 0);
        for (const char* invalid : {R"J({"name":"Unknown","value":0})J", R"J({"name":"LeftX","value":1.01})J",
                                    R"J({"name":"LeftX","value":-1.01})J", R"J({"name":"LT","value":-0.01})J",
                                    R"J({"name":"RT","value":1e100})J"}) {
            CHECK(!Call(e, "input.axis", invalid)["ok"].asBool());
        }
        CHECK(std::fabs(e.Input().Axis("LeftX") - 0.5f) < 1e-6f);  // invalid calls preserve the previous value
        Call(e, "input.key", R"J({"key":"GamepadA","down":true})J");
        CHECK(e.UndoDepth() == undo);
        Call(e, "sim.step", R"J({"frames":60})J");
        EntityId actor = e.GetScene().FindByName("Actor");
        CHECK(std::fabs(e.GetScene().Get<Transform>(actor)->position.x - 3.0f) < 1e-4f);
        Json eval = Call(e, "script.eval", R"J({"entity":"Actor","code":"return {self.presses,input.down('GamepadA'),input.axis('RT'),pcall(input.axis,'Unknown')}"})J")["result"]["value"];
        CHECK(eval[0].asInt() == 1 && eval[1].asBool() && eval[2].asFloat() == 1 && !eval[3].asBool());
        Call(e, "input.key", R"J({"key":"GamepadA","down":true})J");
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(Call(e, "script.eval", R"J({"entity":"Actor","code":"return self.presses"})J")["result"]["value"].asInt() == 1);
        Call(e, "input.key", R"J({"key":"GamepadA","down":false})J");
        Call(e, "input.key", R"J({"key":"GamepadA","down":true})J");
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(Call(e, "script.eval", R"J({"entity":"Actor","code":"return self.presses"})J")["result"]["value"].asInt() == 2);
        CHECK(e.Scripts().Errors().empty());
        std::string result = e.GetScene().ToJson().dump();
        Call(e, "sim.stop", "{}");
        CHECK(e.Input().axes.empty() && !e.Input().IsDown("GamepadA") && e.Input().Axis("LeftX") == 0);
        return result;
    };
    std::string first = run();
    CHECK(first == run());
}

TEST(MouseLookInput) {
    // Relative mouse motion reaches scripts for exactly one step; the lock flag
    // is set by scripts, visible to tools and released by input.mouse / sim.stop.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("mouselook"), &err));
    Call(e, "script.write", R"J({"path": "scripts/look.lua", "source": "local M = {}\nfunction M:onStart() self.yaw = 0 input.lockMouse() end\nfunction M:onUpdate(dt) local dx, dy = input.mouseDelta() self.yaw = self.yaw - dx * 0.1 self.lastDy = dy end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Looker", "components": {"Script": {"path": "scripts/look.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(Call(e, "sim.state", "{}")["result"]["mouseLocked"].asBool());
    Call(e, "input.mouse", R"J({"dx": 30, "dy": -4})J");
    Call(e, "input.mouse", R"J({"dx": 20})J");  // accumulates until the next step
    Call(e, "sim.step", R"J({"frames": 3})J");
    Json r = Call(e, "script.eval", R"J({"code": "{self.yaw, self.lastDy}", "entity": "Looker"})J");
    CHECK(std::fabs(r["result"]["value"][0].asNumber() + 5.0) < 1e-6);  // 50 px * 0.1, applied once
    CHECK(r["result"]["value"][1].asNumber() == 0.0);                  // cleared after the step
    CHECK(Call(e, "input.mouse", R"J({"locked": false})J")["result"]["mouseLocked"].asBool() == false);
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(!Call(e, "script.eval", R"J({"code": "input.mouseLocked()"})J")["result"]["value"].asBool());
    Call(e, "input.mouse", R"J({"locked": true})J");
    Call(e, "sim.stop", "{}");
    CHECK(!Call(e, "sim.state", "{}")["result"]["mouseLocked"].asBool());
}

TEST(Sprites2DAndTilemaps) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("sprites2d"), &err));
    // 4x2 sheet with two 2x2 frames: frame 0 has only its top-left texel opaque (red),
    // frame 1 is fully opaque green.
    Image sheet;
    sheet.width = 4;
    sheet.height = 2;
    sheet.rgba.assign(4 * 2 * 4, 0);
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t a) {
        uint8_t* p = &sheet.rgba[static_cast<size_t>((y * 4 + x) * 4)];
        p[0] = r; p[1] = g; p[2] = 0; p[3] = a;
    };
    px(0, 0, 255, 0, 255);
    for (int y = 0; y < 2; ++y)
        for (int x = 2; x < 4; ++x) px(x, y, 0, 255, 255);
    CHECK(WritePng(JoinPath(e.ProjectDir(), "sheet.png"), sheet, true));

    Call(e, "scene.new", "{}");
    Call(e, "component.set", R"J({"id": "Main Camera", "type": "Transform", "values": {"position": [0, 0, 10], "rotation": [0, 0, 0]}})J");
    Call(e, "component.set", R"J({"id": "Main Camera", "type": "Camera", "values": {"projection": "orthographic", "orthoSize": 1}})J");
    Call(e, "entity.create", R"J({"name": "S", "components": {"Transform": {}, "Sprite": {"texture": "sheet.png", "columns": 2, "pixelsPerUnit": 2}}})J");
    EntityId s = e.GetScene().FindByName("S");
    // The 1x1 unit sprite covers pixels 16..48 of a 64x64 view; picking uses the same cut-out rules as drawing.
    auto pick = [&](int x, int y) {
        Json a = Json::MakeObject();
        a["x"] = x; a["y"] = y; a["width"] = 64; a["height"] = 64;
        return static_cast<EntityId>(e.Call("render.pick", a)["result"]["id"].asInt());
    };
    CHECK(pick(20, 20) == s);           // opaque texel of frame 0
    CHECK(pick(44, 44) == kNullEntity); // transparent texel: cut out
    CHECK(pick(44, 20) == kNullEntity);
    Call(e, "component.set", R"J({"id": "S", "type": "Sprite", "values": {"flipX": true}})J");
    CHECK(pick(44, 20) == s && pick(20, 20) == kNullEntity);
    Call(e, "component.set", R"J({"id": "S", "type": "Sprite", "values": {"frame": 1, "flipX": false}})J");
    CHECK(pick(44, 44) == s);

    // SpriteAnimation drives Sprite.frame on simulated time; non-looping clips stop at the end.
    Call(e, "component.add", R"J({"id": "S", "type": "SpriteAnimation", "values": {"clip": "a", "clips": {"a": {"frames": [0, 1], "fps": 10, "loop": false}}}})J");
    Call(e, "sim.step", R"J({"frames": 3})J");
    CHECK(e.GetScene().Get<Sprite>(s)->frame == 0);
    Call(e, "sim.step", R"J({"frames": 4})J");
    CHECK(e.GetScene().Get<Sprite>(s)->frame == 1);
    CHECK(e.GetScene().Get<SpriteAnimation>(s)->finished);
    Call(e, "sim.stop", "{}");

    // Tilemap: rows go down from the entity origin; solid cells collide (a 2D character lands on them).
    Call(e, "entity.create", R"J({"name": "Map", "components": {"Transform": {}, "Tilemap": {"tileset": "sheet.png", "columns": 2,
        "map": ["....", "#..#", "####"], "legend": {"#": 1}, "solid": "#"}}})J");
    Call(e, "entity.create", R"J({"name": "Hero", "components": {"Transform": {"position": [1.5, 0, 0]},
        "CharacterBody": {"shape": "sphere", "radius": 0.3, "plane2D": true, "velocity": [0, 0, 5]}}})J");
    Call(e, "sim.step", R"J({"frames": 90})J");
    const Transform* hero = e.GetScene().Get<Transform>(e.GetScene().FindByName("Hero"));
    CHECK(std::fabs(hero->position.y + 1.7f) < 0.05f);  // resting on row 2 (top at y = -2)
    CHECK(std::fabs(hero->position.x - 1.5f) < 0.01f);
    CHECK(hero->position.z == 0.0f);                    // plane2D ignores velocity.z
    CHECK(e.GetScene().Get<CharacterBody>(e.GetScene().FindByName("Hero"))->grounded);
    Json r = Call(e, "script.eval", R"J({"code": "{tilemap.get('Map', 0, 1), tilemap.solid('Map', 1, 1), tilemap.get('Map', 9, 9)}"})J");
    CHECK(r["result"]["value"][0].asString() == "#" && !r["result"]["value"][1].asBool());
    // Changing a tile from Lua updates collision: a ray down column 2 now stops on row 1.
    Call(e, "script.eval", R"J({"code": "tilemap.set('Map', 2, 1, '#')"})J");
    r = Call(e, "physics.raycast", R"J({"origin": [2.5, 0, 0], "direction": [0, -1, 0]})J");
    CHECK(r["result"]["entity"].asInt() == static_cast<int>(e.GetScene().FindByName("Map")));
    CHECK(std::fabs(r["result"]["point"][1].asNumber() + 1.0) < 0.02);
    r = Call(e, "script.eval", R"J({"code": "local c, r = tilemap.cellAt('Map', {x = 2.5, y = -1.2, z = 0}) return {c, r}"})J");
    CHECK(r["result"]["value"][0].asInt() == 2 && r["result"]["value"][1].asInt() == 1);

    // Jumping into a ceiling ends the upward motion right away.
    Call(e, "script.eval", R"J({"code": "tilemap.set('Map', 1, 0, '#')"})J");
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody", "values": {"velocity": [0, 12, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 12})J");
    CHECK(e.GetScene().Get<CharacterBody>(e.GetScene().FindByName("Hero"))->velocity.y <= 0.0f);
}

TEST(ScriptErrorsAndHotReload) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("reload"), &err));
    Call(e, "script.write", R"J({"path": "scripts/counter.lua", "source": "local M = {}\nfunction M:onStart() self.count = 0 end\nfunction M:onUpdate(dt)\n  self.count = self.count + 1\n  if self.count == 5 then local t = nil; t.boom = 1 end\nend\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Counter", "components": {"Script": {"path": "scripts/counter.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 10})J");
    Json errors = Call(e, "script.errors", "{}")["result"];
    CHECK(errors.size() == 1);  // faulted instance reports once, not every frame
    CHECK(errors[0]["message"].asString().find("scripts/counter.lua:5:") != std::string::npos);
    CHECK(errors[0]["entity"].asInt() == static_cast<int>(e.GetScene().FindByName("Counter")));

    // Fix the script: the running instance keeps its state (count = 5) and continues.
    Json rewritten = Call(e, "script.write", R"J({"path": "scripts/counter.lua", "source": "local M = {}\nfunction M:onStart() self.count = 0 end\nfunction M:onUpdate(dt) self.count = self.count + 100 end\nreturn M\n"})J");
    CHECK(rewritten["result"]["reloaded"].size() == 1);
    Call(e, "sim.step", R"J({"frames": 1})J");
    Json r = Call(e, "script.eval", R"J({"code": "self.count", "entity": "Counter"})J");
    CHECK(r["result"]["value"].asInt() == 105);
    CHECK(Call(e, "script.errors", "{}")["result"].size() == 1);

    // A missing script file is reported, not fatal.
    Call(e, "entity.create", R"J({"name": "Ghost", "components": {"Script": {"path": "scripts/missing.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 3})J");
    CHECK(Call(e, "script.errors", "{}")["result"].size() == 2);

    // sim.stop discards the Lua state.
    Call(e, "sim.stop", "{}");
    CHECK(!e.Scripts().Status()["sessionActive"].asBool());
    CHECK(Call(e, "script.write", R"J({"path": "../escape.lua", "source": ""})J")["error"]["code"].asString() == "path_outside_project");
}

TEST(ScriptedSceneIsDeterministic) {
    std::string dir = TempProject("determinism");
    auto run = [&] {
        Engine e;
        std::string err;
        e.Open(dir, &err);
        Call(e, "script.eval", R"J({"code": "local t = {} for i = 1, 50 do t['k' .. i] = i end local s = '' for k in pairs(t) do s = s .. k end return s"})J");
        Call(e, "sim.step", R"J({"frames": 75})J");
        Json shot = Call(e, "render.screenshot", R"J({"width": 96, "height": 54, "inline": false})J");
        Json order = Call(e, "script.eval", R"J({"code": "local t = {} for i = 1, 50 do t['k' .. i] = i end local s = '' for k in pairs(t) do s = s .. k end return s"})J");
        const Transform* t = e.GetScene().Get<Transform>(e.GetScene().FindByName("Pyramid"));
        return std::make_tuple(shot["result"]["hash"].asString(), order["result"]["value"].asString(), t->position.y);
    };
    auto a = run();
    auto b = run();
    CHECK(std::get<0>(a) == std::get<0>(b));
    CHECK(std::get<1>(a) == std::get<1>(b));  // pairs() order is stable across runs
    CHECK(std::fabs(std::get<2>(a) - 0.75f) > 0.05f);  // bob.lua moved the pyramid
}

// ----- physics -----------------------------------------------------------------

// Empty scene with a 20x1x20 static ground whose top face is at y = 0.
void PhysicsScene(Engine& e) {
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Ground", "components": {"Transform": {"position": [0, -0.5, 0], "scale": [20, 1, 20]}, "Collider": {}}})J");
}

Vec3 PosOf(Engine& e, const char* name) { return e.GetScene().Get<Transform>(e.GetScene().FindByName(name))->position; }

TEST(PhysicsBallRestsOnGround) {
    Engine e;
    PhysicsScene(e);
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [0, 5, 0]}, "Collider": {"shape": "sphere"}, "RigidBody": {}}})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    CHECK(PosOf(e, "Ball").y < 4.0f);  // falling
    Call(e, "sim.step", R"J({"frames": 270})J");
    Vec3 p = PosOf(e, "Ball");
    CHECK(std::fabs(p.y - 0.5f) < 0.03f);
    const RigidBody* rb = e.GetScene().Get<RigidBody>(e.GetScene().FindByName("Ball"));
    CHECK(Length(rb->velocity) < 0.05f);
    Json state = Call(e, "physics.state", "{}")["result"];
    CHECK(state["dynamicBodies"].asInt() == 1 && state["staticBodies"].asInt() == 1);
}

TEST(PhysicsBoxStackIsStable) {
    Engine e;
    PhysicsScene(e);
    for (int i = 0; i < 3; ++i) {
        Json args = Json::parse(R"J({"components": {"Transform": {}, "Collider": {}, "RigidBody": {}}})J");
        args["name"] = "Box" + std::to_string(i);
        args["components"]["Transform"]["position"] = Json(Json::Array{0.0, 0.5 + i * 1.02, 0.0});
        e.Call("entity.create", args);
    }
    Call(e, "sim.step", R"J({"frames": 300})J");
    for (int i = 0; i < 3; ++i) {
        Vec3 p = PosOf(e, ("Box" + std::to_string(i)).c_str());
        CHECK(std::fabs(p.y - (0.5f + static_cast<float>(i))) < 0.05f);
        CHECK(std::fabs(p.x) < 0.05f && std::fabs(p.z) < 0.05f);
    }
}

TEST(PhysicsCharacterBlockedByWall) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("physics_character"), &err));
    PhysicsScene(e);
    // Wall face at z = -3 (wall spans z -3.5 .. -3).
    Call(e, "entity.create", R"J({"name": "Wall", "components": {"Transform": {"position": [0, 1, -3.25], "scale": [6, 2, 0.5]}, "Collider": {}}})J");
    Call(e, "entity.create", R"J({"name": "Player", "components": {"Transform": {"position": [0, 0.5, 0]}, "CharacterBody": {"shape": "sphere", "radius": 0.5}, "Script": {"path": "scripts/player_controller.lua"}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "sim.step", R"J({"frames": 180})J");  // 3 s at 4 m/s would reach z = -12 without the wall
    Vec3 p = PosOf(e, "Player");
    CHECK(p.z > -2.55f && p.z < -2.4f);  // touching the wall face (z = -3 + radius)
    CHECK(std::fabs(p.y - 0.5f) < 0.05f);
    CHECK(e.GetScene().Get<CharacterBody>(e.GetScene().FindByName("Player"))->grounded);
    // Jump: leaves the ground, then lands again.
    Call(e, "input.clear", "{}");
    Call(e, "input.key", R"J({"key": "Space"})J");
    Call(e, "sim.step", R"J({"frames": 10})J");
    Call(e, "input.clear", "{}");
    CHECK(PosOf(e, "Player").y > 1.0f);
    Call(e, "sim.step", R"J({"frames": 120})J");
    CHECK(std::fabs(PosOf(e, "Player").y - 0.5f) < 0.05f);
}

TEST(PhysicsTriggersAndScriptCallbacks) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("physics_events"), &err));
    PhysicsScene(e);
    Call(e, "script.write", R"J({"path": "scripts/coin.lua", "source": "local M = {}\nfunction M:onTriggerEnter(other)\n  if scene.name(other) == 'Player' then hits = (hits or 0) + 1 end\nend\nreturn M\n"})J");
    Call(e, "script.write", R"J({"path": "scripts/crate.lua", "source": "local M = {}\nfunction M:onCollisionEnter(other) landed = (landed or 0) + 1 lastHit = scene.name(other) end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Coin", "components": {"Transform": {"position": [0, 0.5, -2]}, "Collider": {"shape": "sphere", "radius": 0.4, "isTrigger": true}, "Script": {"path": "scripts/coin.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Crate", "components": {"Transform": {"position": [3, 2, 0]}, "Collider": {}, "RigidBody": {}, "Script": {"path": "scripts/crate.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Player", "components": {"Transform": {"position": [0, 0.5, 0]}, "CharacterBody": {"shape": "sphere", "radius": 0.5}, "Script": {"path": "scripts/player_controller.lua"}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "sim.step", R"J({"frames": 90})J");  // walks through the coin and beyond
    CHECK(Call(e, "script.eval", R"J({"code": "hits"})J")["result"]["value"].asInt() == 1);
    CHECK(Call(e, "script.eval", R"J({"code": "landed"})J")["result"]["value"].asInt() == 1);
    CHECK(Call(e, "script.eval", R"J({"code": "lastHit"})J")["result"]["value"].asString() == "Ground");
    CHECK(e.Scripts().Errors().empty());
    Json contacts = Call(e, "physics.contacts", R"J({"id": "Crate"})J")["result"];
    CHECK(contacts.size() == 1 && contacts[0]["kind"].asString() == "collision");
}

TEST(PhysicsQueries) {
    Engine e;
    PhysicsScene(e);
    Call(e, "entity.create", R"J({"name": "Box", "components": {"Transform": {"position": [2, 0.5, 0]}, "Collider": {}}})J");
    // Works outside simulation too.
    Json hit = Call(e, "physics.raycast", R"J({"origin": [0, 10, 0], "direction": [0, -1, 0]})J")["result"];
    CHECK(hit["hit"].asBool() && hit["name"].asString() == "Ground");
    CHECK(std::fabs(hit["point"][1].asNumber()) < 1e-3 && std::fabs(hit["normal"][1].asNumber() - 1.0) < 1e-3);
    CHECK(std::fabs(hit["distance"].asNumber() - 10.0) < 1e-3);
    hit = Call(e, "physics.raycast", R"J({"origin": [-5, 0.5, 0], "direction": [1, 0, 0]})J")["result"];
    CHECK(hit["name"].asString() == "Box" && std::fabs(hit["distance"].asNumber() - 6.5) < 1e-3);
    Json overlap = Call(e, "physics.overlap", R"J({"center": [2, 1.2, 0], "radius": 0.3})J")["result"];
    CHECK(overlap.size() == 1 && overlap[0]["name"].asString() == "Box");
    // Collider wireframes show up in screenshots.
    Json a = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false})J");
    Json b = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false, "colliders": true})J");
    CHECK(a["result"]["hash"].asString() != b["result"]["hash"].asString());
}

TEST(TemplatePlayerScriptWithCharacterBody) {
    // With a CharacterBody the template's player script only sets the velocity; physics moves the body.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("lua_character"), &err));
    PhysicsScene(e);
    Call(e, "entity.create", R"J({"name": "Lua", "components": {"Transform": {"position": [3, 0.5, 0]}, "CharacterBody": {"shape": "sphere", "radius": 0.5}, "Script": {"path": "scripts/player_controller.lua"}}})J");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "input.key", R"J({"key": "Space"})J");
    Call(e, "sim.step", R"J({"frames": 5})J");
    Call(e, "input.key", R"J({"key": "Space", "down": false})J");
    Call(e, "sim.step", R"J({"frames": 20})J");
    CHECK(PosOf(e, "Lua").y > 1.0f);  // in the air
    Call(e, "sim.step", R"J({"frames": 70})J");
    Vec3 p = PosOf(e, "Lua");
    CHECK(p.z < -6.0f && p.z > -6.5f && std::fabs(p.x - 3) < 1e-3f && std::fabs(p.y - 0.5f) < 0.05f);  // 95 frames at 4 m/s, landed
    CHECK(e.Scripts().Errors().empty());
}

const UIText* TextOf(Engine& e, const char* name) {
    EntityId id = e.GetScene().FindByName(name);
    return id ? e.GetScene().Get<UIText>(id) : nullptr;
}

int CountPlayed(Engine& e, const char* path) {
    int n = 0;
    Json state = Call(e, "audio.state", "{}");
    for (const Json& ev : state["result"]["played"].items()) n += ev["path"].asString() == path;
    return n;
}

TEST(TemplateGamePlaythrough) {
    // The `oe new` sample game, played headless like an agent would:
    // level 1 (walk forward over 3 coins) -> level 2 (walk right over 4 coins)
    // -> "You win!" -> click "Play again" -> back to level 1.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("template_game"), &err));
    CHECK(e.GetScene().name == "Level 1");
    Call(e, "input.key", R"J({"key": "W"})J");
    Call(e, "sim.step", R"J({"frames": 110})J");
    CHECK(e.GetScene().FindByName("Coin 3") == kNullEntity);
    CHECK(TextOf(e, "Message")->visible && TextOf(e, "Message")->text == "Level clear!");
    CHECK(TextOf(e, "Score")->text.find("Coins: 3/3") == 0);
    CHECK(CountPlayed(e, "sounds/coin.wav") == 3 && CountPlayed(e, "sounds/success.wav") == 1);

    Call(e, "input.clear", "{}");                 // stop walking before level 2 loads
    Call(e, "sim.step", R"J({"frames": 100})J");  // 1.5 s timer -> level 2
    Json state = Call(e, "game.state", "{}")["result"];
    CHECK(state["scene"].asString() == "scenes/level2.scene.json" && state["sceneName"].asString() == "Level 2");
    CHECK(state["data"]["total"].asInt() == 3);

    Call(e, "input.key", R"J({"key": "D"})J");
    Call(e, "sim.step", R"J({"frames": 130})J");
    CHECK(TextOf(e, "Message")->text == "You win!");
    EntityId button = e.GetScene().FindByName("Play Again");
    CHECK(e.GetScene().Get<UIButton>(button)->visible);
    CHECK(Call(e, "game.state", "{}")["result"]["data"]["total"].asInt() == 7);
    CHECK(Call(e, "save.state", "{}")["result"]["data"]["highScore"].asInt() == 7);

    // The button is centered, 60 reference px below the middle: (320, 210) in a 640x360 shot.
    Json click = Call(e, "input.click", R"J({"x": 320, "y": 210})J");
    CHECK(click["result"]["buttonName"].asString() == "Play Again");
    Call(e, "sim.step", R"J({"frames": 2})J");
    CHECK(e.GetScene().name == "Level 1");
    CHECK(Call(e, "game.state", "{}")["result"]["data"]["total"].asInt() == 0);
    CHECK(TextOf(e, "Score")->text.find("Best: 7") != std::string::npos);
    CHECK(e.Scripts().Errors().empty());

    // Walking off the edge respawns the player at the level start.
    Call(e, "input.clear", "{}");
    Call(e, "input.key", R"J({"key": "S"})J");
    Call(e, "sim.step", R"J({"frames": 110})J");  // past the edge at z = 8
    Call(e, "input.clear", "{}");
    CHECK(PosOf(e, "Player").y < 0.0f);           // falling
    Call(e, "sim.step", R"J({"frames": 150})J");
    Vec3 p = PosOf(e, "Player");
    CHECK(p.y > -1.0f && std::fabs(p.z - 2.0f) < 0.5f);

    Call(e, "sim.stop", "{}");
    CHECK(e.GetScene().name == "Level 1" && e.GetScene().FindByName("Coin 1") != kNullEntity);
    CHECK(Call(e, "save.state", "{}")["result"]["data"]["highScore"].asInt() == 7);
}

TEST(PrefabsCreateAndInstantiate) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("prefabs"), &err));
    Call(e, "entity.create", R"J({"name": "Tower", "components": {"MeshRenderer": {}}})J");
    Call(e, "entity.create", R"J({"name": "Flag", "parent": "Tower", "components": {"Transform": {"position": [0, 1, 0]}, "Tag": {"tags": "flag"}}})J");
    Json made = Call(e, "prefab.create", R"J({"id": "Tower", "path": "prefabs/tower.prefab.json"})J");
    CHECK(made["result"]["entities"].asInt() == 2);
    Json a = Call(e, "prefab.instantiate", R"J({"path": "prefabs/tower.prefab.json", "name": "Tower A", "position": [5, 0, 0]})J");
    CHECK(a["ok"].asBool());
    EntityId root = static_cast<EntityId>(a["result"]["id"].asNumber());
    CHECK(e.GetScene().Get<Prefab>(root)->path == "prefabs/tower.prefab.json");
    CHECK(e.GetScene().Get<Transform>(root)->position.x == 5.0f);
    std::vector<EntityId> kids = e.GetScene().Children(root);
    CHECK(kids.size() == 1 && e.GetScene().Get<Tag>(kids[0])->tags == "flag");
    // From Lua, during simulation.
    Json r = Call(e, "script.eval", R"J({"code": "return scene.instantiate('prefabs/coin.prefab.json', {name = 'Spawned', position = {x = 1, y = 2, z = 3}})"})J");
    EntityId spawned = static_cast<EntityId>(r["result"]["value"].asNumber());
    CHECK(e.GetScene().Record(spawned)->name == "Spawned" && e.GetScene().Get<Transform>(spawned)->position.z == 3.0f);
    CHECK(Call(e, "prefab.list", "{}")["result"].size() == 2);
    CHECK(Call(e, "prefab.instantiate", R"J({"path": "prefabs/missing.prefab.json"})J")["error"]["code"].asString() == "not_found");
}

TEST(EntityClipboardSubtreesAndAtomicPaste) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("entity_clipboard"), &error));
    CHECK(Call(e, "scene.new", R"J({"empty":true})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Root","components":{"Transform":{"position":[1,2,3]},"CameraFollow":{}}})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Child","parent":"Root","components":{"CameraFollow":{}}})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Outside","components":{"Transform":{"position":[9,0,0]}}})J")["ok"].asBool());
    const EntityId child = e.GetScene().FindByName("Child"), outside = e.GetScene().FindByName("Outside");
    Json follow = Json::parse(R"J({"id":"Root","type":"CameraFollow","values":{}})J");
    follow["values"]["target"] = child;
    CHECK(e.Call("component.set", follow)["ok"].asBool());
    follow["id"] = "Child";
    follow["values"]["target"] = outside;
    CHECK(e.Call("component.set", follow)["ok"].asBool());
    Json selection = Json::MakeObject();
    selection["ids"] = Json(Json::Array{"Root", child, "Root"});
    const uint64_t copyRevision = e.Revision();
    const size_t copyUndo = e.UndoDepth();
    const Json copied = e.Call("entity.copy", selection);
    CHECK(copied["ok"].asBool() && copied["result"]["entities"].size() == 2);
    CHECK(e.Revision() == copyRevision && e.UndoDepth() == copyUndo);
    Json paste = Json::MakeObject();
    paste["document"] = copied["result"];
    paste["parent"] = "Outside";
    const Json pasted = e.Call("entity.paste", paste);
    CHECK(pasted["ok"].asBool() && pasted["result"]["roots"].size() == 1);
    const EntityId rootCopy = static_cast<EntityId>(pasted["result"]["roots"][0].asNumber());
    const auto children = e.GetScene().Children(rootCopy);
    CHECK(children.size() == 1 && e.GetScene().Entities().size() == 5);
    CHECK(e.GetScene().Record(rootCopy)->parent == outside);
    CHECK(e.GetScene().Get<Transform>(rootCopy)->position.x == 1);
    CHECK(e.GetScene().Get<CameraFollow>(rootCopy)->target == children[0]);
    CHECK(e.GetScene().Get<CameraFollow>(children[0])->target == kNullEntity);
    CHECK(e.UndoDepth() == copyUndo + 1);
    CHECK(e.GetScene().Record(rootCopy)->name == "Root Copy");
    CHECK(e.Call("entity.paste", paste)["ok"].asBool());
    CHECK(e.GetScene().FindByName("Root Copy 2") != kNullEntity);
    CHECK(e.GetScene().FindByName("Child Copy 2") != kNullEntity);
    const std::string before = e.GetScene().ToJson().dump();
    const uint64_t revision = e.Revision();
    const size_t undo = e.UndoDepth();
    auto rejected = [&](const Json& document) {
        Json args = Json::MakeObject(); args["document"] = document;
        CHECK(!e.Call("entity.paste", args)["ok"].asBool());
        CHECK(e.GetScene().ToJson().dump() == before);
        CHECK(e.Revision() == revision && e.UndoDepth() == undo);
    };
    rejected(Json::MakeObject());
    Json bad = copied["result"];
    bad["entities"][0]["id"] = bad["entities"][1]["id"];
    rejected(bad);
    bad = copied["result"];
    bad["entities"][0]["parent"] = bad["entities"][1]["id"];
    rejected(bad); // child already has root as its parent: this creates a cycle
    bad = copied["result"];
    bad["entities"][1]["components"]["UnknownComponent"] = Json::MakeObject();
    rejected(bad);
    bad = copied["result"];
    bad["entities"][1]["components"]["MeshRenderer"]["shading"] = "invalid";
    rejected(bad);
    bad = copied["result"];
    bad["entities"][0]["components"]["CameraFollow"]["target"] = 999;
    rejected(bad);
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());
    CHECK(e.GetScene().Entities().size() == 5);
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());
    CHECK(e.GetScene().Entities().size() == 3);
}

TEST(HistoryTimelineCursorAndMergedArguments) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("history_timeline"), &error));
    CHECK(Call(e, "scene.new", R"J({"empty":true})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Box","components":{"Transform":{}}})J")["ok"].asBool());
    const size_t base = e.UndoDepth();
    for (int x : {1,2,3}) {
        Json args = Json::parse(R"J({"id":"Box","type":"Transform","values":{"position":[0,0,0]},"merge":"drag"})J");
        args["values"]["position"][0] = x;
        CHECK(e.Call("component.set", args)["ok"].asBool());
    }
    CHECK(Call(e, "entity.rename", R"J({"id":"Box","name":"Renamed"})J")["ok"].asBool());
    Json timeline = Call(e, "history.list", "{}")["result"];
    CHECK(timeline["cursor"].asNumber() == base + 2 && timeline["entries"].size() == base + 2);
    CHECK(timeline["entries"][base]["command"].asString() == "component.set");
    CHECK(timeline["entries"][base]["args"]["values"]["position"][0].asInt() == 3);
    CHECK(timeline["entries"][base + 1]["command"].asString() == "entity.rename");
    Json go = Json::MakeObject(); go["cursor"] = static_cast<uint64_t>(base);
    CHECK(e.Call("history.go", go)["ok"].asBool());
    CHECK(e.GetScene().FindByName("Box") != kNullEntity && PosOf(e,"Box").x == 0);
    timeline = Call(e, "history.list", "{}")["result"];
    CHECK(timeline["entries"].size() == base + 2);
    CHECK(!timeline["entries"][base]["applied"].asBool() && !timeline["entries"][base + 1]["applied"].asBool());
    go["cursor"] = static_cast<uint64_t>(base + 2);
    CHECK(e.Call("history.go", go)["ok"].asBool());
    CHECK(PosOf(e,"Renamed").x == 3);
    go["cursor"] = static_cast<uint64_t>(base);
    CHECK(e.Call("history.go", go)["ok"].asBool());
    CHECK(Call(e, "entity.rename", R"J({"id":"Box","name":"Branch"})J")["ok"].asBool());
    timeline = Call(e, "history.list", "{}")["result"];
    CHECK(timeline["cursor"].asNumber() == base + 1 && timeline["entries"].size() == base + 1 && e.RedoDepth() == 0);
    CHECK(timeline["entries"][base]["args"]["name"].asString() == "Branch");
    CHECK(!Call(e, "history.go", R"J({"cursor":-1})J")["ok"].asBool());
    CHECK(Call(e, "sim.play", "{}")["ok"].asBool());
    CHECK(Call(e, "history.go", R"J({"cursor":0})J")["error"]["code"].asString() == "simulating");
    CHECK(Call(e, "sim.stop", "{}")["ok"].asBool());
}

TEST(PrefabSourceEditIsolationAndSave) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("prefab_source_edit"), &error));
    CHECK(Call(e, "scene.new", R"J({"empty":true})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Root","components":{"Transform":{}}})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Child","parent":"Root","components":{"Tag":{"tags":"original"}}})J")["ok"].asBool());
    CHECK(Call(e, "prefab.create", R"J({"id":"Root","path":"prefabs/edit.prefab.json"})J")["ok"].asBool());
    CHECK(Call(e, "prefab.instantiate", R"J({"path":"prefabs/edit.prefab.json","name":"Existing"})J")["ok"].asBool());
    const EntityId existing = e.GetScene().FindByName("Existing");
    const std::string original = e.GetScene().ToJson().dump();
    const std::string history = Call(e, "history.list", "{}")["result"].dump();
    const bool dirty = e.Dirty();
    CHECK(Call(e, "prefab.edit", R"J({"path":"prefabs/edit.prefab.json"})J")["ok"].asBool());
    CHECK(e.GetScene().Entities().size() == 2 && e.GetScene().FindByName("Existing") == kNullEntity);
    CHECK(e.UndoDepth() == 0 && !e.Dirty());
    CHECK(Call(e, "prefab.state", "{}")["result"]["path"].asString() == "prefabs/edit.prefab.json");
    CHECK(!Call(e, "prefab.state", "{}")["result"]["dirty"].asBool());
    for (const char* command : {"scene.new", "scene.save", "sim.play", "sim.step"})
        CHECK(Call(e, command, "{}")["error"]["code"].asString() == "prefab_editing");
    CHECK(Call(e, "component.set", R"J({"id":"Child","type":"Tag","values":{"tags":"edited"}})J")["ok"].asBool());
    CHECK(Call(e, "prefab.state", "{}")["result"]["dirty"].asBool());
    CHECK(!Call(e, "prefab.close", "{}")["ok"].asBool());
    CHECK(e.EditingPrefab() == "prefabs/edit.prefab.json");
    CHECK(Call(e, "prefab.save", "{}")["ok"].asBool());
    CHECK(!e.Dirty());
    CHECK(Call(e, "prefab.close", "{}")["ok"].asBool());
    CHECK(e.GetScene().ToJson().dump() == original && e.Dirty() == dirty);
    CHECK(Call(e, "history.list", "{}")["result"].dump() == history);
    CHECK(Call(e, "prefab.state", "{}")["result"]["path"].asString().empty());
    CHECK(e.GetScene().Get<Tag>(e.GetScene().Children(existing)[0])->tags == "original");
    CHECK(Call(e, "prefab.instantiate", R"J({"path":"prefabs/edit.prefab.json","name":"Later"})J")["ok"].asBool());
    CHECK(e.GetScene().Get<Tag>(e.GetScene().Children(e.GetScene().FindByName("Later"))[0])->tags == "edited");
    const std::string restored = e.GetScene().ToJson().dump();
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(),"prefabs/invalid.prefab.json"),
        R"J({"format":"ownengine.prefab","version":1,"entities":[{"id":1,"name":"A"},{"id":2,"name":"B"}]})J"));
    CHECK(!Call(e, "prefab.edit", R"J({"path":"prefabs/invalid.prefab.json"})J")["ok"].asBool());
    CHECK(e.GetScene().ToJson().dump() == restored && e.EditingPrefab().empty());
    CHECK(Call(e, "prefab.edit", R"J({"path":"prefabs/edit.prefab.json"})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"ExtraRoot"})J")["ok"].asBool());
    CHECK(!Call(e, "prefab.save", "{}")["ok"].asBool());
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());
    CHECK(Call(e, "entity.delete", R"J({"id":"Root"})J")["ok"].asBool());
    CHECK(e.GetScene().Entities().empty());
    CHECK(!Call(e, "prefab.save", "{}")["ok"].asBool());
    CHECK(!Call(e, "prefab.close", "{}")["ok"].asBool());
    CHECK(Call(e, "prefab.close", R"J({"discard":true})J")["ok"].asBool());
    CHECK(e.GetScene().ToJson().dump() == restored);
}

TEST(AssetPreviewPixelsFramingAndIsolation) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("asset_preview"), &error));
    CHECK(Call(e,"scene.new",R"({"empty":true})")["ok"].asBool());
    CHECK(Call(e,"entity.create",R"({"name":"Cube","components":{"Transform":{},"MeshRenderer":{"color":[1,0,0],"unlit":true}}})")["ok"].asBool());
    CHECK(Call(e,"scene.save",R"({"path":"scenes/preview.scene.json"})")["ok"].asBool());
    CHECK(Call(e,"prefab.create",R"({"id":"Cube","path":"prefabs/preview.prefab.json"})")["ok"].asBool());
    CHECK(CopyFileTo(TestSourceDir()+"/samples/Showcase/assets/models/fox.glb",JoinPath(e.ProjectDir(),"assets/fox.glb")));
    Image image; image.width=4; image.height=2; image.rgba.assign(32,0);
    for (int y=0;y<2;++y) for (int x=2;x<4;++x) {
        const size_t at=static_cast<size_t>((y*4+x)*4); image.rgba[at]=255; image.rgba[at+3]=255;
    }
    CHECK(WritePng(JoinPath(e.ProjectDir(),"assets/wide.png"),image,true));
    auto preview=[&](const char* path) {
        Json args=Json::MakeObject(); args["path"]=path; args["size"]=64; args["pixels"]=true;
        Json result=e.Call("asset.preview",args); CHECK(result["ok"].asBool());
        CHECK(result["result"]["width"].asInt()==64 && result["result"]["pixels"].size()==4096);
        return result["result"]["pixels"];
    };
    const std::string scene=e.GetScene().ToJson().dump();
    const uint64_t revision=e.Revision(); const size_t undo=e.UndoDepth(); const bool dirty=e.Dirty();
    const Json texture=preview("assets/wide.png");
    CHECK(texture[32*64+48].asNumber()==static_cast<double>(0xFF0000FFu));
    CHECK(texture[32*64+16].asNumber()==static_cast<double>(0xFF484848u)); // transparent texel reveals checkerboard
    CHECK(texture[8*64+48].asNumber()==static_cast<double>(0xFF303030u)); // centered 2:1 image leaves top margin
    for (const char* path : {"assets/fox.glb","scenes/preview.scene.json","prefabs/preview.prefab.json"}) {
        const Json pixels=preview(path); int changed=0;
        for (const Json& pixel : pixels.items()) changed+=pixel.asNumber()!=pixels[0].asNumber();
        CHECK(changed>100);
    }
    CHECK(e.GetScene().ToJson().dump()==scene && e.Revision()==revision && e.UndoDepth()==undo && e.Dirty()==dirty);
    CHECK(!Call(e,"asset.preview",R"({"path":"../outside.png"})")["ok"].asBool());
    CHECK(!Call(e,"asset.preview",R"({"path":"assets/wide.png","out":"../outside.png"})")["ok"].asBool());
    CHECK(!Call(e,"asset.preview",R"({"path":"assets/wide.png","size":1000})")["ok"].asBool());
    CHECK(e.Revision()==revision && e.UndoDepth()==undo);
    CHECK(Call(e,"component.set",R"({"id":"Cube","type":"Transform","values":{"scale":[1000,1000,1000]}})")["ok"].asBool());
    CHECK(Call(e,"scene.save",R"({"path":"scenes/large.scene.json"})")["ok"].asBool());
    const Json large=preview("scenes/large.scene.json"); int visible=0;
    for (const Json& pixel : large.items()) visible+=(static_cast<uint32_t>(pixel.asNumber()) & 0xFFFFFFu)==255;
    CHECK(visible>100);
    CHECK(Call(e,"shader.create",R"({"path":"preview.shader.json","graph":{"nodes":[{"op":"constant","value":[1,0,0,1]}],"color":0}})")["ok"].asBool());
    CHECK(Call(e,"material.create",R"({"path":"preview.mat.json","values":{"shader":"preview.shader.json","unlit":true}})")["ok"].asBool());
    const Json red=preview("preview.mat.json");
    CHECK(Call(e,"shader.create",R"({"path":"preview.shader.json","overwrite":true,"graph":{"nodes":[{"op":"constant","value":[0,0,1,1]}],"color":0}})")["ok"].asBool());
    const Json blue=preview("preview.mat.json");
    CHECK(red.dump()!=blue.dump());
    CHECK(Call(e,"material.set",R"({"path":"preview.mat.json","values":{"shader":"","shaderUniforms":{},"baseColor":[0,1,0]}})")["ok"].asBool());
    CHECK(preview("preview.mat.json").dump()!=blue.dump());
}

TEST(PrefabSparseReferencesAndRecordingIsolation) {
    Engine e; std::string error;
    CHECK(e.Open(TempProject("prefab_sparse"),&error));
    CHECK(Call(e,"scene.new",R"({"empty":true})")["ok"].asBool());
    CHECK(Call(e,"entity.create",R"({"name":"Original"})")["ok"].asBool());
    const std::string original=e.GetScene().ToJson().dump();
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(),"sparse.prefab.json"),R"({"format":"ownengine.prefab","version":1,
      "entities":[{"id":100,"name":"Root","components":{"CameraFollow":{"target":400}}},
      {"id":400,"name":"Child","parent":100,"components":{"CameraFollow":{"target":100}}}]})"));
    CHECK(Call(e,"prefab.edit",R"({"path":"sparse.prefab.json"})")["ok"].asBool());
    CHECK(Call(e,"sim.record_state","{}")["error"]["code"].asString()=="prefab_editing");
    CHECK(!e.InPlaySession());
    CHECK(Call(e,"prefab.save","{}")["ok"].asBool());
    CHECK(Call(e,"prefab.close","{}")["ok"].asBool());
    CHECK(e.GetScene().ToJson().dump()==original && !e.InPlaySession());
    const Json result=Call(e,"prefab.instantiate",R"({"path":"sparse.prefab.json","name":"Instance"})");
    CHECK(result["ok"].asBool());
    const EntityId root=static_cast<EntityId>(result["result"]["id"].asNumber());
    const auto children=e.GetScene().Children(root);
    CHECK(children.size()==1);
    if (children.size()==1) {
        CHECK(e.GetScene().Get<CameraFollow>(root)->target==children[0]);
        CHECK(e.GetScene().Get<CameraFollow>(children[0])->target==root);
    }
}

TEST(TimersMessagesAndGameData) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("timers"), &err));
    PhysicsScene(e);
    Call(e, "script.write", R"J({"path": "scripts/receiver.lua", "source": "local M = {}\nfunction M:onStart() self.got = 0 end\nfunction M:ping(n) self.got = self.got + n return self.got end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "A", "components": {"Script": {"path": "scripts/receiver.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "B", "components": {"Script": {"path": "scripts/receiver.lua"}}})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(Call(e, "script.eval", R"J({"code": "scene.send(scene.find('A'), 'ping', 5)"})J")["result"]["value"].asInt() == 5);
    CHECK(Call(e, "script.eval", R"J({"code": "scene.broadcast('ping', 1)"})J")["result"]["value"].asInt() == 2);
    CHECK(Call(e, "script.eval", R"J({"code": "self.got", "entity": "A"})J")["result"]["value"].asInt() == 6);
    CHECK(Call(e, "script.eval", R"J({"code": "scene.send(scene.find('Ground'), 'ping', 1)"})J")["result"]["value"].isNull());

    Call(e, "script.eval", R"J({"code": "fired = 0 ticks = 0 timer.after(0.5, function() fired = time.frame() end) local id = timer.every(0.25, function() ticks = ticks + 1 end) cancelMe = timer.after(0.1, function() fired = -1 end) timer.cancel(cancelMe)"})J");
    Call(e, "sim.step", R"J({"frames": 60})J");
    int fired = Call(e, "script.eval", R"J({"code": "fired"})J")["result"]["value"].asInt();
    CHECK(fired >= 30 && fired <= 32);
    CHECK(Call(e, "script.eval", R"J({"code": "ticks"})J")["result"]["value"].asInt() == 3);  // at +0.25, +0.5, +0.75 s

    Call(e, "script.eval", R"J({"code": "game.set('lives', 3) game.set('name', 'hero')"})J");
    Json data = Call(e, "game.state", "{}")["result"]["data"];
    CHECK(data["lives"].asInt() == 3 && data["name"].asString() == "hero");
    Call(e, "sim.stop", "{}");
    CHECK(Call(e, "game.state", "{}")["result"]["data"].size() == 0);  // reset with the session
    CHECK(e.Scripts().Errors().empty());
}

TEST(UIRenderingAndClicks) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "script.write", R"J({"path": "scripts/button.lua", "source": "local M = {}\nfunction M:onClick() clicks = (clicks or 0) + 1 end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Label", "components": {"UIText": {"text": "HI", "font": "pixel", "x": 0, "y": 0, "size": 80, "color": [1, 1, 1]}}})J");
    Call(e, "entity.create", R"J({"name": "Btn", "components": {"UIButton": {"anchor": "bottom-right", "x": -10, "y": -10, "width": 200, "height": 100}, "Script": {"path": "scripts/button.lua"}}})J");
    // Text pixels: the 'H' left column starts at the top-left corner.
    RenderTarget rt;
    rt.Resize(1280, 720);
    RenderView view;
    MakeSceneView(e.GetScene(), 1280.0f / 720.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    CHECK(rt.color[static_cast<size_t>(20) * 1280 + 3] == 0xFFFFFFFFu);  // inside the H stem
    CHECK(rt.IdAt(3, 20) == e.GetScene().FindByName("Label"));
    // Hidden elements are not drawn; free cameras skip UI.
    Json shot = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false})J");
    Json free = Call(e, "render.screenshot", R"J({"width": 64, "height": 36, "inline": false, "camera": {"eye": [0, 5, 5], "target": [0, 0, 0]}})J");
    CHECK(shot["result"]["hash"].asString() != free["result"]["hash"].asString());
    // Click inside / outside the button (bottom-right 200x100 at 10 px margin in a 1280x720 view).
    Json hit = Call(e, "input.click", R"J({"x": 1200, "y": 650, "width": 1280, "height": 720})J");
    CHECK(hit["result"]["buttonName"].asString() == "Btn");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.click", R"J({"x": 100, "y": 650, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(Call(e, "script.eval", R"J({"code": "clicks"})J")["result"]["value"].asInt() == 1);
}

TEST(WebTouchLifecycle) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("web_touch"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"LeftBtn","components":{"UIButton":{"anchor":"bottom-left","x":10,"y":-10,"width":100,"height":100,"key":"Left"}}})J");
    Call(e, "entity.create", R"J({"name":"JumpBtn","components":{"UIButton":{"anchor":"bottom-right","x":-10,"y":-10,"width":100,"height":100,"key":"Space"}}})J");
    auto eval = [&](const char* code) {
        Json args = Json::MakeObject();
        args["code"] = code;
        return e.Call("script.eval", args)["result"]["value"];
    };
    TouchInput touch;
    using Kind = TouchInput::Kind;
    touch.Apply(e.Input(), Kind::Begin, 54, 60.0f / 1280, 660.0f / 720);
    touch.Apply(e.Input(), Kind::Begin, 7, 1220.0f / 1280, 660.0f / 720);
    CHECK(e.Input().touches.size() == 2 && e.Input().touches[0].id == 54 && e.Input().touches[1].id == 7);
    CHECK(e.Input().IsDown("MouseLeft") && e.Input().mouseX < 0.1f);
    CHECK(eval("local t=input.touches(); return t[1].began and t[2].began").asBool());
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(e.Input().IsDown("Left") && e.Input().IsDown("Space"));
    CHECK(!e.Input().touches[0].began && !e.Input().touches[1].began);
    touch.Apply(e.Input(), Kind::Move, 7, 0.5f, 0.5f);
    touch.Apply(e.Input(), Kind::Move, 99, 0.2f, 0.2f);  // unknown moves never invent fingers
    CHECK(e.Input().touches.size() == 2 && e.Input().mouseX < 0.1f);
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(e.Input().IsDown("Left") && !e.Input().IsDown("Space"));
    touch.Apply(e.Input(), Kind::End, 7, 0.5f, 0.5f);  // secondary end must not release the mouse
    CHECK(e.Input().IsDown("MouseLeft") && e.Input().touches.size() == 1);
    touch.Apply(e.Input(), Kind::Begin, 7, 0.95f, 0.92f);
    touch.Apply(e.Input(), Kind::Begin, 7, 0.95f, 0.92f);  // duplicate begin retains order
    CHECK(e.Input().touches.size() == 2 && e.Input().touches[1].began);
    touch.Apply(e.Input(), Kind::End, 54, 0.05f, 0.92f);  // primary end/cancel leaves the other finger
    CHECK(!e.Input().IsDown("MouseLeft") && e.Input().touches.size() == 1 && e.Input().touches[0].id == 7);
    touch.Apply(e.Input(), Kind::Move, 7, 0.9f, 0.9f);
    CHECK(!e.Input().IsDown("MouseLeft"));  // no synthetic second click from a held finger
    touch.Apply(e.Input(), Kind::End, 7, 0.9f, 0.9f);
    touch.Apply(e.Input(), Kind::Begin, 54, 0.4f, 0.3f);  // IDs may be reused in another gesture
    CHECK(e.Input().IsDown("MouseLeft") && e.Input().mouseX == 0.4f);
    touch.Reset(e.Input());
    CHECK(e.Input().touches.empty() && !e.Input().IsDown("MouseLeft") && !e.Input().pressedThisFrame.count("MouseLeft"));
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(!e.Input().IsDown("Left") && !e.Input().IsDown("Space"));
    CHECK(e.Scripts().Errors().empty());
}

TEST(MultiTouchButtonKeys) {
    // On-screen controls: UIButton.key holds a key while any finger (or the mouse) is on the button.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("touch"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "script.write", R"J({"path": "scripts/pad.lua", "source": "local M = {}\nfunction M:onUpdate()\n  if input.pressed('Space') then jumps = (jumps or 0) + 1 end\n  left = input.down('Left')\n  fingers = #input.touches()\n  local t = input.touches()[1]\n  firstBegan = t and t.began or false\nend\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Pad", "components": {"Script": {"path": "scripts/pad.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "LeftBtn", "components": {"UIButton": {"anchor": "bottom-left", "x": 10, "y": -10, "width": 100, "height": 100, "key": "Left"}}})J");
    Call(e, "entity.create", R"J({"name": "JumpBtn", "components": {"UIButton": {"anchor": "bottom-right", "x": -10, "y": -10, "width": 100, "height": 100, "key": "Space"}}})J");
    auto eval = [&](const char* code) { return Call(e, "script.eval", (std::string(R"J({"code": ")J") + code + "\"}").c_str())["result"]["value"]; };
    // Finger 1 holds Left, finger 2 taps Jump at the same time (1280x720 view).
    CHECK(Call(e, "input.touch", R"J({"id": 1, "x": 60, "y": 660, "width": 1280, "height": 720})J")["ok"].asBool());
    Call(e, "input.touch", R"J({"id": 2, "x": 1220, "y": 660, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("left").asBool() && eval("jumps").asInt() == 1 && eval("fingers").asInt() == 2 && eval("firstBegan").asBool());
    CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("JumpBtn"))->pressed);
    Call(e, "sim.step", R"J({"frames": 3})J");
    CHECK(eval("jumps").asInt() == 1 && !eval("firstBegan").asBool());  // held: pressed only once
    Call(e, "input.touch", R"J({"id": 2, "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.touch", R"J({"id": 2, "x": 1220, "y": 660, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("jumps").asInt() == 2 && eval("left").asBool());
    // Sliding finger 1 off the button releases Left; lifting everything clears the keys.
    Call(e, "input.touch", R"J({"id": 1, "x": 640, "y": 300, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(!eval("left").asBool());
    Call(e, "input.touch", R"J({"id": 1, "down": false})J");
    Call(e, "input.touch", R"J({"id": 2, "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("fingers").asInt() == 0 && !e.Input().IsDown("Space") && !e.Input().IsDown("Left"));
    // The mouse works the same way (desktop, editor Game view).
    Call(e, "input.mouse", R"J({"x": 60, "y": 660, "width": 1280, "height": 720, "button": "MouseLeft", "down": true})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(eval("left").asBool());
    CHECK(!Call(e, "input.touch", R"J({"id": 9})J")["ok"].asBool());  // a new finger needs a position
    CHECK(e.Scripts().Errors().empty());
}

TEST(AudioMixingIsCapturable) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("audio"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    CHECK(Call(e, "audio.generate", R"J({"path": "sounds/blip.wav", "preset": "blip"})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name": "Music", "components": {"AudioSource": {"clip": "sounds/coin.wav", "loop": true, "volume": 0.5}}})J");
    Call(e, "audio.capture", R"J({"action": "start"})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    Call(e, "script.eval", R"J({"code": "audio.play('sounds/blip.wav')"})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    Json stats = Call(e, "audio.capture", R"J({"action": "stop", "path": "out.wav"})J")["result"];
    CHECK(std::fabs(stats["seconds"].asNumber() - 1.0) < 1e-6);
    CHECK(stats["peak"].asNumber() > 0.1 && stats["rms"].asNumber() > 0.01);
    Json st = Call(e, "audio.state", "{}")["result"];
    CHECK(st["voices"].size() == 1 && st["voices"][0]["loop"].asBool());  // blip finished, music loops
    CHECK(st["played"].size() == 2 && st["played"][1]["frame"].asInt() == 30);
    // WAV round trip through the decoder.
    std::vector<unsigned char> bytes;
    CHECK(ReadBinaryFile(JoinPath(e.ProjectDir(), "out.wav"), bytes));
    AudioClip clip;
    CHECK(DecodeWav(bytes, clip, &err) && clip.Frames() == 48000);
    // Silence is silent; removing the source stops its voice.
    Call(e, "component.remove", R"J({"id": "Music", "type": "AudioSource"})J");
    Call(e, "audio.capture", R"J({"action": "start"})J");
    Call(e, "sim.step", R"J({"frames": 10})J");
    CHECK(Call(e, "audio.capture", R"J({"action": "stop"})J")["result"]["peak"].asNumber() == 0.0);
    CHECK(Call(e, "audio.play", R"J({"path": "sounds/nope.wav"})J")["error"]["code"].asString() == "not_found");
}

TEST(PhysicsIsDeterministic) {
    auto run = [] {
        Engine e;
        std::string err;
        CHECK(e.Open(TempProject("physics_deterministic"), &err));
        PhysicsScene(e);
        for (int i = 0; i < 6; ++i) {
            Json args = Json::parse(R"J({"components": {"Transform": {}, "Collider": {}, "RigidBody": {}}})J");
            args["name"] = "Box" + std::to_string(i);
            args["components"]["Transform"]["position"] = Json(Json::Array{0.3 * i, 1.0 + 1.3 * i, 0.2 * i});
            args["components"]["Transform"]["rotation"] = Json(Json::Array{10.0 * i, 20.0 * i, 0.0});
            e.Call("entity.create", args);
        }
        Call(e, "entity.create", R"J({"name": "Player", "components": {"Transform": {"position": [0, 0.5, 4]}, "CharacterBody": {}, "Script": {"path": "scripts/player_controller.lua"}}})J");
        Call(e, "input.key", R"J({"key": "W"})J");
        Call(e, "sim.step", R"J({"frames": 240})J");
        Json shot = Call(e, "render.screenshot", R"J({"width": 96, "height": 54, "inline": false, "camera": {"eye": [6, 5, 8], "target": [0, 1, 0]}})J");
        return std::make_pair(shot["result"]["hash"].asString(), e.GetScene().ToJson().dump());
    };
    auto a = run(), b = run();
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);  // every transform and velocity identical
}

// ----- 2D physics (Box2D) & tiles --------------------------------------------------

Tilemap MakeMap(std::initializer_list<const char*> rows, const char* legend, const char* solid = "") {
    Tilemap tm;
    tm.map = Json::MakeArray();
    for (const char* r : rows) tm.map.push(std::string(r));
    tm.legend = Json::parse(legend);
    tm.solid = solid;
    return tm;
}

int FrameAt(const TileFrames& f, int col, int row) { return f.frames[static_cast<size_t>(row * f.width + col)]; }

float LoopArea(const std::vector<TilePoint>& loop) {
    float a = 0;
    for (size_t i = 0; i < loop.size(); ++i) a += loop[i].x * loop[(i + 1) % loop.size()].y - loop[(i + 1) % loop.size()].x * loop[i].y;
    return 0.5f * a;
}

TEST(TileRulesAndAutotiling) {
    // 47 blob patterns: nothing connected is the first, everything the last.
    CHECK(BlobMasks().size() == 47);
    CHECK(BlobIndex(0) == 0 && BlobIndex(255) == 46);
    CHECK(BlobIndex(2) == 0);  // a lone corner without both sides does not count
    // "sides": 4 neighbours (N=1 E=2 S=4 W=8) pick one of 16 frames; outside the map connects by default.
    Tilemap ring = MakeMap({"###", "# #", "###"}, R"J({"#": {"autotile": "sides", "frame": 16}})J");
    TileRules rules = BuildTileRules(ring, nullptr);
    TileFrames f = ResolveFrames(ring, rules);
    CHECK(f.width == 3 && f.height == 3);
    CHECK(FrameAt(f, 1, 1) == -1);             // the hole
    CHECK(FrameAt(f, 0, 0) == 16 + 15);        // N, W outside + E, S
    CHECK(FrameAt(f, 1, 0) == 16 + 1 + 2 + 8); // N outside, E, W (S is the hole)
    ring.legend = Json::parse(R"J({"#": {"autotile": "sides", "frame": 16, "edges": false}})J");
    f = ResolveFrames(ring, BuildTileRules(ring, nullptr));
    CHECK(FrameAt(f, 0, 0) == 16 + 2 + 4);
    // Blob with connects: '=' counts as the same terrain.
    Tilemap blob = MakeMap({"#=", "##"}, R"J({"#": {"autotile": "blob", "frames": [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46], "connects": "=", "edges": false}, "=": 99})J");
    f = ResolveFrames(blob, BuildTileRules(blob, nullptr));
    CHECK(FrameAt(f, 0, 0) == BlobIndex(4 | 8 | 16));  // E ('='), SE, S
    CHECK(FrameAt(f, 1, 0) == 99);
    // Variants: stable per cell, and they do vary.
    Tilemap grass = MakeMap({"..........", ".........."}, R"J({".": {"variants": [5, 6, 7]}})J");
    TileFrames g1 = ResolveFrames(grass, BuildTileRules(grass, nullptr)), g2 = ResolveFrames(grass, BuildTileRules(grass, nullptr));
    CHECK(g1.frames == g2.frames);
    std::set<int> seen(g1.frames.begin(), g1.frames.end());
    CHECK(seen.size() >= 2 && !seen.count(-1) && *seen.begin() >= 5 && *seen.rbegin() <= 7);
    // Rule errors are reported, the rest keeps working.
    Tilemap bad = MakeMap({"#"}, R"J({"#": {"autotile": "blob", "frames": [1, 2]}})J");
    CHECK(BuildTileRules(bad, nullptr).error.find("47") != std::string::npos);

    // Collision: solid outlines (holes wind the other way), one-way runs, shaped cells.
    Tilemap level = MakeMap({"#####", "#   #", "#####", "  ==/", "   /#"}, R"J({"=": {"frame": 1, "collision": "oneway"}, "/": {"collision": "slope-up"}})J", "#");
    TileRules lr = BuildTileRules(level, nullptr);
    std::vector<std::vector<TilePoint>> loops = SolidOutlines(level, lr);
    CHECK(loops.size() == 3);  // ring outside, ring hole, lone block
    int outer = 0, holes = 0;
    for (const auto& l : loops) (LoopArea(l) > 0 ? outer : holes) += 1;
    CHECK(outer == 2 && holes == 1);
    CHECK(loops[0].size() == 4 && std::fabs(LoopArea(loops[0]) - 15.0f) < 1e-4f);  // 5x3 block, collinear corners removed
    std::vector<TileRect> runs = OneWayRuns(level, lr);
    CHECK(runs.size() == 1 && runs[0].col == 2 && runs[0].row == 3 && runs[0].width == 2);
    std::vector<TileShape> shaped = ShapedCells(level, lr);
    CHECK(shaped.size() == 2 && shaped[0].col == 4 && shaped[0].row == 3 && shaped[0].points.size() == 3);
    CHECK(CollisionAt(level, lr, 0, 0) == TileCollision::Solid && CollisionAt(level, lr, 2, 3) == TileCollision::OneWay);
    CHECK(SolidRects(level, lr).size() == 5);

    // Concave polygons are split into convex pieces.
    std::vector<std::vector<TilePoint>> pieces = ConvexPieces({{0, 0}, {3, 0}, {3, 1}, {1, 1}, {1, 3}, {0, 3}});  // L shape
    float area = 0;
    for (const auto& piece : pieces) area += LoopArea(piece);
    CHECK(pieces.size() == 2 && std::fabs(area - 5.0f) < 1e-4f);
    CHECK(ConvexPieces({{0, 0}, {1, 0}, {1, 1}, {0, 1}}).size() == 1);
}

TEST(TilesetFilesAndPaintCommands) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("tilesets"), &err));
    Call(e, "scene.new", R"J({"empty": true})J");
    // tileset.create validates the rules.
    Json bad = Call(e, "tileset.create", R"J({"path": "tilesets/t.tileset.json", "image": "t.png", "columns": 8, "rows": 8, "tiles": {"#": {"autotile": "sides"}}})J");
    CHECK(!bad["ok"].asBool() && bad["error"]["code"].asString() == "invalid_tileset");
    Json ok = Call(e, "tileset.create", R"J({"path": "tilesets/t.tileset.json", "image": "t.png", "columns": 8, "rows": 8,
        "tiles": {"#": {"autotile": "sides", "frame": 16, "collision": "solid"}, "=": {"frame": 2, "collision": "oneway"}, ".": {"variants": [0, 1]}}})J");
    CHECK(ok["ok"].asBool());
    CHECK(Call(e, "asset.info", R"J({"path": "tilesets/t.tileset.json"})J")["result"]["tiles"].asString() == "#.=");
    Call(e, "entity.create", R"J({"name": "Map", "components": {"Transform": {}, "Tilemap": {"tileset": "tilesets/t.tileset.json", "map": ["", "", ""], "legend": {"=": 3}}}})J");
    // The component's legend overrides a rule's frame and keeps its collision.
    Json info = Call(e, "tilemap.info", R"J({"id": "Map"})J")["result"];
    CHECK(info["columns"].asInt() == 8 && info["image"].asString() == "t.png");
    CHECK(info["tiles"].size() == 3);
    CHECK(info["tiles"][2]["char"].asString() == "=" && info["tiles"][2]["frame"].asInt() == 3 && info["tiles"][2]["collision"].asString() == "oneway");
    CHECK(info["tiles"][0]["frame"].asInt() == 31 && info["tiles"][0]["autotile"].asString() == "sides");

    // Painting: one undo step per merge key.
    size_t depth = e.UndoDepth();
    Call(e, "tilemap.paint", R"J({"id": "Map", "char": "#", "cells": [[0, 2], [1, 2]], "merge": "stroke1"})J");
    Call(e, "tilemap.paint", R"J({"id": "Map", "char": "#", "cells": [[2, 2], [3, 2]], "merge": "stroke1"})J");
    Json fill = Call(e, "tilemap.fill", R"J({"id": "Map", "char": "=", "col": 1, "row": 0, "width": 2, "height": 1})J");
    CHECK(fill["result"]["changed"].asInt() == 2);
    const Tilemap* tm = e.GetScene().Get<Tilemap>(e.GetScene().FindByName("Map"));
    CHECK(tm->map[0].asString() == " ==" && tm->map[2].asString() == "####");
    CHECK(e.UndoDepth() == depth + 2);
    Call(e, "history.undo", "{}");
    tm = e.GetScene().Get<Tilemap>(e.GetScene().FindByName("Map"));
    CHECK(tm->map[0].asString() == "" && tm->map[2].asString() == "####");
    CHECK(!Call(e, "tilemap.paint", R"J({"id": "Map", "char": "##", "cells": [[0, 0]]})J")["ok"].asBool());

    // Tile collision from the file: a 2D ball rests on the painted row (top at y = -2).
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [1.5, 0, 0]}, "Collider2D": {"shape": "circle", "radius": 0.25}, "RigidBody2D": {}}})J");
    Call(e, "sim.step", R"J({"frames": 120})J");
    CHECK(std::fabs(PosOf(e, "Ball").y + 1.75f) < 0.02f);
    // Lua sees the same rules.
    Json r = Call(e, "script.eval", R"J({"code": "local w, h = tilemap.size('Map') return {tilemap.collision('Map', 0, 2), tilemap.solid('Map', 0, 1), w, h}"})J");
    CHECK(r["result"]["value"][0].asString() == "solid" && !r["result"]["value"][1].asBool());
    CHECK(r["result"]["value"][2].asInt() == 4 && r["result"]["value"][3].asInt() == 3);
    CHECK(Call(e, "physics.state", "{}")["result"]["warnings"].size() == 0);
}

void Scene2D(Engine& e) {
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Ground", "components": {"Transform": {"position": [0, -0.5, 0], "scale": [40, 1, 1]}, "Collider2D": {}}})J");
}

TEST(Physics2DBodies) {
    Engine e;
    Scene2D(e);
    Call(e, "entity.create", R"J({"name": "Box", "components": {"Transform": {"position": [0, 3, 0], "rotation": [0, 0, 30]}, "Collider2D": {}, "RigidBody2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [3, 2, 0], "scale": [0.5, 0.5, 0.5]}, "Collider2D": {"shape": "circle"}, "RigidBody2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Tri", "components": {"Transform": {"position": [-3, 2, 5]}, "Collider2D": {"shape": "polygon", "points": [[-0.5, 0], [0.5, 0], [0, 1], [0, 0.5]]}, "RigidBody2D": {"fixedRotation": true}}})J");
    Call(e, "sim.step", R"J({"frames": 240})J");
    Vec3 box = PosOf(e, "Box");
    const Transform* bt = e.GetScene().Get<Transform>(e.GetScene().FindByName("Box"));
    CHECK(std::fabs(box.y - 0.5f) < 0.02f);                               // settled flat on a face
    CHECK(std::fabs(std::fmod(std::fabs(bt->rotation.z) + 1.0f, 90.0f) - 1.0f) < 1.0f);
    CHECK(bt->rotation.x == 0.0f && bt->rotation.y == 0.0f && box.z == 0.0f);  // only Z rotation, Z position kept
    CHECK(std::fabs(PosOf(e, "Ball").y - 0.25f) < 0.02f);                 // radius scaled by 0.5
    CHECK(std::fabs(PosOf(e, "Tri").y) < 0.02f && PosOf(e, "Tri").z == 5.0f);  // concave outline, z untouched
    Json st = Call(e, "physics.state", "{}")["result"]["world2D"];
    CHECK(st["dynamicBodies"].asInt() == 3 && st["staticBodies"].asInt() == 1);

    // Velocity and impulses from scripts; queries in the plane.
    Call(e, "component.set", R"J({"id": "Ball", "type": "RigidBody2D", "values": {"velocity": [0, 6, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 20})J");
    CHECK(PosOf(e, "Ball").y > 1.5f);
    Json hit = Call(e, "physics.raycast", R"J({"origin": [0, 5, 0], "direction": [0, -1, 0]})J")["result"];
    CHECK(hit["hit"].asBool() && hit["name"].asString() == "Box" && std::fabs(hit["point"][1].asNumber() - 1.0) < 0.03);
    CHECK(std::fabs(hit["normal"][1].asNumber() - 1.0) < 1e-3);
    hit = Call(e, "physics.raycast", R"J({"origin": [-10, 0.5, 3], "direction": [1, 0, 0]})J")["result"];
    CHECK(hit["name"].asString() == "Tri" && hit["point"][2].asNumber() == 3.0);  // 2D hit keeps the ray's z
    Json over = Call(e, "physics.overlap", R"J({"center": [0, 0.5, 0], "radius": 0.2})J")["result"];
    CHECK(over.size() == 1 && over[0]["name"].asString() == "Box");

    // Layers: a body on layer 2 that ignores layer 0 falls through the ground.
    Call(e, "entity.create", R"J({"name": "Ghost", "components": {"Transform": {"position": [8, 1, 0]}, "Collider2D": {"layer": 2, "ignoreLayers": [0]}, "RigidBody2D": {}}})J");
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(PosOf(e, "Ghost").y < -1.0f);
}

TEST(Physics2DCharacters) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("physics2d_char"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // Floor top at y = -4, a wall at x 9..10, a one-way platform (top y = -2) and a slope rising to the right.
    Call(e, "entity.create", R"J({"name": "Level", "components": {"Transform": {}, "Tilemap": {"map": [
        "            ",
        "            ",
        "   ===      ",
        "         /# ",
        "############"], "legend": {"=": {"collision": "oneway"}, "/": {"collision": "slope-up"}}, "solid": "#"}}})J");
    Call(e, "entity.create", R"J({"name": "Hero", "components": {"Transform": {"position": [1, -2, 0]}, "CharacterBody2D": {"radius": 0.3, "height": 1}}})J");
    const CharacterBody2D* cb = [&] { return e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero")); }();
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(std::fabs(PosOf(e, "Hero").y + 3.5f) < 0.02f);  // standing on the floor
    cb = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero"));
    CHECK(cb->grounded);

    // Jump up through the one-way platform, land on top of it.
    Call(e, "component.set", R"J({"id": "Hero", "type": "Transform", "values": {"position": [4, -3.5, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 2})J");
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"velocity": [0, 9, 0]}})J");
    Call(e, "sim.step", R"J({"frames": 150})J");
    cb = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero"));
    CHECK(std::fabs(PosOf(e, "Hero").y + 1.5f) < 0.03f && cb->grounded);
    // Drop through it.
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"dropThrough": true}})J");
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(std::fabs(PosOf(e, "Hero").y + 3.5f) < 0.03f);
    Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"dropThrough": false}})J");

    // Walk right: up the slope onto the block (top y = -3), then into... the map edge is open, so it walks off.
    Call(e, "component.set", R"J({"id": "Hero", "type": "Transform", "values": {"position": [7, -3.5, 0]}})J");
    bool climbed = false;
    for (int i = 0; i < 40; ++i) {
        Call(e, "component.set", R"J({"id": "Hero", "type": "CharacterBody2D", "values": {"velocity": [3, 0, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
        if (PosOf(e, "Hero").x > 10.0f && PosOf(e, "Hero").y > -2.6f) climbed = true;
    }
    CHECK(climbed);
    cb = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Hero"));
    CHECK(cb->velocity.y <= 0.0f);  // never launched upward by the ramp

    // A wall stops the character; top-down mode has no gravity.
    Call(e, "entity.create", R"J({"name": "Wall", "components": {"Transform": {"position": [3, 5, 0], "scale": [1, 4, 1]}, "Collider2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Top", "components": {"Transform": {"position": [0, 5, 0]}, "CharacterBody2D": {"mode": "topdown", "shape": "circle", "radius": 0.5}}})J");
    for (int i = 0; i < 30; ++i) {
        Call(e, "component.set", R"J({"id": "Top", "type": "CharacterBody2D", "values": {"velocity": [4, 0, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
    }
    Vec3 top = PosOf(e, "Top");
    CHECK(std::fabs(top.x - 2.0f) < 0.03f && std::fabs(top.y - 5.0f) < 1e-3f);
    const CharacterBody2D* tc = e.GetScene().Get<CharacterBody2D>(e.GetScene().FindByName("Top"));
    CHECK(tc->onWall && !tc->grounded);

    // Triggers see characters; collision callbacks fire.
    Call(e, "script.write", R"J({"path": "scripts/coin.lua", "source": "local M = {}\nfunction M:onTriggerEnter(other) coins = (coins or 0) + 1 end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Coin", "components": {"Transform": {"position": [0, 7, 0]}, "Collider2D": {"shape": "circle", "radius": 0.3, "isTrigger": true}, "Script": {"path": "scripts/coin.lua"}}})J");
    for (int i = 0; i < 30; ++i) {
        Call(e, "component.set", R"J({"id": "Top", "type": "CharacterBody2D", "values": {"velocity": [-2, 2, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
    }
    CHECK(Call(e, "script.eval", R"J({"code": "coins"})J")["result"]["value"].asInt() == 1);
    CHECK(e.Scripts().Errors().empty());
}

TEST(Physics2DPlatformsAndPushing) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("physics2d_platform"), &err));
    Scene2D(e);
    // A kinematic platform moved by a script carries the character standing on it.
    Call(e, "script.write", R"J({"path": "scripts/mover.lua", "source": "local M = {}\nfunction M:onUpdate(dt)\n  local p = scene.get(self.id, 'Transform').position\n  scene.set(self.id, 'Transform', {position = {x = p.x + 2 * dt, y = p.y, z = 0}})\nend\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Lift", "components": {"Transform": {"position": [-10, 2, 0], "scale": [3, 0.4, 1]}, "Collider2D": {}, "RigidBody2D": {"type": "kinematic"}, "Script": {"path": "scripts/mover.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Rider", "components": {"Transform": {"position": [-10, 3, 0]}, "CharacterBody2D": {"radius": 0.3, "height": 1}}})J");
    Call(e, "sim.step", R"J({"frames": 30})J");
    float x0 = PosOf(e, "Rider").x;
    Call(e, "sim.step", R"J({"frames": 60})J");
    CHECK(std::fabs(PosOf(e, "Rider").x - x0 - 2.0f) < 0.05f);  // moved 2 m with the lift in 1 s
    CHECK(std::fabs(PosOf(e, "Rider").y - 2.7f) < 0.03f);

    // Walking into a crate pushes it; a heavy one barely moves.
    Call(e, "entity.create", R"J({"name": "Crate", "components": {"Transform": {"position": [3, 0.4, 0], "scale": [0.8, 0.8, 1]}, "Collider2D": {}, "RigidBody2D": {}}})J");
    Call(e, "entity.create", R"J({"name": "Safe", "components": {"Transform": {"position": [9, 0.4, 0], "scale": [0.8, 0.8, 1]}, "Collider2D": {}, "RigidBody2D": {"mass": 500}}})J");
    Call(e, "entity.create", R"J({"name": "Pusher", "components": {"Transform": {"position": [1, 0.5, 0]}, "CharacterBody2D": {"radius": 0.3, "height": 1}}})J");
    for (int i = 0; i < 90; ++i) {
        Call(e, "component.set", R"J({"id": "Pusher", "type": "CharacterBody2D", "values": {"velocity": [3, 0, 0]}})J");
        Call(e, "sim.step", R"J({"frames": 1})J");
    }
    CHECK(PosOf(e, "Crate").x > 5.5f);
    CHECK(std::fabs(PosOf(e, "Crate").y - 0.4f) < 0.03f);
    CHECK(PosOf(e, "Safe").x < 9.3f);
    CHECK(Call(e, "physics.contacts", R"J({"id": "Pusher"})J")["result"].size() >= 2);  // ground + a crate
}

TEST(Physics2DIsDeterministic) {
    auto run = [] {
        Engine e;
        Scene2D(e);
        Call(e, "entity.create", R"J({"name": "Slope", "components": {"Transform": {"position": [0, 0, 0]}, "Collider2D": {"shape": "edge", "points": [[-8, 6], [-2, 1], [4, 0]]}}})J");
        for (int i = 0; i < 8; ++i) {
            Json args = Json::parse(R"J({"components": {"Transform": {}, "Collider2D": {}, "RigidBody2D": {}}})J");
            args["name"] = "B" + std::to_string(i);
            args["components"]["Transform"]["position"] = Json(Json::Array{-6.0 + 0.4 * i, 7.0 + 1.1 * i, 0.0});
            args["components"]["Transform"]["rotation"] = Json(Json::Array{0.0, 0.0, 17.0 * i});
            if (i % 2) args["components"]["Collider2D"] = Json::parse(R"J({"shape": "circle", "radius": 0.4, "bounciness": 0.3})J");
            e.Call("entity.create", args);
        }
        Call(e, "entity.create", R"J({"name": "Hero", "components": {"Transform": {"position": [6, 1, 0]}, "CharacterBody2D": {"velocity": [-3, 0, 0]}}})J");
        Call(e, "sim.step", R"J({"frames": 300})J");
        return e.GetScene().ToJson().dump();
    };
    std::string a = run(), b = run();
    CHECK(a == b);
    // Compare this line between builds (native SSE2/NEON vs WebAssembly scalar): Box2D is cross-platform deterministic.
    uint64_t h = 1469598103934665603ull;
    for (char c : a) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
    std::printf("  2D scene hash %016llx\n", static_cast<unsigned long long>(h));
}

TEST(DungeonSamplePlays) {
    auto run = [](std::string* summary) {
        Engine e;
        std::string err;
        CHECK(e.Open(TestSourceDir() + "/samples/Dungeon", &err));
        Call(e, "sim.step", R"J({"frames": 5})J");
        auto eval = [&](const char* code) {
            Json a = Json::MakeObject();
            a["code"] = code;
            return e.Call("script.eval", a)["result"]["value"];
        };
        CHECK(eval("return game.get('coinsTotal')").asInt() == 8);
        // The level reads its rules from tilesets/dungeon.tileset.json.
        Json info = Call(e, "tilemap.info", R"J({"id": "Level"})J")["result"];
        CHECK(info["width"].asInt() == 36 && info["height"].asInt() == 20 && !info.has("error"));
        // Walk right, then shoot the slime east of the start twice.
        Call(e, "component.set", R"J({"id": "Player", "type": "Transform", "values": {"position": [22.5, -3.5, 0.1]}})J");
        Call(e, "input.key", R"J({"key": "D", "down": true})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
        Call(e, "input.key", R"J({"key": "D", "down": false})J");
        Call(e, "sim.step", R"J({"frames": 10})J");
        for (int shot = 0; shot < 2; ++shot) {
            Call(e, "input.key", R"J({"key": "Space", "down": true})J");
            Call(e, "sim.step", R"J({"frames": 1})J");
            Call(e, "input.key", R"J({"key": "Space", "down": false})J");
            Call(e, "sim.step", R"J({"frames": 25})J");
        }
        Call(e, "sim.step", R"J({"frames": 15})J");
        CHECK(eval("return #scene.withTag('enemy')").asInt() == 5);
        // Coins are triggers the hero collects.
        Call(e, "component.set", R"J({"id": "Player", "type": "Transform", "values": {"position": [29.5, -1.5, 0.1]}})J");
        Call(e, "sim.step", R"J({"frames": 5})J");
        CHECK(eval("return #scene.withTag('coin')").asInt() == 7);
        CHECK(eval("return scene.get(scene.find('CoinsText'), 'UIText').text").asString() == "COINS 1/8");
        // The stairs refuse to end the level early.
        Call(e, "component.set", R"J({"id": "Player", "type": "Transform", "values": {"position": [28.5, -14.5, 0.1]}})J");
        Call(e, "sim.step", R"J({"frames": 5})J");
        CHECK(eval("return scene.get(scene.find('Note'), 'UIText').text").asString() == "7 COINS LEFT");
        CHECK(e.Scripts().Errors().empty());
        CHECK(Call(e, "physics.state", "{}")["result"]["warnings"].size() == 0);
        Json shot = Call(e, "render.screenshot", R"J({"width": 128, "height": 72, "inline": false})J");
        *summary = shot["result"]["hash"].asString() + e.GetScene().ToJson().dump();
    };
    std::string a, b;
    run(&a);
    run(&b);
    CHECK(a == b);
}

TEST(HD2DSamplePlays) {
    // Billboard sprites in a 3D world: walk, collect the five shards, report to the elder, switch the lens effects.
    auto run = [](std::string* summary) {
        Engine e;
        std::string err;
        CHECK(e.Open(TestSourceDir() + "/samples/HD2D", &err));
        Call(e, "sim.step", R"J({"frames": 5})J");
        auto eval = [&](const std::string& code) {
            Json a = Json::MakeObject();
            a["code"] = code;
            return e.Call("script.eval", a)["result"]["value"];
        };
        auto tap = [&](const char* key, int frames) {
            Json a = Json::MakeObject();
            a["key"] = key;
            a["down"] = true;
            e.Call("input.key", a);
            Call(e, "sim.step", R"J({"frames": 1})J");
            a["down"] = false;
            e.Call("input.key", a);
            Json step = Json::MakeObject();
            step["frames"] = frames;
            e.Call("sim.step", step);
        };
        auto moveHero = [&](float x, float z) {
            Json a = Json::parse(R"J({"id": "Hero", "type": "Transform", "values": {"position": [0, 0.6, 0]}})J");
            a["values"]["position"][0] = x;
            a["values"]["position"][2] = z;
            CHECK(e.Call("component.set", a)["ok"].asBool());
            Call(e, "sim.step", R"J({"frames": 5})J");
        };
        const Scene& scene = e.GetScene();
        const EntityId heroSprite = scene.FindByName("HeroSprite");
        CHECK(scene.Get<Sprite>(heroSprite)->castShadows);
        // Billboards are project code (scripts/billboards.lua): upright sprites take the camera's yaw,
        // camera-facing ones its pitch as well.
        const Vec3 look = scene.Get<Transform>(scene.FindByName("Camera"))->rotation;
        CHECK(look.x < -30 && Near(scene.Get<Transform>(heroSprite)->rotation, Vec3(0, look.y, 0), 1e-3f));
        CHECK(Near(scene.Get<Transform>(scene.FindByName("Tree 1"))->rotation, Vec3(0, look.y, 0), 1e-3f));
        CHECK(Near(scene.Get<Transform>(scene.FindByName("Crystal 1"))->rotation, Vec3(look.x, look.y, 0), 0.5f));
        CHECK(eval("return #scene.withTag('crystal')").asInt() == 5 && eval("return game.get('crystals')").asInt() == 0);

        // Walking picks the side row and mirrors it for left.
        const float startX = scene.Get<Transform>(scene.FindByName("Hero"))->position.x;
        Call(e, "input.key", R"J({"key": "D", "down": true})J");
        Call(e, "sim.step", R"J({"frames": 30})J");
        CHECK(scene.Get<Transform>(scene.FindByName("Hero"))->position.x > startX + 1.0f);
        CHECK(scene.Get<SpriteAnimation>(heroSprite)->clip == "walk_side" && !scene.Get<Sprite>(heroSprite)->flipX);
        Call(e, "input.key", R"J({"key": "D", "down": false})J");
        Call(e, "input.key", R"J({"key": "A", "down": true})J");
        Call(e, "sim.step", R"J({"frames": 5})J");
        CHECK(scene.Get<Sprite>(heroSprite)->flipX);
        Call(e, "input.key", R"J({"key": "A", "down": false})J");
        Call(e, "sim.step", R"J({"frames": 2})J");
        CHECK(scene.Get<SpriteAnimation>(heroSprite)->clip == "idle_side");

        // A shard is collected by walking into it.
        moveHero(-6.5f, 1.5f);
        CHECK(eval("return game.get('crystals')").asInt() == 1 && eval("return #scene.withTag('crystal')").asInt() == 4);
        CHECK(eval("return scene.get(scene.find('Quest'), 'UIText').text").asString() == "Crystals 1 / 5");
        CHECK(Call(e, "audio.state", "{}")["result"].dump().find("crystal.wav") != std::string::npos);

        // The elder explains the quest; the hero stands still while the dialog is open.
        moveHero(2.4f, -0.2f);
        CHECK(eval("return scene.get(scene.find('Prompt'), 'UIText').visible").asBool());
        tap("E", 3);
        CHECK(eval("return game.get('dialog')").asBool() && eval("return scene.get(scene.find('Dialog'), 'UIPanel').visible").asBool());
        const float talkX = scene.Get<Transform>(scene.FindByName("Hero"))->position.x;
        Call(e, "input.key", R"J({"key": "D", "down": true})J");
        Call(e, "sim.step", R"J({"frames": 20})J");
        Call(e, "input.key", R"J({"key": "D", "down": false})J");
        CHECK(std::fabs(scene.Get<Transform>(scene.FindByName("Hero"))->position.x - talkX) < 0.01f);
        auto finishDialog = [&] {
            for (int press = 0; press < 12 && eval("return game.get('dialog')").asBool(); ++press) tap("E", 3);
            CHECK(!eval("return game.get('dialog')").asBool());
        };
        finishDialog();
        CHECK(eval("return scene.get(scene.find('Quest'), 'UIText').text").asString() == "Crystals 1 / 5");

        const float shards[4][2] = {{8.5f, -1.2f}, {0, -8.2f}, {-11.5f, 9.5f}, {16.2f, 5.4f}};
        for (const auto& shard : shards) moveHero(shard[0], shard[1]);
        CHECK(eval("return game.get('crystals')").asInt() == 5 && eval("return #scene.withTag('crystal')").asInt() == 0);
        moveHero(2.4f, -0.2f);
        tap("E", 3);
        finishDialog();
        CHECK(eval("return scene.get(scene.find('Quest'), 'UIText').text").asString().find("lanterns") != std::string::npos);

        // Keys 2 and 1 switch depth of field and the whole lens look.
        const PostProcess* post = scene.Get<PostProcess>(scene.FindByName("Camera"));
        CHECK(post->dofRadius == 6 && post->bloom > 0);
        tap("2", 1);
        CHECK(post->dofRadius == 0 && post->bloom > 0);
        tap("2", 1);
        CHECK(post->dofRadius == 6);
        tap("1", 1);
        CHECK(post->dofRadius == 0 && post->bloom == 0 && post->toneMapping == "none");
        tap("1", 1);
        CHECK(post->dofRadius == 6 && post->toneMapping == "reinhard");

        CHECK(e.Scripts().Errors().empty());
        CHECK(Call(e, "physics.state", "{}")["result"]["warnings"].size() == 0);
        Json shot = Call(e, "render.screenshot", R"J({"width": 160, "height": 90, "inline": false})J");
        *summary = shot["result"]["hash"].asString() + e.GetScene().ToJson().dump();
    };
    std::string a, b;
    run(&a);
    run(&b);
    CHECK(a == b);
}

TEST(WaterSamplePlays) {
    // A sea whose shader graph lifts the vertices; Lua floats ride the same waves.
    auto run = [](std::string* summary) {
        Engine e;
        std::string err;
        CHECK(e.Open(TestSourceDir() + "/samples/Water", &err));
        Call(e, "sim.step", R"J({"frames": 45})J");
        auto eval = [&](const std::string& code) {
            Json a = Json::MakeObject();
            a["code"] = code;
            return e.Call("script.eval", a)["result"]["value"];
        };
        const Scene& scene = e.GetScene();
        ShaderGraph graph;
        CHECK(CompileShaderGraph(e.ReadProjectJson("materials/water.shader.json"), graph, &err));
        CHECK(graph.offset >= 0 && graph.normal >= 0);
        const EntityId water = scene.FindByName("Water"), buoy = scene.FindByName("Buoy 1");
        CHECK(scene.Get<MeshRenderer>(water)->mesh == "plane64");
        // Height of the drawn surface at a world position: the graph's vertex stage with the entity's uniforms.
        auto surface = [&](float x, float z) {
            std::array<Vec4, ShaderGraph::kMaxUniforms> uniforms;
            CHECK(ShaderUniforms(graph, scene.Get<MeshRenderer>(water)->shaderUniforms, uniforms, &err));
            ShaderInputs inputs;
            inputs.position = Vec4(x, 0, z, 1);
            inputs.time = static_cast<float>(e.SimTime());
            return ShaderVertexOffset(graph, inputs, uniforms).y;
        };
        auto buoyOnSurface = [&] {
            const Vec3 p = scene.Get<Transform>(buoy)->position;
            return std::fabs(p.y - 0.12f - surface(p.x, p.z)) < 2e-3f;
        };
        CHECK(buoyOnSurface());
        const float before = scene.Get<Transform>(buoy)->position.y;
        Call(e, "sim.step", R"J({"frames": 40})J");
        CHECK(buoyOnSurface() && std::fabs(scene.Get<Transform>(buoy)->position.y - before) > 0.02f);
        CHECK(std::fabs(scene.Get<Transform>(scene.FindByName("Boat"))->rotation.z) > 0.5f);  // leans with the slope

        // Key 3: the storm's amplitudes ease in and reach the graph.
        CHECK(eval("return game.get('seaState')").asString() == "Swell");
        const float swell = scene.Get<MeshRenderer>(water)->shaderUniforms["amp"][0].asFloat();
        CHECK(std::fabs(swell - 0.16f) < 1e-4f);
        Call(e, "input.key", R"J({"key": "3", "down": true})J");
        Call(e, "sim.step", R"J({"frames": 1})J");
        Call(e, "input.key", R"J({"key": "3", "down": false})J");
        Call(e, "sim.step", R"J({"frames": 300})J");
        CHECK(eval("return game.get('seaState')").asString() == "Storm");
        CHECK(scene.Get<UIText>(scene.FindByName("Title"))->text == "Sea state: Storm");
        CHECK(scene.Get<MeshRenderer>(water)->shaderUniforms["amp"][0].asFloat() > 0.16f * 1.8f);
        CHECK(buoyOnSurface());

        // A / D orbit the camera around the scene.
        const EntityId camera = scene.FindByName("Camera");
        const Vec3 offset = scene.Get<CameraFollow>(camera)->offset;
        Call(e, "input.key", R"J({"key": "D", "down": true})J");
        Call(e, "sim.step", R"J({"frames": 40})J");
        Call(e, "input.key", R"J({"key": "D", "down": false})J");
        const Vec3 turned = scene.Get<CameraFollow>(camera)->offset;
        CHECK(turned.x > offset.x + 3 && std::fabs(Length(Vec3(turned.x, 0, turned.z)) - Length(Vec3(offset.x, 0, offset.z))) < 1e-3f);

        CHECK(e.Scripts().Errors().empty());
        Json shot = Call(e, "render.screenshot", R"J({"width": 160, "height": 90, "inline": false})J");
        *summary = shot["result"]["hash"].asString() + e.GetScene().ToJson().dump();
    };
    std::string a, b;
    run(&a);
    run(&b);
    CHECK(a == b);
}

TEST(SampleGamepadControls) {
    auto run = [](const char* sample) {
        Engine e;
        std::string err;
        CHECK(e.Open(TestSourceDir() + "/samples/" + sample, &err));
        Call(e, "sim.step", R"J({"frames":60})J");
        float initialX = e.GetScene().Get<Transform>(e.GetScene().FindByName("Player"))->position.x;
        auto velocity = [&] {
            EntityId player = e.GetScene().FindByName("Player");
            if (auto* body = e.GetScene().Get<CharacterBody2D>(player)) return body->velocity;
            return e.GetScene().Get<CharacterBody>(player)->velocity;
        };
        const bool platformer = std::string(sample) == "Platformer";
        const float speed = platformer ? 7.0f : 5.0f;
        Call(e, "input.axis", R"J({"name":"LeftX","value":0.575})J");
        // Android's legacy arrow alias must not turn a partial stick into full speed.
        Call(e, "input.key", R"J({"key":"Right","down":true})J");
        Call(e, "sim.step", R"J({"frames":8})J");
        CHECK(std::fabs(velocity().x - speed * 0.5f) < 0.02f);
        Call(e, "input.key", R"J({"key":"Right","down":false})J");
        Call(e, "input.axis", R"J({"name":"LeftX","value":0.1})J");
        Call(e, "sim.step", R"J({"frames":12})J");
        CHECK(std::fabs(velocity().x) < 0.02f);
        Call(e, "input.key", R"J({"key":"GamepadDPadRight","down":true})J");
        Call(e, "sim.step", R"J({"frames":12})J");
        CHECK(std::fabs(velocity().x - speed) < 0.02f);
        Call(e, "input.key", R"J({"key":"GamepadDPadRight","down":false})J");
        Call(e, "input.axis", R"J({"name":"LeftX","value":0})J");
        Call(e, "sim.step", R"J({"frames":12})J");
        Call(e, "input.key", R"J({"key":"GamepadA","down":true})J");
        Call(e, "sim.step", R"J({"frames":1})J");
        if (platformer) CHECK(velocity().y > 10);  // gamepad jump uses the real character mover
        else CHECK(e.GetScene().FindByName("Bolt") != kNullEntity);
        Call(e, "sim.step", R"J({"frames":3})J");
        if (platformer) CHECK(velocity().y > 8);  // held A preserves a high jump
        Call(e, "input.key", R"J({"key":"GamepadA","down":false})J");
        Call(e, "sim.step", R"J({"frames":1})J");
        if (platformer) CHECK(velocity().y < 8);  // releasing A cuts the jump
        CHECK(e.Scripts().Errors().empty());
        std::string state = e.GetScene().ToJson().dump();
        Call(e, "input.key", R"J({"key":"GamepadStart","down":true})J");
        Call(e, "sim.step", R"J({"frames":2})J");
        CHECK(e.GetScene().FindByName("Player") != kNullEntity);
        CHECK(std::fabs(e.GetScene().Get<Transform>(e.GetScene().FindByName("Player"))->position.x - initialX) < 0.01f);
        Call(e, "input.key", R"J({"key":"GamepadStart","down":false})J");
        CHECK(e.Scripts().Errors().empty());
        return state;
    };
    for (const char* sample : {"Platformer", "Dungeon"}) CHECK(run(sample) == run(sample));
}

// ----- assets & rendering ---------------------------------------------------------

size_t CountId(const RenderTarget& rt, EntityId id) {
    size_t n = 0;
    for (EntityId v : rt.ids) n += v == id;
    return n;
}

double MeanLuma(const RenderTarget& rt) {
    double sum = 0;
    for (uint32_t c : rt.color) sum += (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF);
    return sum / (3.0 * static_cast<double>(rt.color.size()));
}

TEST(SampleParticleEffects) {
    for (const char* sample : {"Platformer", "Dungeon"}) {
        auto run = [&](bool writeImage) {
            Engine e;
            std::string err;
            CHECK(e.Open(TestSourceDir() + "/samples/" + sample, &err));
            Call(e, "sim.step", R"J({"frames":2})J");
            Json eval = Call(e, "script.eval", R"J({"code":"return scene.withTag('coin')[1]"})J");
            EntityId coin = static_cast<EntityId>(eval["result"]["value"].asInt());
            CHECK(coin != kNullEntity);
            Vec3 position = e.GetScene().Get<Transform>(coin)->position;
            Json teleport = Json::parse(R"J({"id":"Player","type":"Transform","values":{}})J");
            teleport["values"]["position"] = Json(Json::Array{position.x, position.y, 0.1});
            CHECK(e.Call("component.set", teleport)["ok"].asBool());
            Call(e, "sim.step", R"J({"frames":2})J");
            CHECK(!e.GetScene().Exists(coin));
            CHECK(!e.GetScene().Pool<ParticleEmitter>().empty());
            std::vector<EntityId> effects;
            for (const auto& kv : e.GetScene().Pool<ParticleEmitter>()) {
                effects.push_back(kv.first);
                CHECK(kv.second.space == "world" && kv.second.dimensions == 2 && !kv.second.loop);
                CHECK(kv.second.emitted == 12 && kv.second.particles.size() == 12);
            }
            Call(e, "sim.step", R"J({"frames":6})J");
            // Focus the active orthographic camera without waiting for its smoothing.
            for (const auto& kv : e.GetScene().Pool<CameraFollow>()) {
                Json cameraArgs = Json::parse(R"J({"type":"CameraFollow","values":{"smoothing":0}})J");
                cameraArgs["id"] = kv.first;
                CHECK(e.Call("component.set", cameraArgs)["ok"].asBool());
            }
            Call(e, "sim.step", R"J({"frames":1})J");
            RenderTarget shot;
            shot.Resize(640,360);
            e.RenderGameView(shot);
            size_t visible = 0;
            for (EntityId effect : effects) visible += CountId(shot, effect);
            CHECK(visible > 10);
            if (writeImage) CHECK(WritePng(std::string("build/particle-") + sample + ".png", shot.ToImage(), true));
            std::string summary = e.GetScene().ToJson().dump() + std::to_string(shot.Hash());
            std::string hit = std::string(sample) == "Dungeon" ?
                "local id=scene.withTag('enemy')[1]; scene.send(id,'hit',{x=1,y=0}); scene.send(scene.find('Player'),'hurt',id)" :
                "scene.send(scene.withTag('enemy')[1],'squash'); scene.send(scene.find('Player'),'die')";
            Json hitArgs = Json::MakeObject();
            hitArgs["code"] = hit;
            CHECK(e.Call("script.eval", hitArgs)["ok"].asBool());
            int hitEffects = 0;
            for (const auto& kv : e.GetScene().Pool<ParticleEmitter>()) {
                if (kv.second.emitted == 16) { ++hitEffects; effects.push_back(kv.first); }
            }
            CHECK(hitEffects == 2);
            Call(e, "sim.step", R"J({"frames":45})J");
            for (EntityId effect : effects) CHECK(!e.GetScene().Exists(effect));
            CHECK(e.Scripts().Errors().empty());
            Json checked = Call(e, "script.check", R"J({"path":"scripts/effects.lua"})J");
            CHECK(checked["ok"].asBool() && checked["result"]["errors"].asInt() == 0 && checked["result"]["warnings"].asInt() == 0);
            return summary;
        };
        std::string first = run(true);
        CHECK(first == run(false));
    }
}

RenderTarget RenderLook(Engine& e, Vec3 eye, Vec3 target, int w = 160, int h = 90) {
    RenderTarget rt;
    rt.Resize(w, h);
    RenderView v = MakeLookAtView(eye, target, 50.0f, static_cast<float>(w) / static_cast<float>(h));
    e.Renderer().Render(e.GetScene(), v, rt);
    return rt;
}

TEST(TexturesAndGltfModels) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("assets"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // PNG encode -> decode round trip.
    Image img;
    img.width = 3;
    img.height = 2;
    img.rgba = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255};
    std::vector<uint8_t> png = EncodePng(img);
    Texture tex;
    CHECK(DecodeImage(png.data(), png.size(), tex, &err) && tex.width == 3 && tex.height == 2);
    CHECK(tex.texels[1] == 0xFF00FF00u && tex.texels[5] == 0xFF5A5046u);
    CHECK(Call(e, "asset.generate_texture", R"J({"path": "assets/textures/c.png", "pattern": "checker", "size": 64})J")["ok"].asBool());
    CHECK(Call(e, "asset.info", R"J({"path": "assets/textures/c.png"})J")["result"]["width"].asInt() == 64);

    // A 1x1 m quad glTF with an embedded 1x1 red PNG (data URIs), written by hand.
    std::vector<uint8_t> bin;
    auto putF = [&](float f) { uint8_t b[4]; std::memcpy(b, &f, 4); bin.insert(bin.end(), b, b + 4); };
    for (float v : {-0.5f, -0.5f, 0.f, 0.5f, -0.5f, 0.f, 0.5f, 0.5f, 0.f, -0.5f, 0.5f, 0.f}) putF(v);  // positions (48 bytes)
    for (float v : {0.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f}) putF(v);                                   // uvs (32 bytes)
    for (int i : {0, 1, 2, 0, 2, 3}) { bin.push_back(static_cast<uint8_t>(i)); bin.push_back(0); }  // indices (12 bytes)
    Image red;
    red.width = red.height = 1;
    red.rgba = {255, 0, 0, 255};
    std::string gltf = std::string(R"J({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
      "meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],
      "materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],
      "textures":[{"source":0}],"images":[{"uri":"data:image/png;base64,)J") + Base64Encode(EncodePng(red)) +
                       R"J("}],"buffers":[{"byteLength":92,"uri":"data:application/octet-stream;base64,)J" + Base64Encode(bin) + R"J("}],
      "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":32},{"buffer":0,"byteOffset":80,"byteLength":12}],
      "accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-0.5,-0.5,0],"max":[0.5,0.5,0]},
                   {"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},
                   {"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}]})J";
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(), "assets/models/quad.gltf"), gltf));
    Json info = Call(e, "asset.info", R"J({"path": "assets/models/quad.gltf"})J")["result"];
    CHECK(info["triangles"].asInt() == 2 && info["textures"].size() == 1 && info["submeshes"][0]["texture"].asInt() == 0);
    CHECK(std::fabs(info["size"][0].asNumber() - 1.0) < 1e-6);
    Call(e, "entity.create", R"J({"name": "Quad", "components": {"MeshRenderer": {"mesh": "assets/models/quad.gltf", "color": [1, 1, 1], "unlit": true}}})J");
    RenderTarget rt = RenderLook(e, Vec3(0, 0, 2), Vec3(0, 0, 0));
    CHECK(rt.color[45 * 160 + 80] == 0xFF0000FFu);  // textured red, unlit
    // A texture override replaces the model's texture.
    Call(e, "component.set", R"J({"id": "Quad", "type": "MeshRenderer", "values": {"texture": "assets/textures/c.png"}})J");
    rt = RenderLook(e, Vec3(0, 0, 2), Vec3(0, 0, 0));
    CHECK(rt.color[45 * 160 + 80] != 0xFF0000FFu);
    // Missing models render as a magenta error cube instead of disappearing.
    Call(e, "component.set", R"J({"id": "Quad", "type": "MeshRenderer", "values": {"mesh": "assets/models/missing.glb"}})J");
    rt = RenderLook(e, Vec3(0, 0, 3), Vec3(0, 0, 0));
    CHECK(rt.color[45 * 160 + 80] == 0xFFFF00FFu);
    CHECK(Call(e, "asset.list", R"J({"kind": "model"})J")["result"].size() == 1);
}

TEST(GltfSkinAndAnimationLoading) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("animation_loader"), &err));
    Json source = Json::parse(R"J({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[1]}],
      "nodes":[{"mesh":0,"skin":0},{"translation":[3,0,0],"children":[0,2]},{"translation":[0,2,0]}],
      "skins":[{"joints":[1,2]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]}],
      "animations":[{"name":"Move","samplers":[{"input":3,"output":4,"interpolation":"STEP"}],
        "channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]},
        {"name":"Turn","samplers":[{"input":3,"output":5}],"channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}}]},
        {"name":"Move","samplers":[{"input":3,"output":6,"interpolation":"CUBICSPLINE"}],
        "channels":[{"sampler":0,"target":{"node":2,"path":"scale"}}]}],"bufferViews":[],"accessors":[]})J");
    std::vector<uint8_t> bin;
    auto floats = [&](std::initializer_list<float> values) {
        std::vector<uint8_t> bytes;
        for (float value : values) {
            uint8_t encoded[4];
            std::memcpy(encoded, &value, 4);
            bytes.insert(bytes.end(), encoded, encoded + 4);
        }
        return bytes;
    };
    auto accessor = [&](const std::vector<uint8_t>& bytes, int component, int count, const char* type, bool normalized = false) {
        while (bin.size() % 4) bin.push_back(0);
        Json view = Json::MakeObject();
        view["buffer"] = 0;
        view["byteOffset"] = static_cast<uint64_t>(bin.size());
        view["byteLength"] = static_cast<uint64_t>(bytes.size());
        Json a = Json::MakeObject();
        a["bufferView"] = static_cast<uint64_t>(source["bufferViews"].size());
        a["componentType"] = component;
        a["count"] = count;
        a["type"] = type;
        if (normalized) a["normalized"] = true;
        source["bufferViews"].push(view);
        source["accessors"].push(a);
        bin.insert(bin.end(), bytes.begin(), bytes.end());
    };
    accessor(floats({0,0,0, 1,0,0, 0,1,0}), 5126, 3, "VEC3");
    source["accessors"][0]["min"] = Json::parse("[0,0,0]");
    source["accessors"][0]["max"] = Json::parse("[1,1,0]");
    accessor({0,1,0,0, 1,0,0,0, 0,0,0,0}, 5121, 3, "VEC4");
    accessor(floats({2,2,0,0, 1,0,0,0, 0,0,0,0}), 5126, 3, "VEC4");
    accessor(floats({0,2}), 5126, 2, "SCALAR");
    source["accessors"][3]["min"] = Json::parse("[0]");
    source["accessors"][3]["max"] = Json::parse("[2]");
    accessor(floats({3,0,0, 5,0,0}), 5126, 2, "VEC3");
    accessor({0,0,0,127, 0,0,127,0}, 5120, 2, "VEC4", true);
    accessor(floats({0,0,0, 1,1,1, 0,0,0, 0,0,0, 2,2,2, 0,0,0}), 5126, 6, "VEC3");
    const std::string path = JoinPath(e.ProjectDir(), "assets/models/rig.gltf");
    auto write = [&](Json model, const std::vector<uint8_t>& bytes) {
        Json buffer = Json::MakeObject();
        buffer["byteLength"] = static_cast<uint64_t>(bytes.size());
        buffer["uri"] = "data:application/octet-stream;base64," + Base64Encode(bytes);
        model["buffers"] = Json::MakeArray();
        model["buffers"].push(buffer);
        CHECK(WriteTextFile(path, model.dump()));
    };
    write(source, bin);
    Json info = Call(e, "asset.info", R"J({"path":"assets/models/rig.gltf"})J")["result"];
    CHECK(info["joints"].asInt() == 2 && info["clips"].size() == 3);
    CHECK(info["clips"][0]["name"].asString() == "Move" && info["clips"][2]["name"].asString() == "Move_1");
    CHECK(info["clips"][0]["duration"].asFloat() == 2 && info["clips"][0]["channels"].asInt() == 1);
    Mesh mesh;
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.nodes.size() == 3 && mesh.nodes[2].parent == 1 && mesh.joints[1].node == 2);
    CHECK(mesh.positions[0].x == 0 && mesh.skin[0].weights[0] == 0.5f && mesh.skin[0].weights[1] == 0.5f);
    CHECK(mesh.skin[2].weights[0] == 0 && mesh.joints[0].inverseBind.m[0] == 1);
    CHECK(mesh.clips[0].channels[0].interpolation == AnimationInterpolation::Step);
    CHECK(mesh.clips[1].channels[0].values[1].z == 1);
    CHECK(mesh.clips[2].channels[0].interpolation == AnimationInterpolation::CubicSpline);

    auto reject = [&](Json model, std::vector<uint8_t> bytes, const char* message) {
        write(model, bytes);
        CHECK(!LoadModelFile(path, mesh, &err));
        CHECK(err.find(message) != std::string::npos);
    };
    std::vector<uint8_t> broken = bin;
    broken[static_cast<size_t>(source["bufferViews"][1]["byteOffset"].asInt())] = 2;
    reject(source, broken, "joint index is outside");
    broken = bin;
    size_t times = static_cast<size_t>(source["bufferViews"][3]["byteOffset"].asInt());
    std::fill(broken.begin() + times + 4, broken.begin() + times + 8, uint8_t{0});
    reject(source, broken, "strictly increasing");
    broken = bin;
    float negative = -1;
    std::memcpy(broken.data() + source["bufferViews"][2]["byteOffset"].asInt(), &negative, 4);
    reject(source, broken, "finite and nonnegative");
    Json invalid = source;
    invalid["animations"][0]["channels"].push(invalid["animations"][0]["channels"][0]);
    reject(invalid, bin, "duplicate channels");
    invalid = source;
    invalid["meshes"][0]["primitives"][0]["attributes"]["JOINTS_1"] = 1;
    reject(invalid, bin, "four influences");
    invalid = source;
    invalid["skins"][0]["joints"] = Json::MakeArray();
    for (int i = 0; i < 65; ++i) invalid["skins"][0]["joints"].push(1);
    reject(invalid, bin, "64-entry");
    // Rigid child geometry is baked once, then receives a one-joint animation binding.
    invalid = source;
    invalid["nodes"][0].erase("skin");
    invalid["meshes"][0]["primitives"][0]["attributes"].erase("JOINTS_0");
    invalid["meshes"][0]["primitives"][0]["attributes"].erase("WEIGHTS_0");
    write(invalid, bin);
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.joints.size() == 1 && mesh.joints[0].node == 0 && mesh.positions[0].x == 3);
    CHECK(mesh.skin[0].weights[0] == 1 && mesh.joints[0].inverseBind.m[12] == -3);
    // Explicit inverse bind matrices survive loading, including their translations.
    accessor(floats({1,0,0,0, 0,1,0,0, 0,0,1,0, -3,0,0,1,
                     1,0,0,0, 0,1,0,0, 0,0,1,0, -3,-2,0,1}), 5126, 2, "MAT4");
    source["skins"][0]["inverseBindMatrices"] = 7;
    write(source, bin);
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.joints[1].inverseBind.m[12] == -3 && mesh.joints[1].inverseBind.m[13] == -2);
    invalid = source;
    invalid["accessors"][7]["count"] = 1;
    reject(invalid, bin, "inverse bind matrices");
    // Quantized unsigned skin weights decode and normalize in exactly the same path.
    invalid = source;
    invalid["accessors"][2]["componentType"] = 5121;
    invalid["accessors"][2]["normalized"] = true;
    broken = bin;
    size_t weightOffset = static_cast<size_t>(source["bufferViews"][2]["byteOffset"].asInt());
    for (size_t i = 0; i < 12; ++i) broken[weightOffset + i] = i % 4 < 2 ? 127 : 0;
    write(invalid, broken);
    CHECK(LoadModelFile(path, mesh, &err));
    CHECK(mesh.skin[0].weights[0] == 0.5f && mesh.skin[0].weights[1] == 0.5f);
    invalid["accessors"][2]["normalized"] = false;
    reject(invalid, broken, "normalized unsigned");
}

TEST(AnimationPoseInterpolation) {
    Mesh mesh;
    mesh.nodes.resize(2);
    mesh.nodes[0].parent = 1;  // parents may occur after children in the source
    mesh.nodes[0].scale = Vec3(2, 1, 1);
    mesh.nodes[1].usesMatrix = true;
    mesh.nodes[1].matrix = Mat4::Translation(Vec3(3, 0, 0));
    mesh.joints.push_back({0, Mat4::Translation(Vec3(-1, 0, 0))});
    AnimationChannel channel;
    channel.node = 0;
    channel.times = {0, 2};
    channel.values = {Vec4(0,0,0,0), Vec4(4,0,0,0)};
    AnimationClip clip;
    clip.channels.push_back(channel);
    auto sample = [&](float time) { return EvaluateAnimationPose(mesh, &clip, time)[0]; };
    CHECK(Near(sample(1).TransformPoint(Vec3(1,0,0)), Vec3(5,0,0), 1e-5f));
    CHECK(Near(sample(-8).TransformPoint(Vec3(1,0,0)), Vec3(3,0,0), 1e-5f));
    CHECK(Near(sample(8).TransformPoint(Vec3(1,0,0)), Vec3(7,0,0), 1e-5f));
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(2,0,0), 1e-5f));  // unanimated scale retained
    clip.channels[0].interpolation = AnimationInterpolation::Step;
    CHECK(Near(sample(1.999f).TransformPoint(Vec3(1,0,0)), Vec3(3,0,0), 1e-5f));
    CHECK(Near(sample(2).TransformPoint(Vec3(1,0,0)), Vec3(7,0,0), 1e-5f));
    clip.channels[0].interpolation = AnimationInterpolation::CubicSpline;
    clip.channels[0].values = {Vec4(), Vec4(), Vec4(2,0,0,0), Vec4(), Vec4(4,0,0,0), Vec4()};
    CHECK(Near(sample(1).TransformPoint(Vec3(1,0,0)), Vec3(5.5f,0,0), 1e-5f));  // tangent scaled by two seconds
    clip.channels[0].path = AnimationPath::Rotation;
    clip.channels[0].interpolation = AnimationInterpolation::Linear;
    clip.channels[0].values = {Vec4(0,0,0,1), Vec4(0,0,1,0)};
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(0,2,0), 1e-5f));
    // Quaternion signs represent the same rotation; interpolation takes the shortest arc.
    clip.channels[0].values[1] = Vec4(0,0,0,-1);
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(2,0,0), 1e-5f));
    clip.channels[0].interpolation = AnimationInterpolation::CubicSpline;
    clip.channels[0].values = {Vec4(), Vec4(0,0,0,1), Vec4(), Vec4(), Vec4(0,0,1,0), Vec4()};
    CHECK(Near(sample(1).TransformDir(Vec3(1,0,0)), Vec3(0,2,0), 1e-5f));  // interpolated quaternion normalized
    clip.channels[0].path = AnimationPath::Scale;
    clip.channels[0].interpolation = AnimationInterpolation::Linear;
    clip.channels[0].values = {Vec4(1,1,1,0), Vec4(3,3,3,0)};
    CHECK(Near(sample(1).TransformDir(Vec3(0,1,0)), Vec3(0,2,0), 1e-5f));
    CHECK(Near(EvaluateAnimationPose(mesh, nullptr, 0)[0].TransformPoint(Vec3(1,0,0)), Vec3(3,0,0), 1e-5f));
}

TEST(AnimatorCommandsAndLua) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("animator"), &err));
    CHECK(CopyFileTo(TestSourceDir() + "/samples/Showcase/assets/models/fox.glb", JoinPath(e.ProjectDir(), "assets/models/fox.glb")));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Fox","components":{"MeshRenderer":{"mesh":"assets/models/fox.glb"}}})J");
    EntityId id = e.GetScene().FindByName("Fox");
    CHECK(Call(e, "animation.play", R"J({"id":"Fox","clip":"missing"})J")["error"]["code"].asString() == "animation_not_found");
    CHECK(e.GetScene().Get<Animator>(id) == nullptr);
    CHECK(Call(e, "animation.play", R"J({"id":"Fox","clip":"Walk"})J")["ok"].asBool());
    Json saved = e.GetScene().ToJson();
    Scene restored;
    CHECK(restored.FromJson(saved, &err) && restored.Get<Animator>(id)->clip == "Walk");
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());
    CHECK(e.GetScene().Get<Animator>(id) == nullptr);
    CHECK(Call(e, "history.redo", "{}")["ok"].asBool());
    CHECK(e.GetScene().Get<Animator>(id)->clip == "Walk");
    Call(e, "sim.step", R"J({"frames":12})J");
    Json state = Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"];
    CHECK(std::fabs(state["time"].asFloat() - 0.2f) < 1e-6f && state["validClip"].asBool());
    CHECK(state["jointMatrices"].size() == 24 && state["jointMatrices"][0].size() == 16);
    Json pose = state["jointMatrices"];
    Call(e, "component.set", R"J({"id":"Fox","type":"Animator","values":{"playing":false}})J");
    Call(e, "sim.step", R"J({"frames":10})J");
    CHECK(Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"]["jointMatrices"] == pose);
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Walk","loop":false})J");
    Call(e, "sim.step", R"J({"frames":60})J");
    state = Call(e, "animation.state", R"J({"id":"Fox"})J")["result"];
    CHECK(!state["playing"].asBool() && state["time"] == state["duration"]);
    CHECK(Call(e, "script.eval", R"J({"code":"animation.play('Fox', 'Walk', {speed=-1, loop=false})"})J")["ok"].asBool());
    Call(e, "sim.step", R"J({"frames":60})J");
    state = Call(e, "animation.state", R"J({"id":"Fox"})J")["result"];
    CHECK(!state["playing"].asBool() && state["time"].asFloat() == 0);
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run","speed":-1,"loop":true})J");
    Call(e, "component.set", R"J({"id":"Fox","type":"Animator","values":{"time":0}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    state = Call(e, "animation.state", R"J({"id":"Fox"})J")["result"];
    CHECK(std::fabs(state["time"].asFloat() - state["duration"].asFloat() + 1.0f/60) < 1e-6f);
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run","speed":1})J");
    Call(e, "sim.step", R"J({"frames":24})J");
    Json first = Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"];
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run","restart":false})J");
    CHECK(e.GetScene().Get<Animator>(id)->time == first["time"].asFloat());
    Call(e, "animation.play", R"J({"id":"Fox","clip":"Run"})J");
    Call(e, "sim.step", R"J({"frames":24})J");
    CHECK(Call(e, "animation.state", R"J({"id":"Fox","pose":true})J")["result"] == first);
    CHECK(Call(e, "script.eval", R"J({"code":"local ok = pcall(animation.play, 'Fox', 'invalid'); assert(not ok)"})J")["ok"].asBool());
    // self:play follows the same command path from a real script instance.
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(), "scripts/animate.lua"), "return {onStart=function(self) self:play('Survey', {speed=0}) end}"));
    Call(e, "component.add", R"J({"id":"Fox","type":"Script","values":{"path":"scripts/animate.lua"}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(e.GetScene().Get<Animator>(id)->clip == "Survey" && e.GetScene().Get<Animator>(id)->time == 0);
    CHECK(e.Scripts().Errors().empty());
}

TEST(ParticleSimulationAndCommands) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("particles"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "entity.create", R"J({"name":"Effect","components":{"Transform":{"position":[3,4,0]},"ParticleEmitter":{"rate":0,"burst":2,"lifetime":0.5,"speed":2,"spread":0,"gravity":[0,-2,0],"maxParticles":3,"dimensions":2,"loop":false}}})J")["ok"].asBool());
    EntityId id = e.GetScene().FindByName("Effect");
    Call(e, "sim.step", R"J({"frames":1})J");
    Json state = Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    CHECK(state["count"].asInt() == 2 && state["emitted"].asInt() == 2);
    CHECK(state["particles"][0]["age"].asFloat() == 0 && state["particles"][0]["position"][1].asFloat() == 0);
    CHECK(state["particles"][0]["velocity"][1].asFloat() == 2 && state["particles"][0]["velocity"][2].asFloat() == 0);
    CHECK(Call(e, "particles.burst", R"J({"id":"Effect","count":10})J")["result"]["accepted"].asInt() == 1);
    CHECK(Call(e, "particles.burst", R"J({"id":"Effect","count":1})J")["result"]["accepted"].asInt() == 0);
    CHECK(!Call(e, "particles.burst", R"J({"id":"Effect","count":10001})J")["ok"].asBool());
    CHECK(!Call(e, "particles.burst", R"J({"id":"Effect","count":-1})J")["ok"].asBool());
    CHECK(!Call(e, "particles.burst", R"J({"id":"Effect","count":1.5})J")["ok"].asBool());
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"playing":false}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    state = Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    CHECK(std::fabs(state["particles"][0]["age"].asFloat() - 1.0f/60) < 1e-6f);
    CHECK(std::fabs(state["particles"][0]["velocity"][1].asFloat() - (2 - 2.0f/60)) < 1e-6f);
    CHECK(state["particles"][0]["position"][1].asFloat() > 0);
    Call(e, "sim.step", R"J({"frames":31})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 0);
    Call(e, "particles.clear", R"J({"id":"Effect","restart":true})J");
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"space":"world","playing":true}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    state = Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    CHECK(state["particles"][0]["worldSpace"].asBool() && state["particles"][0]["position"][0].asFloat() == 3);
    Call(e, "component.set", R"J({"id":"Effect","type":"Transform","values":{"position":[8,4,0]}})J");
    CHECK(e.GetScene().Get<ParticleEmitter>(id)->particles[0].position.x == 3);
    Json serialized = e.GetScene().ToJson();
    Scene restored;
    CHECK(restored.FromJson(serialized, &err));
    CHECK(restored.Get<ParticleEmitter>(id)->particles.empty() && restored.Get<ParticleEmitter>(id)->burst == 2);
    Call(e, "particles.clear", R"J({"id":"Effect"})J");
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"maxParticles":10,"loop":true,"rate":6,"lifetime":10}})J");
    Call(e, "sim.step", R"J({"frames":60})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 6);
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"maxParticles":1}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 1);
    CHECK(Call(e, "script.eval", R"J({"code":"assert(particles.burst('Effect', 5) == 0); assert(not pcall(particles.burst, 'Effect', -1))"})J")["ok"].asBool());
    Call(e, "particles.clear", R"J({"id":"Effect"})J");
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(), "scripts/burst.lua"), "return {onStart=function(self) assert(self:burst(1) == 1) end}"));
    Call(e, "component.add", R"J({"id":"Effect","type":"Script","values":{"path":"scripts/burst.lua"}})J");
    Call(e, "sim.step", R"J({"frames":1})J");
    CHECK(Call(e, "particles.state", R"J({"id":"Effect"})J")["result"]["count"].asInt() == 1 && e.Scripts().Errors().empty());
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"gravity":[0,1e100,0]}})J");
    CHECK(Call(e, "particles.burst", R"J({"id":"Effect","count":1})J")["error"]["code"].asString() == "invalid_emitter");
}

TEST(ParticleRandomStreamIsDeterministic) {
    auto simulate = [](int seed) {
        Engine e;
        Call(e, "scene.new", R"J({"empty":true})J");
        Json args = Json::parse(R"J({"name":"Effect","components":{"ParticleEmitter":{"rate":0,"burst":10,"loop":false,"spread":90,"gravity":[0,0,0],"lifetime":10}}})J");
        args["components"]["ParticleEmitter"]["seed"] = seed;
        e.Call("entity.create", args);
        Call(e, "sim.step", R"J({"frames":15})J");
        return Call(e, "particles.state", R"J({"id":"Effect","particles":true})J")["result"];
    };
    Json first = simulate(7);
    CHECK(first == simulate(7) && first != simulate(8));
    CHECK(first["count"].asInt() == 10);
    for (const Json& particle : first["particles"].items()) {
        const Json& v = particle["velocity"];
        float length = std::sqrt(v[0].asFloat()*v[0].asFloat() + v[1].asFloat()*v[1].asFloat() + v[2].asFloat()*v[2].asFloat());
        CHECK(std::fabs(length - 1) < 1e-5f && v[1].asFloat() >= -1e-6f);
    }
}

TEST(ShadingShadowsAndLights) {
    Engine e;
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Sun", "components": {"Transform": {"rotation": [-70, 20, 0]}, "DirectionalLight": {"shadowStrength": 1}}})J");
    Call(e, "entity.create", R"J({"name": "Ground", "components": {"Transform": {"scale": [10, 1, 10]}, "MeshRenderer": {"mesh": "plane"}}})J");
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"Transform": {"position": [0, 1, 0], "scale": [1.5, 1.5, 1.5]}, "MeshRenderer": {"mesh": "sphere"}}})J");
    Vec3 eye(0, 6, 7), at(0, 0, 0);
    RenderTarget withShadow = RenderLook(e, eye, at);
    Call(e, "component.set", R"J({"id": "Sun", "type": "DirectionalLight", "values": {"shadows": false}})J");
    RenderTarget noShadow = RenderLook(e, eye, at);
    CHECK(MeanLuma(withShadow) < MeanLuma(noShadow) - 0.5);
    Call(e, "component.set", R"J({"id": "Ball", "type": "MeshRenderer", "values": {"shading": "flat"}})J");
    RenderTarget flat = RenderLook(e, eye, at);
    CHECK(flat.Hash() != noShadow.Hash());
    Call(e, "entity.create", R"J({"name": "Lamp", "components": {"Transform": {"position": [2, 0.5, 2]}, "PointLight": {"intensity": 3, "range": 4}}})J");
    RenderTarget lamp = RenderLook(e, eye, at);
    CHECK(MeanLuma(lamp) > MeanLuma(flat) + 1.0);
}

TEST(OrthographicAndFollowCamera) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("follow_camera"), &err));  // the runner below moves through a project script
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 0, 5]}, "Camera": {"projection": "orthographic", "orthoSize": 3}}})J");
    Call(e, "entity.create", R"J({"name": "Box", "components": {"MeshRenderer": {}}})J");
    EntityId box = e.GetScene().FindByName("Box");
    auto coverage = [&]() {
        RenderTarget rt;
        rt.Resize(160, 90);
        e.RenderGameView(rt);
        return CountId(rt, box);
    };
    size_t near1 = coverage();
    Call(e, "component.set", R"J({"id": "Box", "type": "Transform", "values": {"position": [0, 0, -10]}})J");
    size_t far1 = coverage();
    CHECK(near1 > 200 && near1 == far1);  // orthographic: size does not depend on distance
    Call(e, "component.set", R"J({"id": "Cam", "type": "Camera", "values": {"projection": "perspective"}})J");
    CHECK(coverage() < far1 / 4);

    // CameraFollow keeps the camera at target + offset and aims at the target.
    Call(e, "script.write", R"J({"path": "scripts/runner.lua", "source": "local M = {}\nfunction M:onUpdate(dt) self:translate(3 * dt, 0, 0) end\nreturn M\n"})J");
    Call(e, "entity.create", R"J({"name": "Runner", "components": {"Transform": {"position": [0, 0.5, 0]}, "Script": {"path": "scripts/runner.lua"}}})J");
    EntityId runner = e.GetScene().FindByName("Runner");
    Json args = Json::parse(R"J({"id": "Cam", "type": "CameraFollow", "values": {"offset": [0, 3, 6], "smoothing": 0}})J");
    args["values"]["target"] = runner;
    e.Call("component.add", args);
    Call(e, "sim.step", R"J({"frames": 60})J");
    Vec3 cam = PosOf(e, "Cam"), run = PosOf(e, "Runner");
    CHECK(Near(cam, run + Vec3(0, 3, 6), 1e-3f));
    Vec3 fwd = ForwardFromEuler(e.GetScene().Get<Transform>(e.GetScene().FindByName("Cam"))->rotation);
    CHECK(Dot(fwd, Normalize(run + Vec3(0, 0.5f, 0) - cam)) > 0.999f);
}

TEST(DebugDrawing) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("debugdraw"), &err));
    e.Call("scene.new", Json());
    auto hash = [&]() { return Call(e, "render.screenshot", R"J({"width": 96, "height": 54, "inline": false})J")["result"]["hash"].asString(); };
    Call(e, "sim.step", R"J({"frames": 1})J");
    std::string clean = hash();
    CHECK(Call(e, "debug.draw", R"J({"boxes": [{"center": [0, 1, 0], "size": [2, 2, 2], "color": [1, 0, 0]}], "seconds": 0.5})J")["result"]["added"].asInt() == 12);
    CHECK(hash() != clean);
    Call(e, "sim.step", R"J({"frames": 40})J");  // expired
    CHECK(hash() == clean);
    // Lua: 0-second lines last one simulated frame.
    Call(e, "script.eval", R"J({"code": "draw.sphere({x = 0, y = 1, z = 0}, 1, {1, 1, 0})"})J");
    CHECK(e.DebugLineCount() == 72);
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(e.DebugLineCount() == 0);
}

TEST(ShowcaseFoxModel) {
    // Completion check for rendering: the external glTF character (samples/Showcase) is drawn with its texture.
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
    Json info = Call(e, "asset.info", R"J({"path": "assets/models/fox.glb"})J")["result"];
    CHECK(info["triangles"].asInt() == 576 && info["textures"][0][0].asInt() == 1024);
    CHECK(info["joints"].asInt() == 24 && info["clips"].size() == 3);
    CHECK(info["clips"][0]["name"].asString() == "Survey" && info["clips"][1]["name"].asString() == "Walk" &&
          info["clips"][2]["name"].asString() == "Run");
    CHECK(info["clips"][1]["duration"].asFloat() > 0.7f && info["clips"][1]["channels"].asInt() == 21);
    RenderTarget rt;
    rt.Resize(320, 180);
    e.RenderGameView(rt);
    EntityId fox = e.GetScene().FindByName("Fox");
    CHECK(CountId(rt, fox) > 100);
    // Textured: the fox's pixels are not all one color.
    std::set<uint32_t> colors;
    for (size_t i = 0; i < rt.ids.size(); ++i) {
        if (rt.ids[i] == fox) colors.insert(rt.color[i]);
    }
    CHECK(colors.size() > 20);
    CHECK(e.Scripts().Errors().empty());
    // Multi-threaded rendering is bit-identical to a single thread.
    SetMaxRenderThreads(1);
    RenderTarget single;
    single.Resize(320, 180);
    e.RenderGameView(single);
    SetMaxRenderThreads(16);
    CHECK(single.Hash() == rt.Hash());
}

TEST(ShowcaseAnimationControls) {
    auto play = []() {
        Engine e;
        std::string err;
        CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
        EntityId fox = e.GetScene().FindByName("Fox");
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Survey");
        Call(e, "input.key", R"J({"key":"W","down":true})J");
        Call(e, "sim.step", R"J({"frames":12})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Walk");
        float walkTime = e.GetScene().Get<Animator>(fox)->time;
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(e.GetScene().Get<Animator>(fox)->time > walkTime);  // no restart each update
        Call(e, "input.key", R"J({"key":"Shift","down":true})J");
        Call(e, "sim.step", R"J({"frames":12})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Run");
        RenderTarget rt;
        rt.Resize(320,180);
        e.RenderGameView(rt);
        uint64_t runHash = rt.Hash();
        Call(e, "input.key", R"J({"key":"W","down":false})J");
        Call(e, "input.key", R"J({"key":"Shift","down":false})J");
        Call(e, "sim.step", R"J({"frames":1})J");
        CHECK(e.GetScene().Get<Animator>(fox)->clip == "Survey");
        CHECK(e.Scripts().Errors().empty());
        return runHash;
    };
    uint64_t first = play();
    CHECK(first == play());
    std::printf("  animated Showcase hash %016llx\n", static_cast<unsigned long long>(first));
}

TEST(ShowcasePostProcessControls) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
    const EntityId camera = e.GetScene().FindByName("Main Camera");
    CHECK(!e.GetScene().Get<PostProcess>(camera)->fxaa);
    Call(e, "sim.step", R"J({"frames":1})J");
    for (int mode : {2, 3, 4, 5, 6, 1}) {
        const std::string key = std::to_string(mode);
        CHECK(e.Call("input.key", Json::parse("{\"key\":\"" + key + "\",\"down\":true}"))["ok"].asBool());
        Call(e, "sim.step", R"J({"frames":1})J");
        const PostProcess& p = *e.GetScene().Get<PostProcess>(camera);
        const bool hdr = mode == 2 || mode == 3 || mode == 6;
        CHECK(p.exposure == (hdr ? 0.75f : 1) && p.toneMapping == (hdr ? "reinhard" : "none"));
        CHECK((p.bloom > 0) == (mode == 3 || mode == 6));
        CHECK((p.vignette > 0) == (mode == 4 || mode == 6));
        CHECK(p.fxaa == (mode == 5 || mode == 6));
        CHECK(e.Call("input.key", Json::parse("{\"key\":\"" + key + "\",\"down\":false}"))["ok"].asBool());
        Call(e, "sim.step", R"J({"frames":1})J");
    }
    CHECK(e.GetScene().Get<UIText>(e.GetScene().FindByName("Hint"))->text.find("All: Off") != std::string::npos);
    CHECK(e.Scripts().Errors().empty());
}

TEST(ShaderGraphSceneUniforms) {
    // cameraPosition and lightDirection are reserved uniform names filled by the renderer,
    // so a view-dependent graph follows a moving camera without material edits.
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("shader_scene_uniforms"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "shader.create", R"J({"path":"materials/side.shader.json","graph":{
      "uniforms":{"cameraPosition":[9,9,9,9],"lightDirection":[9,9,9,9]},
      "nodes":[{"op":"uniform","name":"cameraPosition"},{"op":"uniform","name":"lightDirection"},
      {"op":"constant","value":0},{"op":"step","args":[2,0]},{"op":"swizzle","args":[3],"value":[0,0,0,0]},
      {"op":"constant","value":[1,0,0,1]},{"op":"constant","value":[0,0,1,1]},
      {"op":"mix","args":[5,6,4]}],"color":7}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"side.mat.json","values":{"unlit":true,
      "shader":"materials/side.shader.json","shaderUniforms":{"cameraPosition":[-50,0,0,1]}}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[3,0,10]},"Camera":{"clearColor":[0,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Sun","components":{"Transform":{"rotation":[-40,25,0]},"DirectionalLight":{}}})J");
    Call(e, "entity.create", R"J({"name":"Cube","components":{"Transform":{"scale":[4,4,1]},"MeshRenderer":{"material":"side.mat.json"}}})J");
    auto count = [&](uint32_t rgb) {
        RenderTarget target; target.Resize(128, 72);
        e.RenderGameView(target);
        int n = 0;
        for (uint32_t color : target.color) n += (color & 0xFFFFFF) == rgb;
        return n;
    };
    CHECK(count(0xFF0000) > 100 && count(0x0000FF) == 0);  // camera at +X: blue, despite the override
    Call(e, "component.set", R"J({"id":"Camera","type":"Transform","values":{"position":[-3,0,10]}})J");
    CHECK(count(0x0000FF) > 100 && count(0xFF0000) == 0);  // camera at -X: red
    RenderView view = MakeLookAtView(Vec3(1,2,3), Vec3(0,0,0), 45, 1);
    auto items = GatherRenderItems(e.GetScene(), &e.Assets(), view.view);
    RenderLights lights = GatherRenderLights(e.GetScene());
    auto draws = BuildDrawList(items, view.eye, &lights);
    CHECK(draws.size() == 1 && draws[0].material.shader);
    if (draws.size() != 1 || !draws[0].material.shader) return;
    const Vec4 eye = draws[0].material.shaderUniforms[0], light = draws[0].material.shaderUniforms[1];
    CHECK(eye.x == 1 && eye.y == 2 && eye.z == 3 && eye.w == 1);
    const Vec3 toward = -Normalize(lights.dirs[0].dir);
    CHECK(Length(light.xyz() - toward) < 1e-6f && light.w == 0 && toward.y > 0.5f);
    draws = BuildDrawList(items, view.eye);  // no lights supplied: the declared value stays
    CHECK(draws[0].material.shaderUniforms[1].x == 9);
}

TEST(WuwaToonGraphBandsRimAndSpecular) {
    Engine e; std::string error;
    CHECK(e.Open(TestSourceDir()+"/samples/WuwaToon",&error));
    CHECK(Call(e,"shader.check",R"({"path":"materials/toon.shader.json"})")["ok"].asBool());
    CHECK(Call(e,"shader.check",R"({"path":"materials/normals.shader.json"})")["ok"].asBool());
    ShaderGraph graph;
    CHECK(CompileShaderGraph(e.ReadProjectJson("materials/toon.shader.json"),graph,&error));
    CHECK(graph.instructions.size()<=32 && graph.uniformNames.size()<=8);
    std::array<Vec4,ShaderGraph::kMaxUniforms> uniforms;
    Json overrides=Json::parse(R"({"lightDir":[0,0,1,0],"shadowCut":0.12,"shadowTint":[0.5,0.25,0.75,1],
      "cameraPos":[0,0,10,1],"rimStart":0.78,"rimColor":[0,0,0,0],"halfDir":[0,0,1,0],"specColor":[0,0,0,0]})");
    ShaderInputs input; input.position=Vec4(0,0,0,1); input.baseColor=Vec4(0.8f,0.6f,0.4f,1);
    auto expect=[&](float z,const Vec3& rgb) {
        CHECK(ShaderUniforms(graph,overrides,uniforms,&error));
        input.normal=Vec4(std::sqrt(std::max(0.0f,1-z*z)),0,z,0);
        const ShaderSurface surface=EvaluateShaderGraph(graph,input,uniforms);
        CHECK(std::fabs(surface.color.x-rgb.x)<1e-5f && std::fabs(surface.color.y-rgb.y)<1e-5f &&
              std::fabs(surface.color.z-rgb.z)<1e-5f && surface.color.w==1);
    };
    expect(0.11f,Vec3(0.4f,0.15f,0.3f)); // below threshold: tinted shadow
    expect(0.13f,Vec3(0.8f,0.6f,0.4f)); // above threshold: flat diffuse band
    expect(0.7f,Vec3(0.8f,0.6f,0.4f)); // diffuse remains flat within its band
    overrides["rimColor"]=Json(Json::Array{0.1,0.2,0.3,0});
    expect(0.21f,Vec3(0.9f,0.8f,0.7f)); // 1-N.V > .78: colored rim
    expect(0.23f,Vec3(0.8f,0.6f,0.4f)); // 1-N.V < .78: no rim
    overrides["specColor"]=Json(Json::Array{0.2,0.1,0.05,0});
    expect(0.96f,Vec3(0.8f,0.6f,0.4f));
    expect(0.97f,Vec3(1.0f,0.7f,0.45f)); // quantized specular threshold .965
}

TEST(WuwaToonSampleControlsAndPackaging) {
    Engine e; std::string error;
    const std::string project=TestSourceDir()+"/samples/WuwaToon";
    CHECK(e.Open(project,&error));
    CHECK(Call(e,"script.check",R"({"path":"scripts/lab.lua"})")["ok"].asBool());
    CHECK(Call(e,"sim.step",R"({"frames":1})")["ok"].asBool());
    RenderTarget toon, pbr, normals, reset;
    for (RenderTarget* target : {&toon,&pbr,&normals,&reset}) target->Resize(240,135);
    e.RenderGameView(toon);
    RenderTarget surfaceToon, surfacePbr, surfaceNormals;
    for (RenderTarget* target : {&surfaceToon,&surfacePbr,&surfaceNormals}) target->Resize(240,135);
    auto renderSurface=[&](RenderTarget& target) {
        RenderView view; MakeSceneView(e.GetScene(),240.0f/135,view); view.drawUI=false;
        e.Renderer().Render(e.GetScene(),view,target);
    };
    renderSurface(surfaceToon);
    const std::vector<std::string> parts={"skin","hair","coat","boots","trim","eyes","iris","ink","blush"};
    auto checkMode=[&](const char* mode) {
        for (const std::string& part : parts) {
            const MeshRenderer* renderer=e.GetScene().Get<MeshRenderer>(e.GetScene().FindByName("Part_"+part));
            CHECK(renderer && renderer->material==(std::string(mode)=="normals"?"materials/normals.mat.json":
                                                   "materials/"+part+"-"+mode+".mat.json"));
        }
    };
    checkMode("toon");
    int visible=0;
    for (EntityId id : toon.ids) if (id && e.GetScene().Record(id)->name.find("Part_")==0) ++visible;
    CHECK(visible>100);
    auto key=[&](const char* name) {
        Json args=Json::MakeObject(); args["key"]=name; args["down"]=true;
        CHECK(e.Call("input.key",args)["ok"].asBool());
        CHECK(Call(e,"sim.step",R"({"frames":1})")["ok"].asBool());
        args["down"]=false; CHECK(e.Call("input.key",args)["ok"].asBool());
        CHECK(Call(e,"sim.step",R"({"frames":1})")["ok"].asBool());
    };
    key("2"); checkMode("pbr"); e.RenderGameView(pbr); CHECK(pbr.Hash()!=toon.Hash()); renderSurface(surfacePbr);
    key("3"); checkMode("normals"); e.RenderGameView(normals); CHECK(normals.Hash()!=pbr.Hash() && normals.Hash()!=toon.Hash());
    renderSurface(surfaceNormals);
    int changedPbr=0, changedNormals=0;
    for (size_t i=0;i<surfaceToon.color.size();++i) {
        const EntityId id=surfaceToon.ids[i];
        if (!id || e.GetScene().Record(id)->name.find("Part_")!=0) continue;
        changedPbr+=surfaceToon.color[i]!=surfacePbr.color[i];
        changedNormals+=surfaceToon.color[i]!=surfaceNormals.color[i];
    }
    CHECK(changedPbr>100 && changedNormals>100); // the surfaces themselves change, independently of mode-label text
    key("1"); checkMode("toon");
    key("O"); CHECK(!e.GetScene().Get<MeshRenderer>(e.GetScene().FindByName("Outline_skin"))->visible);
    key("P"); CHECK(Call(e,"sim.step",R"({"frames":30})")["ok"].asBool());
    CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Character"))->rotation.y>10);
    key("R"); checkMode("toon"); e.RenderGameView(reset);
    CHECK(reset.Hash()==toon.Hash());
    CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Character"))->rotation.y==0);
    const Json layout=Call(e,"ui.layout",R"({"width":1280,"height":720,"interactable":true})")["result"]["elements"];
    auto button=[&](const char* name,bool touch) {
        Json args=Json::MakeObject(); bool found=false;
        for (const Json& item : layout.items()) if (item["name"].asString()==name) {
            args["x"]=item["center"][0]; args["y"]=item["center"][1]; found=true;
        }
        CHECK(found); if (!found) return;
        args["width"]=1280; args["height"]=720;
        if (touch) { args["id"]=7; args["down"]=true; }
        const Json dispatch=e.Call(touch?"input.touch":"input.click",args);
        CHECK(dispatch["ok"].asBool());
        if (!touch) CHECK(dispatch["result"]["buttonName"].asString()==name);
        CHECK(Call(e,"sim.step",R"({"frames":1})")["ok"].asBool());
        if (touch) {
            const UIButton* control=e.GetScene().Get<UIButton>(e.GetScene().FindByName(name));
            CHECK(control && !control->key.empty() && control->pressed);
            args["down"]=false; CHECK(e.Call("input.touch",args)["ok"].asBool());
            CHECK(Call(e,"sim.step",R"({"frames":1})")["ok"].asBool());
        }
    };
    button("Button_pbr",false); checkMode("pbr");
    button("Button_normals",true); checkMode("normals");
    button("Button_reset",true); checkMode("toon");
    button("Button_outline",false);
    CHECK(!e.GetScene().Get<MeshRenderer>(e.GetScene().FindByName("Outline_skin"))->visible);
    button("Button_outline",true);
    CHECK(e.GetScene().Get<MeshRenderer>(e.GetScene().FindByName("Outline_skin"))->visible);
    button("Button_turntable",true);
    CHECK(Call(e,"sim.step",R"({"frames":10})")["ok"].asBool());
    CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Character"))->rotation.y>0);
    button("Button_reset",false);
    CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Character"))->rotation.y==0);
    // Real mouse presses deliver both the mapped key and onClick; primary-touch compatibility adds a third input source.
    auto heldMouse=[&](const char* name,bool primaryTouch,const std::function<void()>& verify) {
        Json pointer=Json::MakeObject(); bool found=false;
        for (const Json& item : layout.items()) if (item["name"].asString()==name) {
            pointer["x"]=item["center"][0]; pointer["y"]=item["center"][1]; found=true;
        }
        CHECK(found); if (!found) return;
        pointer["width"]=1280; pointer["height"]=720;
        CHECK(e.Call("input.mouse",pointer)["ok"].asBool());
        CHECK(Call(e,"input.key",R"({"key":"MouseLeft","down":true})")["ok"].asBool());
        if (primaryTouch) {
            pointer["id"]=9; pointer["down"]=true;
            CHECK(e.Call("input.touch",pointer)["ok"].asBool());
        }
        CHECK(Call(e,"sim.step",R"({"frames":1})")["ok"].asBool()); verify();
        CHECK(Call(e,"sim.step",R"({"frames":3})")["ok"].asBool()); verify();
        CHECK(Call(e,"input.key",R"({"key":"MouseLeft","down":false})")["ok"].asBool());
        if (primaryTouch) CHECK(Call(e,"input.touch",R"({"id":9,"down":false})")["ok"].asBool());
        CHECK(Call(e,"sim.step",R"({"frames":2})")["ok"].asBool()); verify();
    };
    heldMouse("Button_outline",false,[&]() {
        CHECK(!e.GetScene().Get<MeshRenderer>(e.GetScene().FindByName("Outline_skin"))->visible);
        CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("Button_outline"))->text=="Outline: OFF");
    });
    heldMouse("Button_outline",true,[&]() {
        CHECK(e.GetScene().Get<MeshRenderer>(e.GetScene().FindByName("Outline_skin"))->visible);
        CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("Button_outline"))->text=="Outline: ON");
    });
    heldMouse("Button_turntable",false,[&]() {
        CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("Button_turntable"))->text=="Spin: ON");
        CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Character"))->rotation.y>0);
    });
    const float stoppedAngle=e.GetScene().Get<Transform>(e.GetScene().FindByName("Character"))->rotation.y;
    heldMouse("Button_turntable",true,[&]() {
        CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("Button_turntable"))->text=="Spin: OFF");
        CHECK(e.GetScene().Get<Transform>(e.GetScene().FindByName("Character"))->rotation.y==stoppedAngle);
    });
    key("R"); checkMode("toon");
    CHECK(e.Scripts().Errors().empty());
    const std::vector<std::string> files=GameFiles(project);
    for (const char* path : {"materials/toon.shader.json","materials/normals.shader.json","materials/skin-toon.mat.json",
                             "assets/models/skin.glb","assets/models/hair.glb","scripts/lab.lua"})
        CHECK(std::find(files.begin(),files.end(),path)!=files.end());
    CHECK(WriteGamePak(project,files,"build/test_wuwa_toon/game.pak",&error));
    std::vector<unsigned char> bytes; CHECK(ReadBinaryFile("build/test_wuwa_toon/game.pak",bytes));
    CHECK(ExtractGamePak(bytes,"build/test_wuwa_toon/game",&error));
    Engine packaged; CHECK(packaged.Open("build/test_wuwa_toon/game",&error));
    CHECK(Call(packaged,"sim.step",R"({"frames":1})")["ok"].asBool());
    packaged.RenderGameView(reset);
    CHECK(reset.Hash()==toon.Hash() && packaged.Scripts().Errors().empty());
    RemoveAll("build/test_wuwa_toon");
}

TEST(ShaderLabSampleControlsAndPackaging) {
    Engine e;
    std::string error;
    const std::string project = TestSourceDir() + "/samples/ShaderLab";
    CHECK(e.Open(project, &error));
    for (const char* path : {"materials/pulse.shader.json", "materials/mask.shader.json"}) {
        Json args = Json::MakeObject();
        args["path"] = path;
        CHECK(e.Call("shader.check", args)["ok"].asBool());
    }
    CHECK(Call(e, "script.check", R"J({"path":"scripts/effects.lua"})J")["ok"].asBool());
    RenderTarget first, animated, replay;
    for (RenderTarget* target : {&first, &animated, &replay}) target->Resize(160,90);
    CHECK(Call(e, "sim.step", R"J({"frames":1})J")["ok"].asBool());
    e.RenderGameView(first);
    CHECK(Call(e, "sim.step", R"J({"frames":60})J")["ok"].asBool());
    e.RenderGameView(animated);
    CHECK(animated.Hash() != first.Hash());
    CHECK(animated.ids == first.ids && animated.depth == first.depth);
    const EntityId camera = e.GetScene().FindByName("Camera");
    for (const char* key : {"2", "1"}) {
        Json args = Json::MakeObject();
        args["key"] = key;
        args["down"] = true;
        CHECK(e.Call("input.key", args)["ok"].asBool());
        CHECK(Call(e, "sim.step", R"J({"frames":1})J")["ok"].asBool());
        const PostProcess* effect = e.GetScene().Get<PostProcess>(camera);
        const bool hdr = std::string(key) == "2";
        CHECK(effect && effect->exposure == (hdr ? 0.75f : 1));
        CHECK(effect && effect->toneMapping == (hdr ? "reinhard" : "none"));
        CHECK(effect && (effect->bloom > 0) == hdr);
        args["down"] = false;
        CHECK(e.Call("input.key", args)["ok"].asBool());
        CHECK(Call(e, "sim.step", R"J({"frames":1})J")["ok"].asBool());
    }
    CHECK(e.Scripts().Errors().empty());
    const std::vector<std::string> files = GameFiles(project);
    for (const char* path : {"materials/pulse.shader.json", "materials/mask.shader.json",
                             "materials/pulse.mat.json", "materials/mask.mat.json", "scripts/effects.lua"})
        CHECK(std::find(files.begin(), files.end(), path) != files.end());
    CHECK(WriteGamePak(project, files, "build/test_shader_lab/game.pak", &error));
    std::vector<unsigned char> bytes;
    CHECK(ReadBinaryFile("build/test_shader_lab/game.pak", bytes));
    CHECK(ExtractGamePak(bytes, "build/test_shader_lab/game", &error));
    Engine packaged;
    CHECK(packaged.Open("build/test_shader_lab/game", &error));
    CHECK(Call(packaged, "sim.step", R"J({"frames":61})J")["ok"].asBool());
    packaged.RenderGameView(replay);
    CHECK(replay.Hash() == animated.Hash());
    CHECK(packaged.Scripts().Errors().empty());
    RemoveAll("build/test_shader_lab");
}

TEST(UIQuadsAreWhatSoftwareDraws) {
    // Both renderers draw UI from BuildUIQuads; the software result is the reference.
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
    std::vector<UIQuad> quads = BuildUIQuads(e.GetScene(), 640, 360, &e.Assets());
    CHECK(quads.size() > 50);  // one textured quad per glyph
    RenderTarget rt;
    rt.Resize(640, 360);
    e.RenderGameView(rt);
    std::set<EntityId> drawn;
    for (const UIQuad& q : quads) {
        CHECK(q.x1 > q.x0 && q.y1 > q.y0);
        CHECK(q.texture != nullptr);  // Showcase UI is all text in the default font
        for (int y = std::max(0, q.y0); y < std::min(360, q.y1); ++y) {
            for (int x = std::max(0, q.x0); x < std::min(640, q.x1); ++x) {
                Color c;
                float a;
                ShadeUIQuad(q, x + 0.5f, y + 0.5f, &c, &a);
                if (a >= 0.5f * q.alpha && rt.IdAt(x, y) == q.entity) drawn.insert(q.entity);
            }
        }
    }
    for (const auto& kv : e.GetScene().Pool<UIText>()) CHECK(!kv.second.visible || drawn.count(kv.first));
}

TEST(UITextFontsAndRichText) {
    // UTF-8 decoding and the embedded default font.
    std::vector<uint32_t> cps = DecodeUtf8("A\xea\xb0\x80\xff");
    CHECK(cps.size() == 3 && cps[0] == 'A' && cps[1] == 0xAC00 && cps[2] == 0xFFFD);
    std::shared_ptr<FontFace> roboto = FontFace::Default();
    CHECK(roboto && roboto->familyName == "Roboto" && roboto->HasGlyph('g') && !roboto->HasGlyph(0xAC00));
    FontFace::Glyph g = roboto->GetGlyph('H', 32, false);
    CHECK(g.page == 0 && g.w > 10 && g.h > 18 && g.y0 < 0 && g.advance > 15.0f);
    CHECK(roboto->GetGlyph('H', 32, true).w > g.w);  // synthetic bold is wider
    float w1, h1, w2, h2, wp, hp;
    MeasureText("Hello", 32, &w1, &h1, "default");
    MeasureText("Hello\nWorld", 32, &w2, &h2, "default");
    MeasureText("Hello", 32, &wp, &hp, "pixel");
    CHECK(w1 > 60 && w1 < 90 && std::fabs(w2 - w1) < 20 && h2 > h1 * 1.8f);
    CHECK(wp == (5 * 6 - 1) * 4.0f && hp == 7 * 4.0f);  // pixel font: 6x8 cells of 4 px

    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_text"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // A font file in the project (the embedded Roboto written out).
    CreateDirectories(JoinPath(e.ProjectDir(), "assets/fonts"));
    CHECK(CopyFileTo(TestSourceDir() + "/third_party/fonts/Roboto-Regular.ttf", JoinPath(e.ProjectDir(), "assets/fonts/body.ttf")));
    Json info = Call(e, "asset.info", R"J({"path": "assets/fonts/body.ttf"})J")["result"];
    CHECK(info["kind"].asString() == "font" && info["family"].asString() == "Roboto");
    Call(e, "entity.create", R"J({"name": "Red", "components": {"UIText": {"text": "<color=#ff0000>WW</color>WW", "font": "assets/fonts/body.ttf", "x": 0, "y": 0, "size": 72}}})J");
    Call(e, "entity.create", R"J({"name": "Raw", "components": {"UIText": {"text": "<b>", "richText": false, "x": 0, "y": 200, "size": 40}}})J");
    Call(e, "entity.create", R"J({"name": "Wrapped", "components": {"UIText": {"text": "one two three four five six", "x": 0, "y": 400, "width": 120, "size": 24}}})J");
    Call(e, "entity.create", R"J({"name": "Typed", "components": {"UIText": {"text": "abc", "x": 600, "y": 0, "visibleCharacters": 0}}})J");
    RenderTarget rt;
    rt.Resize(1280, 720);
    RenderView view;
    MakeSceneView(e.GetScene(), 1280.0f / 720.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    int red = 0, white = 0;
    EntityId redId = e.GetScene().FindByName("Red");
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 400; ++x) {
            uint32_t c = rt.color[static_cast<size_t>(y) * 1280 + static_cast<size_t>(x)];
            if (rt.IdAt(x, y) != redId) continue;
            if ((c & 0xFF) > 200 && ((c >> 8) & 0xFF) < 60) ++red;
            if ((c & 0xFF) > 200 && ((c >> 8) & 0xFF) > 200) ++white;
        }
    }
    CHECK(red > 200 && white > 200);
    Json layout = Call(e, "ui.layout", R"J({"width": 1280, "height": 720})J")["result"]["elements"];
    CHECK(layout.size() == 4);
    CHECK(layout[1]["rect"][2].asNumber() > 40);   // "<b>" drawn literally: three glyphs
    CHECK(layout[2]["rect"][2].asNumber() == 120);  // the box width
    CHECK(layout[2]["rect"][3].asNumber() > 70);    // wrapped onto 3 lines
    for (const UIQuad& q : BuildUIQuads(e.GetScene(), 1280, 720, &e.Assets())) CHECK(q.entity != e.GetScene().FindByName("Typed"));
}

TEST(UILayoutAnchorsAndClipping) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_layout"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // A fitted vertical menu: padding 10, spacing 5, children 100x40 and 120x30.
    Call(e, "entity.create", R"J({"name": "Menu", "components": {"UIPanel": {"anchor": "top-left", "x": 100, "y": 50, "width": 0, "height": 0}, "UILayout": {"direction": "vertical", "padding": 10, "spacing": 5, "fit": true}}})J");
    Call(e, "entity.create", R"J({"name": "A", "parent": "Menu", "components": {"UIButton": {"width": 100, "height": 40}}})J");
    Call(e, "entity.create", R"J({"name": "B", "parent": "Menu", "components": {"UIButton": {"width": 120, "height": 30, "order": -5}}})J");
    // Stretch inside a clipping panel; a child outside the clip cannot be clicked.
    Call(e, "entity.create", R"J({"name": "Frame", "components": {"UIPanel": {"anchor": "bottom-right", "x": -20, "y": -20, "width": 200, "height": 100, "clip": true}}})J");
    Call(e, "entity.create", R"J({"name": "Fill", "parent": "Frame", "components": {"UIImage": {"anchor": "stretch", "width": -20, "height": -20}}})J");
    Call(e, "entity.create", R"J({"name": "Hidden", "parent": "Frame", "components": {"UIButton": {"anchor": "top-left", "x": 150, "y": 0, "width": 100, "height": 50}}})J");
    Json els = Call(e, "ui.layout", R"J({"width": 1280, "height": 720})J")["result"]["elements"];
    std::map<std::string, Json> by;
    for (const Json& j : els.items()) by[j["name"].asString()] = j["rect"];
    auto rect = [&](const char* n, double x, double y, double w, double h) {
        const Json& r = by[n];
        return std::fabs(r[0].asNumber() - x) < 0.01 && std::fabs(r[1].asNumber() - y) < 0.01 && std::fabs(r[2].asNumber() - w) < 0.01 && std::fabs(r[3].asNumber() - h) < 0.01;
    };
    CHECK(rect("Menu", 100, 50, 140, 95));
    CHECK(rect("A", 110, 60, 100, 40));  // hierarchy order: A first although B has a lower draw order
    CHECK(rect("B", 110, 105, 120, 30));
    CHECK(rect("Frame", 1060, 600, 200, 100));
    CHECK(rect("Fill", 1070, 610, 180, 80));
    CHECK(Call(e, "input.click", R"J({"x": 1270, "y": 620, "width": 1280, "height": 720})J")["result"]["button"].asInt() == 0);  // clipped part
    CHECK(Call(e, "input.click", R"J({"x": 1220, "y": 620, "width": 1280, "height": 720})J")["result"]["buttonName"].asString() == "Hidden");
    // Horizontal row, centered, and a grid.
    Call(e, "component.set", R"J({"id": "Menu", "type": "UILayout", "values": {"direction": "horizontal", "crossAlign": "center"}})J");
    els = Call(e, "ui.layout", R"J({"width": 1280, "height": 720})J")["result"]["elements"];
    for (const Json& j : els.items()) by[j["name"].asString()] = j["rect"];
    CHECK(rect("Menu", 100, 50, 245, 60));
    CHECK(rect("B", 215, 65, 120, 30));
    // UICanvas: author for 1920x1080, match width.
    Call(e, "entity.create", R"J({"name": "Canvas", "components": {"UICanvas": {"referenceWidth": 1920, "referenceHeight": 1080, "match": 0}}})J");
    CHECK(std::fabs(Call(e, "ui.layout", R"J({"width": 960, "height": 720})J")["result"]["scale"].asNumber() - 0.5) < 1e-6);
}

TEST(UIInteractionHoverSliderAndDisabled) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_input"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "script.write", R"J({"path": "scripts/ui_log.lua", "source": "local M = {}
log_ = log_ or {}
function M:onPointerEnter() table.insert(log_, 'enter') end
function M:onPointerExit() table.insert(log_, 'exit') end
function M:onClick() table.insert(log_, 'click') end
function M:onValueChanged(v) table.insert(log_, string.format('value %.2f', v)) end
return M
"})J");
    Call(e, "entity.create", R"J({"name": "Btn", "components": {"UIButton": {"anchor": "top-left", "x": 0, "y": 0, "width": 200, "height": 100}, "Script": {"path": "scripts/ui_log.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Off", "components": {"UIButton": {"anchor": "top-left", "x": 300, "y": 0, "width": 200, "height": 100, "interactable": false}, "Script": {"path": "scripts/ui_log.lua"}}})J");
    Call(e, "entity.create", R"J({"name": "Vol", "components": {"UISlider": {"anchor": "top-left", "x": 0, "y": 200, "width": 400, "height": 40, "value": 0, "step": 0.25}, "Script": {"path": "scripts/ui_log.lua"}}})J");
    auto logText = [&] {
        return Call(e, "script.eval", R"J({"code": "table.concat(log_ or {}, ',')"})J")["result"]["value"].asString();
    };
    Call(e, "input.mouse", R"J({"x": 100, "y": 50, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(e.GetScene().Get<UIButton>(e.GetScene().FindByName("Btn"))->hovered);
    Call(e, "input.mouse", R"J({"x": 400, "y": 50, "width": 1280, "height": 720, "button": "MouseLeft", "down": true})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.mouse", R"J({"button": "MouseLeft", "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(logText() == "enter,exit");  // the disabled button neither hovers nor clicks
    // Press on the slider at 60 % and drag past its end: snapped values, callback per change.
    Call(e, "input.mouse", R"J({"x": 240, "y": 220, "width": 1280, "height": 720, "button": "MouseLeft", "down": true})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    Call(e, "input.mouse", R"J({"x": 900, "y": 500, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(e.GetScene().Get<UISlider>(e.GetScene().FindByName("Vol"))->value == 1.0f);
    Call(e, "input.mouse", R"J({"button": "MouseLeft", "down": false})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(logText() == "enter,exit,enter,value 0.50,exit,value 1.00");
    Call(e, "input.click", R"J({"x": 100, "y": 50, "width": 1280, "height": 720})J");
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(logText() == "enter,exit,enter,value 0.50,exit,value 1.00,enter,click");
    CHECK(e.Scripts().Errors().empty());
    Call(e, "sim.stop", "{}");
    CHECK(e.GetScene().Get<UISlider>(e.GetScene().FindByName("Vol"))->value == 0.0f);
}

// Mean color of a pixel block in a rendered target.
Color MeanColor(const RenderTarget& rt, int x0, int y0, int x1, int y1) {
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            uint32_t c = rt.color[static_cast<size_t>(y) * static_cast<size_t>(rt.width) + static_cast<size_t>(x)];
            r += c & 0xFF;
            g += (c >> 8) & 0xFF;
            b += (c >> 16) & 0xFF;
            ++n;
        }
    }
    return Color(static_cast<float>(r / n / 255), static_cast<float>(g / n / 255), static_cast<float>(b / n / 255));
}

TEST(MaterialsFilesAndPbr) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("materials"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    // Files: create, validation errors with hints, set, listing, info.
    Json made = Call(e, "material.create", R"J({"path": "materials/gold.mat.json", "values": {"baseColor": [1, 0.8, 0.2], "metallic": 1, "roughness": 0.3}})J");
    CHECK(made["ok"].asBool() && made["result"]["values"]["roughness"].asNumber() > 0.29);
    CHECK(Call(e, "material.create", R"J({"path": "materials/gold.mat.json"})J")["error"]["code"].asString() == "already_exists");
    Json bad = Call(e, "material.create", R"J({"path": "materials/bad.mat.json", "values": {"shininess": 3}})J");
    CHECK(bad["error"]["code"].asString() == "invalid_material" && bad["error"]["hint"].asString().find("roughness") != std::string::npos);
    CHECK(Call(e, "material.create", R"J({"path": "materials/x.json"})J")["error"]["code"].asString() == "invalid_path");
    CHECK(Call(e, "material.create", R"J({"path": "materials/m.mat.json", "values": {"baseTexture": "missing.png"}})J")["error"]["code"].asString() == "invalid_material");
    Json glass = Call(e, "material.create", R"J({"path": "materials/glass.mat.json", "values": {"opacity": 0.4}})J");
    CHECK(glass["result"]["values"]["alphaMode"].asString() == "blend");
    CHECK(Call(e, "asset.list", R"J({"kind": "material"})J")["result"].size() == 2);
    CHECK(Call(e, "asset.info", R"J({"path": "materials/glass.mat.json"})J")["result"]["alphaMode"].asString() == "blend");

    // A sphere in front of the camera, lit head-on.
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 0, 3]}, "Camera": {"clearColor": [0, 0, 0]}}})J");
    Call(e, "entity.create", R"J({"name": "Sun", "components": {"Transform": {"rotation": [0, 0, 0]}, "DirectionalLight": {"ambient": [0.2, 0.2, 0.2]}}})J");
    Call(e, "entity.create", R"J({"name": "Ball", "components": {"MeshRenderer": {"mesh": "sphere", "color": [1, 1, 1], "material": "materials/gold.mat.json"}, "Transform": {"scale": [2, 2, 2]}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(), 1.0f, view);
    RenderTarget rt;
    rt.Resize(128, 128);
    e.Renderer().Render(e.GetScene(), view, rt);
    Color center = MeanColor(rt, 60, 60, 68, 68);  // the specular highlight of a smooth metal
    Color rim = MeanColor(rt, 63, 36, 65, 38);
    CHECK(center.r > 0.9f && center.g > 0.7f);  // bright highlight
    CHECK(rim.r < center.r && rim.r > rim.b + 0.1f);  // gold-tinted reflections away from it
    // Rougher: a dimmer, wider highlight. Materials reload when set.
    Call(e, "material.set", R"J({"path": "materials/gold.mat.json", "values": {"roughness": 0.9, "metallic": 0}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color rough = MeanColor(rt, 60, 60, 68, 68);
    CHECK(rough.r < center.r + 0.01f && rough.b < 0.35f);
    // Emissive glows without light; unlit ignores it.
    Call(e, "material.create", R"J({"path": "materials/neon.mat.json", "values": {"baseColor": [0, 0, 0], "emissive": [0, 1, 0]}})J");
    Call(e, "component.set", R"J({"id": "Ball", "type": "MeshRenderer", "values": {"material": "materials/neon.mat.json"}})J");
    Call(e, "component.set", R"J({"id": "Sun", "type": "DirectionalLight", "values": {"intensity": 0, "ambient": [0, 0, 0]}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color glow = MeanColor(rt, 60, 60, 68, 68);
    CHECK(glow.g > 0.95f && glow.r < 0.05f);
    // A broken material file renders magenta (like a missing mesh).
    Call(e, "component.set", R"J({"id": "Ball", "type": "MeshRenderer", "values": {"material": "materials/none.mat.json"}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color magenta = MeanColor(rt, 60, 60, 68, 68);
    CHECK(magenta.r > 0.95f && magenta.g < 0.05f && magenta.b > 0.95f);
}

TEST(TransparencyNormalMapsAndDoubleSided) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("transparency"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 0, 5]}, "Camera": {"clearColor": [0, 0, 0]}}})J");
    // Created front to back on purpose: the draw list sorts transparent parts back to front.
    Call(e, "entity.create", R"J({"name": "Front", "components": {"MeshRenderer": {"mesh": "quad", "color": [0, 0, 1], "unlit": true, "opacity": 0.5}, "Transform": {"position": [0, 0, 1], "scale": [2, 2, 1]}}})J");
    Call(e, "entity.create", R"J({"name": "Middle", "components": {"MeshRenderer": {"mesh": "quad", "color": [0, 1, 0], "unlit": true, "opacity": 0.5}, "Transform": {"position": [0, 0, 0.5], "scale": [2, 2, 1]}}})J");
    Call(e, "entity.create", R"J({"name": "Back", "components": {"MeshRenderer": {"mesh": "quad", "color": [1, 0, 0], "unlit": true}, "Transform": {"scale": [2, 2, 1]}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(), 1.0f, view);
    RenderTarget rt;
    rt.Resize(64, 64);
    e.Renderer().Render(e.GetScene(), view, rt);
    Color c = MeanColor(rt, 30, 30, 34, 34);
    // red, then green at 50 %, then blue at 50 %: (0.25, 0.25, 0.5)
    CHECK(std::fabs(c.r - 0.25f) < 0.02f && std::fabs(c.g - 0.25f) < 0.02f && std::fabs(c.b - 0.5f) < 0.02f);
    CHECK(rt.IdAt(32, 32) == e.GetScene().FindByName("Front"));  // at least half opaque: pickable
    // An opaque surface in front hides transparent ones behind it (depth test, no depth write for glass).
    Call(e, "component.set", R"J({"id": "Back", "type": "Transform", "values": {"position": [0, 0, 2]}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    c = MeanColor(rt, 30, 30, 34, 34);
    CHECK(c.r > 0.98f && c.g < 0.02f && c.b < 0.02f);
    // Sprites: alphaCutoff 0 blends the image's alpha.
    Call(e, "component.set", R"J({"id": "Back", "type": "Transform", "values": {"position": [0, 0, -1]}})J");
    Call(e, "component.set", R"J({"id": "Front", "type": "MeshRenderer", "values": {"visible": false}})J");
    Call(e, "component.set", R"J({"id": "Middle", "type": "MeshRenderer", "values": {"visible": false}})J");
    Image img;
    img.width = img.height = 4;
    img.rgba.assign(64, 255);
    for (size_t i = 3; i < 64; i += 4) img.rgba[i] = 128;  // white at 50 % alpha
    CHECK(WritePng(JoinPath(e.ProjectDir(), "soft.png"), img, true));
    Call(e, "entity.create", R"J({"name": "Smoke", "components": {"Sprite": {"texture": "soft.png", "alphaCutoff": 0, "pixelsPerUnit": 1, "pixelArt": true}}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    c = MeanColor(rt, 30, 30, 34, 34);
    CHECK(std::fabs(c.r - 1.0f) < 0.02f && std::fabs(c.g - 0.5f) < 0.03f);  // white over red at ~50 %

    // Normal maps: a map tilted towards +u makes a plane lit from +x brighter.
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Cam", "components": {"Transform": {"position": [0, 4, 0], "rotation": [-90, 0, 0]}, "Camera": {"clearColor": [0, 0, 0]}}})J");
    Call(e, "entity.create", R"J({"name": "Sun", "components": {"Transform": {"rotation": [-30, 90, 0]}, "DirectionalLight": {"ambient": [0, 0, 0]}}})J");
    Call(e, "entity.create", R"J({"name": "Floor", "components": {"MeshRenderer": {"mesh": "plane", "color": [1, 1, 1]}, "Transform": {"scale": [4, 1, 4]}}})J");
    MakeSceneView(e.GetScene(), 1.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    Color flat = MeanColor(rt, 28, 28, 36, 36);
    Image nm;
    nm.width = nm.height = 4;
    nm.rgba.clear();
    for (int i = 0; i < 16; ++i) nm.rgba.insert(nm.rgba.end(), {218, 128, 218, 255});  // normal leaning to +u (+x on the plane)
    CHECK(WritePng(JoinPath(e.ProjectDir(), "tilt.png"), nm, true));
    Call(e, "material.create", R"J({"path": "tilt.mat.json", "values": {"normalTexture": "tilt.png", "roughness": 1}})J");
    Call(e, "component.set", R"J({"id": "Floor", "type": "MeshRenderer", "values": {"material": "tilt.mat.json"}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    Color bumped = MeanColor(rt, 28, 28, 36, 36);
    CHECK(bumped.r > flat.r + 0.1f);
    // Double-sided: seen from below, a plane only shows when its material is double-sided.
    Call(e, "component.set", R"J({"id": "Cam", "type": "Transform", "values": {"position": [0, -4, 0], "rotation": [90, 0, 0]}})J");
    Call(e, "component.set", R"J({"id": "Floor", "type": "MeshRenderer", "values": {"material": "", "unlit": true}})J");
    MakeSceneView(e.GetScene(), 1.0f, view);
    e.Renderer().Render(e.GetScene(), view, rt);
    CHECK(rt.IdAt(32, 32) == kNullEntity);
    Call(e, "material.create", R"J({"path": "two.mat.json", "values": {"doubleSided": true}})J");
    Call(e, "component.set", R"J({"id": "Floor", "type": "MeshRenderer", "values": {"material": "two.mat.json"}})J");
    e.Renderer().Render(e.GetScene(), view, rt);
    CHECK(rt.IdAt(32, 32) == e.GetScene().FindByName("Floor"));
}

TEST(GltfMaterialsAreLoaded) {
    // A glTF with a full metallic-roughness material: factors, alpha mode, double sided, emissive strength.
    std::string dir = TempProject("gltf_materials");
    const char* gltf = R"J({
      "asset": {"version": "2.0"}, "extensionsUsed": ["KHR_materials_emissive_strength"],
      "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "material": 0}]}],
      "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.4, 0.6, 0.5], "metallicFactor": 0.25, "roughnessFactor": 0.75},
                     "emissiveFactor": [1, 0.5, 0], "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 3}},
                     "alphaMode": "BLEND", "doubleSided": true}],
      "buffers": [{"byteLength": 36, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],
      "bufferViews": [{"buffer": 0, "byteLength": 36}],
      "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]}]
    })J";
    CHECK(WriteTextFile(JoinPath(dir, "tri.gltf"), gltf));
    Mesh mesh;
    std::string err;
    CHECK(LoadModelFile(JoinPath(dir, "tri.gltf"), mesh, &err));
    CHECK(mesh.materials.size() == 1 && mesh.submeshes.size() == 1 && mesh.submeshes[0].material == 0);
    const Material& m = mesh.materials[0];
    CHECK(std::fabs(m.baseColor.g - 0.4f) < 1e-6f && std::fabs(m.opacity - 0.5f) < 1e-6f);
    CHECK(std::fabs(m.metallic - 0.25f) < 1e-6f && std::fabs(m.roughness - 0.75f) < 1e-6f);
    CHECK(m.alphaMode == AlphaMode::Blend && m.doubleSided && m.emissiveIntensity == 3.0f && m.emissive.g == 0.5f);
    CHECK(mesh.tangents.size() == mesh.positions.size());
}

TEST(UIGpuMatchesSoftware) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("ui_gpu"), &err));
    e.Call("scene.new", Json::parse(R"J({"empty": true})J"));
    Call(e, "entity.create", R"J({"name": "Win", "components": {"UIPanel": {"anchor": "center", "x": 0, "y": 0, "width": 700, "height": 500, "radius": 24, "borderWidth": 4, "color": [0.1, 0.1, 0.2], "opacity": 0.9}, "UILayout": {"padding": 30, "spacing": 16, "crossAlign": "stretch"}}})J");
    Call(e, "entity.create", R"J({"name": "T", "parent": "Win", "components": {"UIText": {"text": "Title <b>bold</b> <color=orange>orange</color>", "size": 40, "outlineWidth": 2}}})J");
    Call(e, "entity.create", R"J({"name": "S", "parent": "Win", "components": {"UISlider": {"value": 0.4}}})J");
    Call(e, "entity.create", R"J({"name": "I", "parent": "Win", "components": {"UIImage": {"height": 60, "color": [0.9, 0.3, 0.3], "radius": 30, "fill": 0.6}}})J");
    Call(e, "entity.create", R"J({"name": "B", "parent": "Win", "components": {"UIButton": {"text": "OK"}}})J");
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    const int w = 640, h = 360;
    RenderView view;
    MakeSceneView(e.GetScene(), static_cast<float>(w) / h, view);
    RenderTarget sw, gpu;
    sw.Resize(w, h);
    gpu.Resize(w, h);
    e.Renderer().Render(e.GetScene(), view, sw);
    e.Gpu()->Render(e.GetScene(), view, gpu);
    // The frame is opaque: UI text/panels blended over it must not punch alpha
    // holes (they showed as dark boxes behind glyphs in the native editor).
    bool opaque = true;
    for (uint32_t c : gpu.color) opaque = opaque && (c >> 24) == 0xFF;
    CHECK(opaque);
    double total = 0;
    int outliers = 0;
    for (size_t i = 0; i < sw.color.size(); ++i) {
        int worst = 0;
        for (int k = 0; k < 3; ++k) {
            int d = std::abs(static_cast<int>((sw.color[i] >> (8 * k)) & 0xFF) - static_cast<int>((gpu.color[i] >> (8 * k)) & 0xFF));
            total += d;
            worst = std::max(worst, d);
        }
        outliers += worst > 24;
    }
    double mean = total / (sw.color.size() * 3.0);
    std::printf("  UI mean channel difference %.3f, outliers %d\n", mean, outliers);
    CHECK(mean < 0.5 && outliers < 50);
}

TEST(CameraPostProcessing) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("postprocess"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},"Camera":{"projection":"orthographic","clearColor":[1,1,1]}}})J");
    Call(e, "entity.create", R"J({"name":"UI","components":{"UIPanel":{"anchor":"top-left","x":0,"y":0,"width":200,"height":200,"color":[1,0,0],"opacity":1}}})J");
    RenderView view;
    CHECK(MakeSceneView(e.GetScene(), 128.0f / 72, view));
    RenderTarget original, neutral, effect;
    for (RenderTarget* target : {&original, &neutral, &effect}) target->Resize(128, 72);
    e.Renderer().Render(e.GetScene(), view, original);
    Call(e, "component.add", R"J({"id":"Camera","type":"PostProcess"})J");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, neutral);
    CHECK(original.Hash() == neutral.Hash());  // merely adding default settings never changes an existing frame
    Call(e, "component.set", R"J({"id":"Camera","type":"PostProcess","values":{"vignette":0.8,"vignetteRadius":0.3,"vignetteSoftness":0.5}})J");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, effect);
    SetMaxRenderThreads(4);
    e.Renderer().Render(e.GetScene(), view, neutral);
    SetMaxRenderThreads(16);
    CHECK(effect.Hash() == neutral.Hash() && effect.Hash() != original.Hash());
    CHECK((effect.color.back() & 255) < 60);  // edges darken, the center and overlaid UI retain their colors
    CHECK(effect.color[36 * 128 + 64] == original.color[36 * 128 + 64]);
    CHECK(effect.color[10 * 128 + 10] == original.color[10 * 128 + 10]);  // opaque UI retains its own color
    CHECK((effect.color[10 * 128 + 10] & 255) == 255);
    CHECK(effect.ids == original.ids && effect.depth == original.depth);
    Scene restored;
    CHECK(restored.FromJson(e.GetScene().ToJson(), &err));
    CHECK(restored.Get<PostProcess>(restored.FindByName("Camera"))->vignette == 0.8f);
    Call(e, "history.undo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, neutral);
    CHECK(neutral.Hash() == original.Hash());
    Call(e, "history.redo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    if (e.EnableGpu(nullptr, &err)) {
        RenderTarget gpu;
        gpu.Resize(128, 72);
        e.Gpu()->Render(e.GetScene(), view, gpu);
        double error = 0;
        for (size_t i = 0; i < effect.color.size(); ++i) for (int shift : {0, 8, 16}) {
            int a = static_cast<int>((effect.color[i] >> shift) & 255);
            int b = static_cast<int>((gpu.color[i] >> shift) & 255);
            error += std::abs(a - b);
        }
        double mean = error / static_cast<double>(effect.color.size() * 3);
        std::printf("  vignette software/GPU mean difference %.4f\n", mean);
        CHECK(mean < 1);
    } else std::printf("  SKIP postprocess GPU comparison (%s)\n", err.c_str());
    Call(e, "entity.create", R"J({"name":"Cube","components":{"Transform":{"position":[-4,3,0]},"MeshRenderer":{"unlit":true,"color":[0.2,0.4,0.6]}}})J");
    view.highlight = e.GetScene().FindByName("Cube");
    view.postProcess.vignette = 0;
    e.Renderer().Render(e.GetScene(), view, original);
    view.postProcess.vignette = 0.8f;
    e.Renderer().Render(e.GetScene(), view, effect);
    int outlinePixels = 0;
    for (size_t i = 0; i < original.color.size(); ++i) {
        uint32_t c = original.color[i];
        if ((c & 255) == 255 && ((c >> 8) & 255) == 158 && ((c >> 16) & 255) == 26) {
            ++outlinePixels;
            CHECK(effect.color[i] == c);  // the vignette never darkens editor selection feedback
        }
    }
    CHECK(outlinePixels > 0);
    // Reusing a view for another camera must not carry over the first camera's effects.
    Call(e, "component.set", R"J({"id":"Camera","type":"Camera","values":{"active":false}})J");
    Call(e, "entity.create", R"J({"name":"OtherCamera","components":{"Camera":{}}})J");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    CHECK(view.postProcess.vignette == 0);
    CHECK(MakeLookAtView(Vec3(0,0,10), Vec3(0,0,0), 60, 1).postProcess.vignette == 0);
}

TEST(HdrCameraToneMapping) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("hdr_postprocess"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "material.create", R"J({"path":"bright.mat.json","values":{"baseColor":[0,0,0],"unlit":true,"emissive":[1,0.5,0.25],"emissiveIntensity":8}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},"Camera":{"projection":"orthographic","clearColor":[0,0,0]},"PostProcess":{}}})J");
    Call(e, "entity.create", R"J({"name":"Bright","components":{"Transform":{"scale":[4,4,4]},"MeshRenderer":{"material":"bright.mat.json"}}})J");
    Call(e, "entity.create", R"J({"name":"UI","components":{"UIPanel":{"anchor":"top-left","x":0,"y":0,"width":200,"height":200,"color":[1,0,0],"opacity":1}}})J");
    RenderView view;
    RenderTarget legacy, exposed, mapped, again;
    for (RenderTarget* t : {&legacy, &exposed, &mapped, &again}) t->Resize(128, 72);
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, legacy);
    const size_t center = 36 * 128 + 64;
    CHECK((legacy.color[center] & 0xFFFFFF) == 0xFFFFFF);
    CHECK(Call(e, "component.set", R"J({"id":"Camera","type":"PostProcess","values":{"exposure":0.25}})J")["ok"].asBool());
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, exposed);
    CHECK((exposed.color[center] & 255) == 255);
    CHECK(((exposed.color[center] >> 16) & 255) == 128);  // emissive 2 * .25, not clipped 1 * .25
    CHECK(Call(e, "component.set", R"J({"id":"Camera","type":"PostProcess","values":{"toneMapping":"reinhard"}})J")["ok"].asBool());
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, mapped);
    SetMaxRenderThreads(4);
    e.Renderer().Render(e.GetScene(), view, again);
    SetMaxRenderThreads(16);
    CHECK(mapped.Hash() == again.Hash());
    CHECK((mapped.color[center] & 255) == 170);
    CHECK(((mapped.color[center] >> 8) & 255) == 128);
    CHECK(((mapped.color[center] >> 16) & 255) == 85);
    CHECK(mapped.ids == legacy.ids && mapped.depth == legacy.depth);
    CHECK(mapped.color[10 * 128 + 10] == legacy.color[10 * 128 + 10]);
    Scene restored;
    CHECK(restored.FromJson(e.GetScene().ToJson(), &err));
    const PostProcess* saved = restored.Get<PostProcess>(restored.FindByName("Camera"));
    CHECK(saved && saved->exposure == 0.25f && saved->toneMapping == "reinhard");
    Call(e, "history.undo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK(again.Hash() == exposed.Hash());
    Call(e, "history.redo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    if (e.EnableGpu(nullptr, &err)) {
        for (const auto& settings : {std::pair<float, std::string>{0.25f, "none"}, {0.25f, "reinhard"}, {1.0f, "none"}}) {
            view.postProcess.exposure = settings.first;
            view.postProcess.toneMapping = settings.second;
            e.Renderer().Render(e.GetScene(), view, again);
            e.Gpu()->Render(e.GetScene(), view, mapped);
            for (int shift : {0, 8, 16}) {
                CHECK(std::abs(static_cast<int>((mapped.color[center] >> shift) & 255) -
                               static_cast<int>((again.color[center] >> shift) & 255)) <= 1);
            }
            CHECK(mapped.color[10 * 128 + 10] == legacy.color[10 * 128 + 10]);
        }
    } else std::printf("  SKIP HDR GPU comparison (%s)\n", err.c_str());
    // Alpha blending must preserve radiance too, before tone mapping.
    CHECK(Call(e, "material.create", R"J({"path":"blend.mat.json","values":{"baseColor":[0,0,0],"unlit":true,"emissive":[1,0.5,0.25],"emissiveIntensity":8,"opacity":0.5,"alphaMode":"blend"}})J")["ok"].asBool());
    Call(e, "component.set", R"J({"id":"Bright","type":"MeshRenderer","values":{"material":"blend.mat.json"}})J");
    view.postProcess.exposure = 0.25f;
    view.postProcess.toneMapping = "reinhard";
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK((again.color[center] & 255) == 128);
    CHECK(((again.color[center] >> 8) & 255) == 85);
    CHECK(((again.color[center] >> 16) & 255) == 51);
    if (e.Gpu()) {
        e.Gpu()->Render(e.GetScene(), view, mapped);
        for (int shift : {0, 8, 16}) CHECK(std::abs(static_cast<int>((mapped.color[center] >> shift) & 255) -
                                                  static_cast<int>((again.color[center] >> shift) & 255)) <= 1);
    }
}

TEST(ParticleBillboardDrawItems) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("particle_items"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Effect","components":{"Transform":{"position":[1,2,3],"scale":[2,3,4]},"ParticleEmitter":{"rate":0,"speed":0,"gravity":[0,0,0],"lifetime":2,"startSize":2,"endSize":0,"startColor":[1,0,0],"endColor":[0,0,1],"startOpacity":1,"endOpacity":0,"columns":2,"frame":1}}})J");
    Call(e, "particles.burst", R"J({"id":"Effect","count":2})J");
    Call(e, "sim.step", R"J({"frames":60})J");
    EntityId id = e.GetScene().FindByName("Effect");
    RenderView view = MakeLookAtView(Vec3(7,5,8), Vec3(1,2,3), 45, 1);
    auto items = GatherRenderItems(e.GetScene(), &e.Assets(), view.view);
    CHECK(items.size() == 2);
    const RenderItem& item = items.front();
    CHECK(item.id == id && item.blend && item.unlit && !item.castShadows && item.pointSample);
    CHECK(Length(item.world.TransformPoint(Vec3()) - Vec3(1,2,3)) < 1e-5f);
    CHECK(std::fabs(Length(item.world.TransformDir(Vec3(1,0,0))) - 1) < 1e-4f);
    CHECK(std::fabs(item.tint.r - 0.5f) < 1e-4f && std::fabs(item.tint.b - 0.5f) < 1e-4f);
    CHECK(std::fabs(item.opacity - 0.5f) < 1e-4f);
    CHECK(item.uvOffset[0] == 0.5f && item.uvScale[0] == 0.5f);
    Vec3 horizontal = view.view.TransformDir(item.world.TransformDir(Vec3(1,0,0)));
    Vec3 vertical = view.view.TransformDir(item.world.TransformDir(Vec3(0,1,0)));
    CHECK(std::fabs(horizontal.y) < 1e-5f && std::fabs(horizontal.z) < 1e-5f && horizontal.x > 0);
    CHECK(std::fabs(vertical.x) < 1e-5f && std::fabs(vertical.z) < 1e-5f && vertical.y > 0);
    auto draws = BuildDrawList(items, view.eye);
    CHECK(draws.size() == 2 && draws[0].item == 0 && draws[1].item == 1);  // equal distance: birth order
    items[1].world = Mat4::Translation(Vec3(-10,2,3));
    draws = BuildDrawList(items, view.eye);
    CHECK(draws[0].item == 1 && draws[1].item == 0);
    Call(e, "component.set", R"J({"id":"Effect","type":"Transform","values":{"position":[5,2,3]}})J");
    items = GatherRenderItems(e.GetScene(), &e.Assets(), view.view);
    CHECK(Length(items[0].world.TransformPoint(Vec3()) - Vec3(5,2,3)) < 1e-5f);
    Call(e, "particles.clear", R"J({"id":"Effect"})J");
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"space":"world"}})J");
    Call(e, "particles.burst", R"J({"id":"Effect","count":1})J");
    Call(e, "component.set", R"J({"id":"Effect","type":"Transform","values":{"position":[9,2,3]}})J");
    items = GatherRenderItems(e.GetScene(), &e.Assets(), view.view);
    CHECK(Length(items[0].world.TransformPoint(Vec3()) - Vec3(5,2,3)) < 1e-5f);
    Call(e, "sim.step", R"J({"frames":121})J");
    CHECK(GatherRenderItems(e.GetScene(), &e.Assets(), view.view).empty());
}

TEST(ParticleRenderers) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("particle_renderers"), &err));
    Image sheet;
    sheet.width = 8;
    sheet.height = 4;
    sheet.rgba.resize(8 * 4 * 4);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 8; ++x) {
        size_t offset = static_cast<size_t>((y * 8 + x) * 4);
        sheet.rgba[offset] = x < 4 ? 255 : 0;
        sheet.rgba[offset + 1] = x < 4 ? 0 : 255;
        sheet.rgba[offset + 2] = 0;
        sheet.rgba[offset + 3] = 192;
    }
    CHECK(WritePng(JoinPath(e.ProjectDir(), "sheet.png"), sheet, true));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Effect","components":{"Transform":{},"ParticleEmitter":{"rate":0,"lifetime":2,"speed":2,"spread":160,"gravity":[0,0,0],"startSize":0.4,"endSize":0.2,"startColor":[1,1,1],"endColor":[0.5,1,0.5],"startOpacity":0.8,"endOpacity":0.4,"seed":23,"texture":"sheet.png","columns":2,"frame":1}}})J");
    EntityId id = e.GetScene().FindByName("Effect");
    bool hasGpu = e.EnableGpu(nullptr, &err);
    if (!hasGpu) std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
    RenderView view = MakeLookAtView(Vec3(3,2,5), Vec3(), 45, 320.0f / 180);
    view.clearColor = Color(0,0,0);
    view.drawUI = false;
    RenderTarget sw, again, gpu;
    sw.Resize(320,180);
    again.Resize(320,180);
    gpu.Resize(320,180);
    for (int dimensions : {2, 3}) {
        Json args = Json::parse(R"J({"id":"Effect","type":"ParticleEmitter","values":{}})J");
        args["values"]["dimensions"] = dimensions;
        CHECK(e.Call("component.set", args)["ok"].asBool());
        Call(e, "particles.clear", R"J({"id":"Effect"})J");
        Call(e, "particles.burst", R"J({"id":"Effect","count":24})J");
        Call(e, "sim.step", R"J({"frames":30})J");
        SetMaxRenderThreads(1);
        e.Renderer().Render(e.GetScene(), view, sw);
        SetMaxRenderThreads(16);
        e.Renderer().Render(e.GetScene(), view, again);
        CHECK(sw.Hash() == again.Hash() && CountId(sw, id) > 300);
        CHECK(sw.Hash() == (dimensions == 2 ? 0xeeb1529ca41029c9ull : 0x4ea73d9e1b71d479ull));
        // The second sheet frame is green with alpha: no red-frame bleed or opaque quads.
        size_t greenPixels = 0;
        for (uint32_t color : sw.color) {
            CHECK((color & 0xFF) == 0 && ((color >> 16) & 0xFF) == 0);
            greenPixels += ((color >> 8) & 0xFF) > 10;
        }
        CHECK(greenPixels > 300);
        CHECK(WritePng("build/particles-software.png", sw.ToImage(), true));
        if (hasGpu) {
            e.Gpu()->Render(e.GetScene(), view, gpu);
            double total = 0;
            int outliers = 0;
            double interiorTotal = 0;
            size_t interiorPixels = 0;
            for (size_t i = 0; i < sw.color.size(); ++i) {
                int worst = 0;
                for (int channel = 0; channel < 3; ++channel) {
                    int difference = std::abs(static_cast<int>((sw.color[i] >> (8 * channel)) & 255) -
                                              static_cast<int>((gpu.color[i] >> (8 * channel)) & 255));
                    total += difference;
                    worst = std::max(worst, difference);
                }
                outliers += worst > 32;
                int x = static_cast<int>(i % 320), y = static_cast<int>(i / 320);
                if (x > 0 && x < 319 && y > 0 && y < 179 && sw.color[i] != 0xFF000000u &&
                    sw.color[i] == sw.color[i - 1] && sw.color[i] == sw.color[i + 1] &&
                    sw.color[i] == sw.color[i - 320] && sw.color[i] == sw.color[i + 320]) {
                    interiorTotal += std::abs(static_cast<int>((sw.color[i] >> 8) & 255) -
                                              static_cast<int>((gpu.color[i] >> 8) & 255));
                    ++interiorPixels;
                }
            }
            double mean = total / (static_cast<double>(sw.color.size()) * 3);
            std::printf("  %dD particles: mean %.3f, outliers %d, software hash %016llx\n", dimensions,
                        mean, outliers, static_cast<unsigned long long>(sw.Hash()));
            // MSAA differs at many overlapping quad edges; fully covered interiors must agree.
            CHECK(mean < 1.0 && outliers < static_cast<int>(sw.color.size()) / 75);
            CHECK(interiorPixels > 200 && interiorTotal / static_cast<double>(interiorPixels) < 2.0);
            e.Gpu()->Render(e.GetScene(), view, again);
            CHECK(again.Hash() == gpu.Hash());
            CHECK(WritePng("build/particles-gpu.png", gpu.ToImage(), true));
        }
        uint64_t hash = sw.Hash();
        Call(e, "particles.clear", R"J({"id":"Effect"})J");
        Call(e, "particles.burst", R"J({"id":"Effect","count":24})J");
        Call(e, "sim.step", R"J({"frames":30})J");
        e.Renderer().Render(e.GetScene(), view, again);
        CHECK(again.Hash() == hash);
    }
    Call(e, "particles.clear", R"J({"id":"Effect"})J");
    Call(e, "component.set", R"J({"id":"Effect","type":"ParticleEmitter","values":{"texture":"","startColor":[1,0,0],"endColor":[1,0,0],"speed":0,"startSize":1,"startOpacity":1}})J");
    Call(e, "particles.burst", R"J({"id":"Effect","count":1})J");
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK((again.color[90 * 320 + 160] & 255) > 250 && ((again.color[90 * 320 + 160] >> 8) & 255) == 0);
}

TEST(CameraBloom) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("camera_bloom"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "material.create", R"J({"path":"bright.mat.json","values":{"baseColor":[0,0,0],"unlit":true,"emissive":[1,0.5,0.25],"emissiveIntensity":8}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},"Camera":{"projection":"orthographic","clearColor":[0,0,0]},"PostProcess":{"exposure":0.25,"toneMapping":"reinhard"}}})J");
    Call(e, "entity.create", R"J({"name":"Bright","components":{"MeshRenderer":{"material":"bright.mat.json"}}})J");
    Call(e, "entity.create", R"J({"name":"UI","components":{"UIPanel":{"anchor":"top-left","x":0,"y":0,"width":200,"height":200,"color":[1,1,1],"opacity":1}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    RenderTarget original, effect, again;
    for (RenderTarget* t : {&original, &effect, &again}) t->Resize(128, 72);
    e.Renderer().Render(e.GetScene(), view, original);
    CHECK(Call(e, "component.set", R"J({"id":"Camera","type":"PostProcess","values":{"bloom":1,"bloomThreshold":1,"bloomRadius":8}})J")["ok"].asBool());
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, effect);
    SetMaxRenderThreads(4);
    e.Renderer().Render(e.GetScene(), view, again);
    SetMaxRenderThreads(16);
    CHECK(effect.Hash() == again.Hash() && effect.Hash() != original.Hash());
    CHECK(effect.ids == original.ids && effect.depth == original.depth);
    CHECK(effect.color[10 * 128 + 10] == original.color[10 * 128 + 10]);
    CHECK((effect.color[21 * 128 + 21] & 0xFFFFFF) == 0);  // white UI never seeds a glow
    int glow = 0;
    for (size_t i = 0; i < original.color.size(); ++i) {
        if ((original.color[i] & 0xFFFFFF) == 0 && (effect.color[i] & 255) > 0) {
            ++glow;
            const uint32_t c = effect.color[i];
            CHECK((c & 255) >= ((c >> 8) & 255) && ((c >> 8) & 255) >= ((c >> 16) & 255));
        }
    }
    CHECK(glow > 50);
    Scene restored;
    CHECK(restored.FromJson(e.GetScene().ToJson(), &err));
    const PostProcess* saved = restored.Get<PostProcess>(restored.FindByName("Camera"));
    CHECK(saved && saved->bloom == 1 && saved->bloomThreshold == 1 && saved->bloomRadius == 8);
    Call(e, "history.undo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK(again.Hash() == original.Hash());
    Call(e, "history.redo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    view.postProcess.bloomThreshold = 32;
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK(again.Hash() == original.Hash());  // dim scene lies completely below the threshold
    view.postProcess.bloomThreshold = 1;
    const bool gpu = e.EnableGpu(nullptr, &err);
    if (!gpu) std::printf("  SKIP bloom GPU comparison (%s)\n", err.c_str());
    for (int radius : {1, 8, 32}) for (int width : {128, 97}) {
        view.postProcess.bloomRadius = radius;
        effect.Resize(width, width == 128 ? 72 : 55);
        again.Resize(effect.width, effect.height);
        e.Renderer().Render(e.GetScene(), view, effect);
        if (gpu) {
            e.Gpu()->Render(e.GetScene(), view, again);
            double error = 0;
            for (size_t i = 0; i < effect.color.size(); ++i) for (int shift : {0, 8, 16})
                error += std::abs(static_cast<int>((effect.color[i] >> shift) & 255) - static_cast<int>((again.color[i] >> shift) & 255));
            const double mean = error / static_cast<double>(effect.color.size() * 3);
            std::printf("  bloom radius %d width %d software/GPU mean difference %.4f\n", radius, width, mean);
            CHECK(mean < 1.5);
            // Toggling bloom on a reused size must not leave a stale glow texture.
            view.postProcess.bloom = 0;
            e.Renderer().Render(e.GetScene(), view, effect);
            e.Gpu()->Render(e.GetScene(), view, again);
            CHECK((effect.color.back() & 0xFFFFFF) == 0 && (again.color.back() & 0xFFFFFF) == 0);
            view.postProcess.bloom = 1;
        }
    }
    view.highlight = e.GetScene().FindByName("Bright");
    view.postProcess.bloom = 0;
    original.Resize(128, 72);
    effect.Resize(128, 72);
    e.Renderer().Render(e.GetScene(), view, original);
    view.postProcess.bloom = 1;
    e.Renderer().Render(e.GetScene(), view, effect);
    int outline = 0;
    for (size_t i = 0; i < original.color.size(); ++i) if ((original.color[i] & 0xFFFFFF) == 0x1A9EFF) {
        ++outline;
        CHECK(effect.color[i] == original.color[i]);
    }
    CHECK(outline > 0);
    view.highlight = kNullEntity;
    view.postProcess.exposure = 0;
    e.Renderer().Render(e.GetScene(), view, effect);
    CHECK((effect.color[36 * 128 + 64] & 0xFFFFFF) == 0);
    CHECK((effect.color[10 * 128 + 10] & 0xFFFFFF) == 0xFFFFFF);
    view.postProcess.exposure = 0.25f;
    // A constant HDR background keeps its brightness at clamped borders.
    CHECK(Call(e, "entity.delete", R"J({"id":"Bright"})J")["ok"].asBool());
    view.clearColor = Color(8, 4, 2);
    view.postProcess.bloomRadius = 32;
    for (RenderTarget* t : {&effect, &again}) t->Resize(128, 72);
    e.Renderer().Render(e.GetScene(), view, effect);
    CHECK((effect.color.back() & 0xFFFFFF) == (effect.color[36 * 128 + 64] & 0xFFFFFF));
    CHECK((effect.color.back() & 255) == 201);  // (8 + (8 - 1)) * .25, then Reinhard
    CHECK(((effect.color.back() >> 8) & 255) == 166);
    CHECK(((effect.color.back() >> 16) & 255) == 123);
    if (gpu) {
        e.Gpu()->Render(e.GetScene(), view, again);
        for (int shift : {0, 8, 16}) CHECK(std::abs(static_cast<int>((effect.color.back() >> shift) & 255) -
                                                  static_cast<int>((again.color.back() >> shift) & 255)) <= 1);
    }
}

TEST(ShaderMaterialAlphaAndShadow) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("shader_alpha_shadow"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "shader.create", R"J({"path":"alpha.shader.json","graph":{"uniforms":{"surface":[0.2,0.5,1,0]},
      "nodes":[{"op":"uniform","name":"surface"}],"color":0}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"alpha.mat.json","values":{"shader":"alpha.shader.json",
      "alphaMode":"mask","alphaCutoff":0.5,"doubleSided":true}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,7,10],"rotation":[-35,0,0]},
      "Camera":{"clearColor":[0.1,0.1,0.1]}}})J");
    Call(e, "entity.create", R"J({"name":"Sun","components":{"Transform":{"rotation":[-30,65,0]},
      "DirectionalLight":{"shadows":true,"ambient":[0.2,0.2,0.2]}}})J");
    Call(e, "entity.create", R"J({"name":"Ground","components":{"Transform":{"scale":[12,1,12]},
      "MeshRenderer":{"mesh":"plane","color":[0.7,0.7,0.7]}}})J");
    Call(e, "entity.create", R"J({"name":"Cube","components":{"Transform":{"position":[0,1,0],"scale":[2,2,2]},
      "MeshRenderer":{"material":"alpha.mat.json","visible":false}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(),160.0f/90,view);
    RenderTarget absent, transparent, opaque, gpuAbsent, gpuTransparent, gpuOpaque;
    for (RenderTarget* target : {&absent,&transparent,&opaque,&gpuAbsent,&gpuTransparent,&gpuOpaque}) target->Resize(160,90);
    e.Renderer().Render(e.GetScene(),view,absent);
    const bool gpu = e.EnableGpu(nullptr,&error);
    if (gpu) e.Gpu()->Render(e.GetScene(),view,gpuAbsent);
    CHECK(Call(e, "component.set", R"J({"id":"Cube","type":"MeshRenderer","values":{"visible":true}})J")["ok"].asBool());
    view.highlight = e.GetScene().FindByName("Cube");
    e.Renderer().Render(e.GetScene(),view,transparent);
    CHECK(transparent.Hash() == absent.Hash());
    CHECK(transparent.ids == absent.ids && transparent.depth == absent.depth);
    if (gpu) {
        e.Gpu()->Render(e.GetScene(),view,gpuTransparent);
        CHECK(gpuTransparent.Hash() == gpuAbsent.Hash());
    }
    CHECK(Call(e, "material.set", R"J({"path":"alpha.mat.json","values":{"shaderUniforms":{"surface":[0.2,0.5,1,1]}}})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(),view,opaque);
    CHECK(opaque.Hash() != transparent.Hash());
    int orange = 0, cubePixels = 0;
    for (size_t i = 0; i < opaque.color.size(); ++i) {
        orange += (opaque.color[i] & 0xFFFFFF) == 0x1A9EFF;
        cubePixels += opaque.ids[i] == view.highlight;
    }
    CHECK(orange > 20 && cubePixels > 100);
    if (gpu) {
        e.Gpu()->Render(e.GetScene(),view,gpuOpaque);
        CHECK(gpuOpaque.Hash() != gpuTransparent.Hash());
        int gpuOrange = 0;
        double difference = 0;
        for (size_t i = 0; i < opaque.color.size(); ++i) {
            const uint32_t color = gpuOpaque.color[i];
            gpuOrange += (color & 255) > 230 && ((color >> 8) & 255) > 140 &&
                         ((color >> 8) & 255) < 175 && ((color >> 16) & 255) < 50;
            for (int shift : {0,8,16})
                difference += std::abs(static_cast<int>((opaque.color[i] >> shift) & 255) -
                                       static_cast<int>((gpuOpaque.color[i] >> shift) & 255));
        }
        const double mean = difference/static_cast<double>(opaque.color.size()*3);
        std::printf("  graph alpha/shadow/outline software/GPU mean difference %.4f\n",mean);
        CHECK(gpuOrange > 20 && mean < 6);
    }
    CHECK(Call(e, "component.set", R"J({"id":"Cube","type":"MeshRenderer","values":{"castShadows":false}})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(),view,transparent);
    int shadowPixels = 0;
    const EntityId ground = e.GetScene().FindByName("Ground");
    for (size_t i = 0; i < opaque.color.size(); ++i)
        shadowPixels += opaque.ids[i] == ground && transparent.ids[i] == ground && opaque.color[i] != transparent.color[i];
    CHECK(shadowPixels > 20);
    if (gpu) {
        e.Gpu()->Render(e.GetScene(),view,gpuTransparent);
        int changed = 0;
        for (size_t i = 0; i < opaque.color.size(); ++i)
            changed += opaque.ids[i] == ground && transparent.ids[i] == ground && gpuOpaque.color[i] != gpuTransparent.color[i];
        CHECK(changed > 20);
    }
}

TEST(ShaderMaterialVaryingAlphaAndOcclusion) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("shader_varying_alpha"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "shader.create", R"J({"path":"half.shader.json","graph":{"nodes":[
      {"op":"uv"},{"op":"constant","value":0.5},{"op":"step","args":[1,0]},
      {"op":"swizzle","args":[2],"value":[0,0,0,0]},
      {"op":"constant","value":[0.2,0.5,1,1]},{"op":"multiply","args":[3,4]}],"color":5}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"half.mat.json","values":{"shader":"half.shader.json",
      "alphaMode":"mask","alphaCutoff":0.5,"doubleSided":true,"unlit":true}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"reference.mat.json","values":{"baseColor":[0.2,0.5,1],
      "doubleSided":true,"unlit":true}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,7,10],"rotation":[-35,0,0]},
      "Camera":{"clearColor":[0.1,0.1,0.1]}}})J");
    Call(e, "entity.create", R"J({"name":"Sun","components":{"Transform":{"rotation":[-30,65,0]},
      "DirectionalLight":{"shadows":true,"ambient":[0.2,0.2,0.2]}}})J");
    Call(e, "entity.create", R"J({"name":"Ground","components":{"Transform":{"scale":[12,1,12]},
      "MeshRenderer":{"mesh":"plane","color":[0.7,0.7,0.7]}}})J");
    Call(e, "entity.create", R"J({"name":"Mask","components":{"Transform":{"position":[0,2,0],"rotation":[-90,0,0],"scale":[4,4,1]},
      "MeshRenderer":{"mesh":"quad","material":"half.mat.json"}}})J");
    // An independent half-width quad has exactly the geometry retained by step(0.5, uv.x).
    CHECK(Call(e, "entity.create", R"J({"name":"Reference","components":{"Transform":{"position":[1,2,0],"rotation":[-90,0,0],"scale":[2,4,1]},
      "MeshRenderer":{"mesh":"quad","material":"reference.mat.json","visible":false}}})J")["ok"].asBool());
    RenderView view;
    MakeSceneView(e.GetScene(), 160.0f / 90, view);
    RenderTarget masked, reference, noShadow, gpuMasked, gpuReference, gpuNoShadow;
    for (RenderTarget* target : {&masked, &reference, &noShadow, &gpuMasked, &gpuReference, &gpuNoShadow}) target->Resize(160,90);
    const bool gpu = e.EnableGpu(nullptr, &error);
    if (!gpu) std::printf("  SKIP varying graph alpha GPU comparison (%s)\n", error.c_str());
    e.Renderer().Render(e.GetScene(), view, masked);
    if (gpu) e.Gpu()->Render(e.GetScene(), view, gpuMasked);
    CHECK(Call(e, "component.set", R"J({"id":"Mask","type":"MeshRenderer","values":{"castShadows":false}})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(), view, noShadow);
    if (gpu) e.Gpu()->Render(e.GetScene(), view, gpuNoShadow);
    CHECK(Call(e, "component.set", R"J({"id":"Mask","type":"MeshRenderer","values":{"visible":false}})J")["ok"].asBool());
    CHECK(Call(e, "component.set", R"J({"id":"Reference","type":"MeshRenderer","values":{"visible":true}})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(), view, reference);
    if (gpu) e.Gpu()->Render(e.GetScene(), view, gpuReference);
    const EntityId ground = e.GetScene().FindByName("Ground");
    int shadowPixels = 0, gpuShadowPixels = 0, compared = 0;
    double cpuDifference = 0, gpuDifference = 0, backendDifference = 0;
    for (size_t i = 0; i < masked.color.size(); ++i) {
        if (masked.ids[i] != ground || reference.ids[i] != ground || noShadow.ids[i] != ground) continue;
        ++compared;
        shadowPixels += masked.color[i] != noShadow.color[i];
        if (gpu) gpuShadowPixels += gpuMasked.color[i] != gpuNoShadow.color[i];
        for (int shift : {0,8,16}) {
            const auto channel = [shift](uint32_t color) { return static_cast<int>((color >> shift) & 255); };
            cpuDifference += std::abs(channel(masked.color[i]) - channel(reference.color[i]));
            if (gpu) {
                gpuDifference += std::abs(channel(gpuMasked.color[i]) - channel(gpuReference.color[i]));
                backendDifference += std::abs(channel(masked.color[i]) - channel(gpuMasked.color[i]));
            }
        }
    }
    CHECK(compared > 1000 && shadowPixels > 20);
    CHECK(cpuDifference / static_cast<double>(std::max(1, compared) * 3) < 1);
    if (gpu) {
        std::printf("  varying graph shadow reference/GPU mean difference %.4f, software/GPU %.4f\n",
                    gpuDifference / static_cast<double>(std::max(1, compared) * 3),
                    backendDifference / static_cast<double>(std::max(1, compared) * 3));
        CHECK(gpuShadowPixels > 20);
        CHECK(gpuDifference / static_cast<double>(std::max(1, compared) * 3) < 1);
        CHECK(backendDifference / static_cast<double>(std::max(1, compared) * 3) < 6);
    }

    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},
      "Camera":{"projection":"orthographic","clearColor":[0,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Back","components":{"Transform":{"scale":[3,3,1]},
      "MeshRenderer":{"mesh":"quad","color":[0,1,0],"unlit":true}}})J");
    Call(e, "entity.create", R"J({"name":"Mask","components":{"Transform":{"position":[0,0,1],"scale":[4,4,1]},
      "MeshRenderer":{"mesh":"quad","material":"half.mat.json"}}})J");
    Call(e, "entity.create", R"J({"name":"Occluder","components":{"Transform":{"position":[-1,0,2],"scale":[2,4,1]},
      "MeshRenderer":{"mesh":"quad","color":[1,0,0],"unlit":true,"visible":false}}})J");
    MakeSceneView(e.GetScene(), 160.0f / 90, view);
    view.highlight = e.GetScene().FindByName("Back");
    const auto outline = [](uint32_t color) {
        return (color & 255) > 230 && ((color >> 8) & 255) > 140 &&
               ((color >> 8) & 255) < 175 && ((color >> 16) & 255) < 50;
    };
    e.Renderer().Render(e.GetScene(), view, masked);
    if (gpu) e.Gpu()->Render(e.GetScene(), view, gpuMasked);
    int visibleBack = 0, cpuOutline = 0, gpuOutline = 0;
    for (size_t i = 0; i < masked.color.size(); ++i) {
        visibleBack += masked.ids[i] == view.highlight;
        cpuOutline += outline(masked.color[i]);
        if (outline(masked.color[i])) CHECK(i % 160 <= 81); // only the hole exposes the selected quad
        if (gpu) {
            gpuOutline += outline(gpuMasked.color[i]);
            if (outline(gpuMasked.color[i])) CHECK(i % 160 <= 81);
        }
    }
    CHECK(visibleBack > 100 && cpuOutline > 20);
    if (gpu) CHECK(gpuOutline > 20);
    // A standard opaque material closes the remaining hole: neither selection pass may leak.
    CHECK(Call(e, "component.set", R"J({"id":"Occluder","type":"MeshRenderer","values":{"visible":true}})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(), view, reference);
    if (gpu) e.Gpu()->Render(e.GetScene(), view, gpuReference);
    for (size_t i = 0; i < reference.color.size(); ++i) {
        CHECK(reference.ids[i] != view.highlight && !outline(reference.color[i]));
        if (gpu) CHECK(!outline(gpuReference.color[i]));
    }
    // The selected graph itself must also respect an ordinary material's foreground depth.
    view.highlight = e.GetScene().FindByName("Mask");
    CHECK(Call(e, "component.set", R"J({"id":"Occluder","type":"Transform","values":{"position":[1,0,2],"scale":[2.2,4.2,1]}})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(), view, reference);
    if (gpu) e.Gpu()->Render(e.GetScene(), view, gpuReference);
    for (size_t i = 0; i < reference.color.size(); ++i) {
        CHECK(reference.ids[i] != view.highlight && !outline(reference.color[i]));
        if (gpu) CHECK(!outline(gpuReference.color[i]));
    }
}

TEST(ShaderMaterialInstructionFamilies) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("shader_instruction_families"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},
      "Camera":{"projection":"orthographic","clearColor":[0,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Surface","components":{"Transform":{"scale":[4,4,1]},
      "MeshRenderer":{"material":"program.mat.json"}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(),128.0f/72,view);
    RenderTarget cpu, gpu;
    cpu.Resize(128,72);
    gpu.Resize(128,72);
    const bool hasGpu = e.EnableGpu(nullptr,&error);
    if (!hasGpu) std::printf("  SKIP graph instruction GPU comparison (%s)\n",error.c_str());
    struct Case { const char* op; const char* args; Vec3 expected; };
    // Independent numeric expectations catch matching mistakes in both evaluators.
    const Case cases[] = {
        {"add", "[0,1]", {0.4f,0.2f,0.7f}},
        {"subtract", "[0,1]", {0,0.2f,0.5f}},
        {"multiply", "[0,1]", {0.03f,0,0.06f}},
        {"divide", "[0,1]", {1.0f/3,0,1}},
        {"min", "[0,1]", {0.1f,0,0.1f}},
        {"max", "[0,1]", {0.3f,0.2f,0.6f}},
        {"sin", "[0]", {std::sin(0.1f),std::sin(0.2f),std::sin(0.6f)}},
        {"cos", "[0]", {std::cos(0.1f),std::cos(0.2f),std::cos(0.6f)}},
        {"floor", "[2]", {0,0,1}},
        {"fract", "[2]", {0,0.25f,0.75f}},
        {"abs", "[2]", {1,0.75f,1}},
        {"clamp", "[2,0,1]", {0.1f,0,0.1f}},
        {"mix", "[0,1,2]", {0,0.35f,0}},
        {"step", "[0,1]", {1,0,0}},
        {"dot", "[0,1]", {0.34f,0.34f,0.34f}},
        {"normalize", "[0]", {0.1f/std::sqrt(0.66f),0.2f/std::sqrt(0.66f),0.6f/std::sqrt(0.66f)}},
        {"swizzle", "[0]", {0.5f,0.6f,0.2f}}
    };
    double worstMean = 0;
    auto render = [&]() {
        e.Renderer().Render(e.GetScene(),view,cpu);
        if (!hasGpu) return;
        e.Gpu()->Render(e.GetScene(),view,gpu);
        double difference = 0;
        for (size_t i = 0; i < cpu.color.size(); ++i) for (int shift : {0,8,16})
            difference += std::abs(static_cast<int>((cpu.color[i] >> shift) & 255) -
                                   static_cast<int>((gpu.color[i] >> shift) & 255));
        const double mean = difference/static_cast<double>(cpu.color.size()*3);
        worstMean = std::max(worstMean,mean);
        CHECK(mean < 2);
    };
    auto install = [&](const Json& graph) {
        Json args = Json::MakeObject();
        args["path"] = "program.shader.json";
        args["graph"] = graph;
        args["overwrite"] = true;
        CHECK(e.Call("shader.create",args)["ok"].asBool());
        CHECK(Call(e, "material.create", R"J({"path":"program.mat.json","overwrite":true,
          "values":{"shader":"program.shader.json","unlit":true,"pixelArt":true}})J")["ok"].asBool());
    };
    for (const Case& item : cases) {
        Json graph = Json::parse(R"J({"nodes":[{"op":"constant","value":[0.1,0.2,0.6,0.5]},
          {"op":"constant","value":[0.3,0,0.1,0.5]},{"op":"constant","value":[-1,-0.75,1.75,0]}],"color":3})J");
        Json node = Json::MakeObject();
        node["op"] = item.op;
        node["args"] = Json::parse(item.args);
        if (std::string(item.op) == "swizzle") node["value"] = Json(Json::Array{3,2,1,0});
        graph["nodes"].push(node);
        install(graph);
        render();
        const float expected[] = {item.expected.x,item.expected.y,item.expected.z};
        for (int channel = 0; channel < 3; ++channel) {
            const int value = static_cast<int>((cpu.color[36*128+64] >> (channel*8)) & 255);
            if (std::abs(value-static_cast<int>(expected[channel]*255+0.5f)) > 1)
                std::printf("  graph %s channel %d: got %d, expected %.4f\n",item.op,channel,value,expected[channel]*255);
            CHECK(std::abs(value-static_cast<int>(expected[channel]*255+0.5f)) <= 1);
        }
    }
    Image texture;
    texture.width = texture.height = 2;
    texture.rgba = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255};
    CHECK(WritePng(JoinPath(e.ProjectDir(),"graph.png"),texture,true));
    for (const char* input : {"uv","position","normal","baseColor","time","texture"}) {
        Json graph = Json::parse(R"J({"nodes":[{"op":"uv"},{"op":"constant","value":0.25}],"color":2})J");
        Json node = Json::MakeObject();
        node["op"] = input;
        if (std::string(input) == "texture") node["args"] = Json(Json::Array{0});
        graph["nodes"].push(node);
        install(graph);
        CHECK(Call(e, "material.set", R"J({"path":"program.mat.json","values":{"baseTexture":"graph.png",
          "baseColor":[0.5,0.25,0.75]}})J")["ok"].asBool());
        view.shaderTime = 0.25f;
        render();
        if (std::string(input) == "texture") {
            for (uint32_t expected : {0x0000FFu,0x00FF00u,0xFF0000u,0x00FFFFu}) {
                int pixels = 0;
                for (uint32_t color : cpu.color) pixels += (color & 0xFFFFFF) == expected;
                CHECK(pixels > 20);
            }
        }
        if (std::string(input) == "normal") CHECK((cpu.color[36*128+64] & 0xFFFFFF) == 0xFF0000);
        if (std::string(input) == "time") {
            CHECK((cpu.color[36*128+64] & 0xFFFFFF) == 0x404040);
            const uint64_t first = cpu.Hash();
            view.shaderTime = 0.75f;
            render();
            CHECK(cpu.Hash() != first && (cpu.color[36*128+64] & 0xFFFFFF) == 0xBFBFBF);
        }
    }
    install(Json::parse(R"J({"nodes":[{"op":"constant","value":[4,1,0.5,1]}],"color":0,"emissive":0})J"));
    view.postProcess.exposure = 0.25f;
    view.postProcess.toneMapping = "reinhard";
    render();
    CHECK((cpu.color[36*128+64] & 0xFFFFFF) == 0x3355AA);  // (base + emission) before exposure/Reinhard
    install(Json::parse(R"J({"nodes":[{"op":"constant","value":[4,1,0.5,0.25]}],"color":0,"emissive":0})J"));
    CHECK(Call(e, "material.set", R"J({"path":"program.mat.json","values":{"alphaMode":"blend","opacity":0}})J")["ok"].asBool());
    render();
    const uint32_t blended = cpu.color[36*128+64];
    CHECK((blended & 0xFFFFFF) == 0x0F1C55);  // graph replaces zero input opacity; blend happens in HDR
    CHECK(cpu.ids[36*128+64] == kNullEntity);  // alpha below 0.5 does not take picking ownership
    std::printf("  graph instruction/input/HDR worst software/GPU mean difference %.4f\n",worstMean);
}

TEST(ShaderMaterialRendering) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("shader_material"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "shader.create", R"J({"path":"materials/stripes.shader.json","graph":{"uniforms":{"frequency":4},
      "nodes":[{"op":"uv"},{"op":"swizzle","args":[0],"value":[0,0,0,0]},
      {"op":"uniform","name":"frequency"},{"op":"multiply","args":[1,2]},{"op":"fract","args":[3]},
      {"op":"constant","value":0.5},{"op":"step","args":[5,4]},
      {"op":"constant","value":[1,0,0,1]},{"op":"constant","value":[0,0,1,1]},
      {"op":"mix","args":[7,8,6]}],"color":9}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"stripes.mat.json","values":{"unlit":true}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},
      "Camera":{"projection":"orthographic","clearColor":[0,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Cube","components":{"Transform":{"scale":[4,4,1]},
      "MeshRenderer":{"material":"stripes.mat.json"}}})J");
    RenderTarget original, stripes, again;
    for (RenderTarget* target : {&original,&stripes,&again}) target->Resize(128,72);
    e.RenderGameView(original);
    CHECK(Call(e, "material.set", R"J({"path":"stripes.mat.json","values":{
      "shaderUniforms":{"frequency":4},"shader":"materials/stripes.shader.json"}})J")["ok"].asBool());
    SetMaxRenderThreads(1);
    e.RenderGameView(stripes);
    SetMaxRenderThreads(4);
    e.RenderGameView(again);
    SetMaxRenderThreads(16);
    CHECK(stripes.Hash() != original.Hash() && stripes.Hash() == again.Hash());
    CHECK(stripes.ids == original.ids && stripes.depth == original.depth);
    int red = 0, blue = 0;
    for (uint32_t color : stripes.color) {
        red += (color & 0xFFFFFF) == 0x0000FF;
        blue += (color & 0xFFFFFF) == 0xFF0000;
    }
    CHECK(red > 100 && blue > 100);
    RenderView view;
    MakeSceneView(e.GetScene(),128.0f/72,view);
    if (e.EnableGpu(nullptr,&error)) {
        e.Gpu()->Render(e.GetScene(),view,again);
        double difference = 0;
        for (size_t i = 0; i < stripes.color.size(); ++i) for (int shift : {0,8,16})
            difference += std::abs(static_cast<int>((stripes.color[i] >> shift) & 255) -
                                   static_cast<int>((again.color[i] >> shift) & 255));
        const double mean = difference / static_cast<double>(stripes.color.size()*3);
        std::printf("  surface graph software/GPU mean difference %.4f\n",mean);
        CHECK(mean < 3);
    } else std::printf("  SKIP surface graph GPU comparison (%s)\n",error.c_str());
    CHECK(!Call(e, "material.set", R"J({"path":"stripes.mat.json","values":{"shaderUniforms":{"typo":2}}})J")["ok"].asBool());
    e.RenderGameView(again);
    CHECK(again.Hash() == stripes.Hash());
    CHECK(Call(e, "material.set", R"J({"path":"stripes.mat.json","values":{"shaderUniforms":{"frequency":2}}})J")["ok"].asBool());
    e.RenderGameView(again);
    CHECK(again.Hash() != stripes.Hash());
    const auto oldGraph = e.Assets().GetShader("materials/stripes.shader.json");
    std::string source;
    CHECK(ReadTextFile(JoinPath(e.ProjectDir(),"materials/stripes.shader.json"),source));
    Json changed = Json::parse(source);
    changed["nodes"][7]["value"] = Json(Json::Array{0,1,0,1});
    CHECK(WriteTextFile(JoinPath(e.ProjectDir(),"materials/stripes.shader.json"),changed.dump(2)));
    const auto path = std::filesystem::path(JoinPath(e.ProjectDir(),"materials/stripes.shader.json"));
    std::filesystem::last_write_time(path,std::filesystem::last_write_time(path)+std::chrono::seconds(2));
    CHECK(!e.Assets().PollChanges().empty());
    CHECK(e.Assets().GetShader("materials/stripes.shader.json") != oldGraph);
    e.RenderGameView(stripes);
    CHECK(stripes.Hash() != again.Hash());
    CHECK(Call(e, "asset.info", R"J({"path":"materials/stripes.shader.json"})J")["result"]["nodes"].asInt() == 10);
    CHECK(Call(e, "material.set", R"J({"path":"stripes.mat.json","values":{"shader":"","shaderUniforms":{}}})J")["ok"].asBool());
    e.RenderGameView(again);
    CHECK(again.Hash() == original.Hash());
}

TEST(MeshRendererShaderUniforms) {
    // Per-entity graph uniforms: two entities share one material file and draw different colors.
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("entity_uniforms"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "shader.create", R"J({"path":"materials/tint.shader.json","graph":{"uniforms":{"tint":[1,0,0,1],"gain":1},
      "nodes":[{"op":"uniform","name":"tint"},{"op":"uniform","name":"gain"},{"op":"multiply","args":[0,1]}],"color":2}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"tint.mat.json","values":{"unlit":true,
      "shader":"materials/tint.shader.json","shaderUniforms":{"gain":[1,1,1,1]}}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},
      "Camera":{"projection":"orthographic","clearColor":[0,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Left","components":{"Transform":{"position":[-3,0,0],"scale":[4,4,1]},
      "MeshRenderer":{"mesh":"quad","material":"tint.mat.json"}}})J");
    Call(e, "entity.create", R"J({"name":"Right","components":{"Transform":{"position":[3,0,0],"scale":[4,4,1]},
      "MeshRenderer":{"mesh":"quad","material":"tint.mat.json","shaderUniforms":{"tint":[0,1,0,1]}}}})J");
    RenderTarget shot, again;
    for (RenderTarget* target : {&shot, &again}) target->Resize(128, 72);
    auto count = [&](const RenderTarget& target, uint32_t rgb) {
        int n = 0;
        for (uint32_t color : target.color) n += (color & 0xFFFFFF) == rgb;
        return n;
    };
    e.RenderGameView(shot);
    CHECK(count(shot, 0x0000FF) > 300 && count(shot, 0x00FF00) > 300);  // red from the material, green from the entity
    // Unnamed uniforms keep the material's values; the override is saved with the scene and undoable.
    CHECK(Call(e, "component.set", R"J({"id":"Left","type":"MeshRenderer","values":{"shaderUniforms":{"gain":0.5}}})J")["ok"].asBool());
    e.RenderGameView(again);
    CHECK(count(again, 0x0000FF) == 0 && count(again, 0x00FF00) == count(shot, 0x00FF00));
    CHECK(count(again, 0x00007F) + count(again, 0x000080) > 300);
    CHECK(e.GetScene().ToJson().dump().find("\"gain\"") != std::string::npos);
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());
    e.RenderGameView(again);
    CHECK(again.Hash() == shot.Hash());
    // Scripts change them every frame through scene.set.
    CHECK(Call(e, "script.eval", R"J({"code":"scene.set(scene.find('Right'), 'MeshRenderer', {shaderUniforms = {tint = {0, 0, 1, 1}}})"})J")["ok"].asBool());
    e.RenderGameView(again);
    CHECK(count(again, 0xFF0000) == count(shot, 0x00FF00));
    RenderView view;
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    if (e.EnableGpu(nullptr, &error)) {
        RenderTarget gpu;
        gpu.Resize(128, 72);
        e.Gpu()->Render(e.GetScene(), view, gpu);
        double difference = 0;
        for (size_t i = 0; i < again.color.size(); ++i) for (int shift : {0, 8, 16})
            difference += std::abs(static_cast<int>((again.color[i] >> shift) & 255) - static_cast<int>((gpu.color[i] >> shift) & 255));
        CHECK(difference / static_cast<double>(again.color.size() * 3) < 3);
    } else std::printf("  SKIP per-entity uniform GPU comparison (%s)\n", error.c_str());
    // A name the graph does not declare, or uniforms without a graph, is as loud as a broken material.
    CHECK(Call(e, "component.set", R"J({"id":"Right","type":"MeshRenderer","values":{"shaderUniforms":{"typo":1}}})J")["ok"].asBool());
    bool flagged = false;
    for (const RenderItem& item : GatherRenderItems(e.GetScene(), &e.Assets()))
        if (item.id == e.GetScene().FindByName("Right")) flagged = item.error;
    CHECK(flagged);
    e.RenderGameView(again);
    CHECK(count(again, 0xFF00FF) == count(shot, 0x00FF00));
}

TEST(SpriteLayerAndAdditiveBlend) {
    // Sprite.layer decides the order of blended sprites whatever their ids or where they are on screen,
    // and blend "add" brightens what is behind instead of covering it.
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("sprite_layers"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},
      "Camera":{"projection":"orthographic","orthoSize":2,"clearColor":[0,0,0]}}})J");
    // Red is created first (lower id) and sits far to the side; green overlaps it at the origin.
    Call(e, "entity.create", R"J({"name":"Red","components":{"Transform":{"position":[1.5,0,0]},
      "Sprite":{"width":5,"height":2,"alphaCutoff":0,"color":[1,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Green","components":{"Transform":{"position":[0,0,0]},
      "Sprite":{"width":1,"height":1,"alphaCutoff":0,"color":[0,1,0]}}})J");
    RenderTarget shot;
    shot.Resize(128, 72);
    auto center = [&] { e.RenderGameView(shot); return shot.color[36 * 128 + 64] & 0xFFFFFF; };
    CHECK(center() == 0x00FF00);  // same layer: nearer bounds centre last, then id
    CHECK(Call(e, "component.set", R"J({"id":"Red","type":"Sprite","values":{"layer":1}})J")["ok"].asBool());
    CHECK(center() == 0x0000FF);  // the higher layer wins although it has the lower id
    CHECK(Call(e, "component.set", R"J({"id":"Green","type":"Sprite","values":{"layer":2}})J")["ok"].asBool());
    CHECK(center() == 0x00FF00);
    CHECK(Call(e, "component.set", R"J({"id":"Green","type":"Sprite","values":{"blend":"add"}})J")["ok"].asBool());
    CHECK(center() == 0x00FFFF);  // red + green
    CHECK(shot.ids[36 * 128 + 64] == e.GetScene().FindByName("Red"));  // additive sprites are not picked
    // A cut-out sprite of a higher layer also stays in front (the layer nudges its depth).
    CHECK(Call(e, "component.set", R"J({"id":"Green","type":"Sprite","values":{"blend":"alpha","alphaCutoff":0.5,"layer":0}})J")["ok"].asBool());
    CHECK(Call(e, "component.set", R"J({"id":"Red","type":"Sprite","values":{"alphaCutoff":0.5,"layer":3}})J")["ok"].asBool());
    CHECK(center() == 0x0000FF);
    RenderView view;
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    Call(e, "component.set", R"J({"id":"Red","type":"Sprite","values":{"alphaCutoff":0,"layer":0}})J");
    Call(e, "component.set", R"J({"id":"Green","type":"Sprite","values":{"alphaCutoff":0,"blend":"add","layer":1}})J");
    e.RenderGameView(shot);
    if (e.EnableGpu(nullptr, &error)) {
        RenderTarget gpu;
        gpu.Resize(128, 72);
        e.Gpu()->Render(e.GetScene(), view, gpu);
        CHECK((gpu.color[36 * 128 + 64] & 0xFFFFFF) == 0x00FFFF);
    } else std::printf("  SKIP additive GPU check (%s)\n", error.c_str());
}

TEST(ParticleVariationAndDrag) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("particle_variation"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Plain","components":{"ParticleEmitter":{"rate":0,"burst":40,"loop":false,"lifetime":2,
      "speed":4,"spread":180,"gravity":[0,0,0],"dimensions":2,"space":"world","maxParticles":64}}})J");
    Call(e, "entity.create", R"J({"name":"Varied","components":{"ParticleEmitter":{"rate":0,"burst":40,"loop":false,"lifetime":2,
      "speed":4,"speedVariation":0.8,"lifetimeVariation":0.5,"sizeVariation":0.5,"drag":3,"spread":180,"gravity":[0,0,0],
      "dimensions":2,"space":"world","maxParticles":64}}})J");
    CHECK(Call(e, "sim.step", R"J({"frames":30})J")["ok"].asBool());
    auto spread = [&](const char* name, float& slowest, float& fastest, float& shortest, float& longest) {
        const ParticleEmitter* emitter = e.GetScene().Get<ParticleEmitter>(e.GetScene().FindByName(name));
        slowest = shortest = 1e9f;
        fastest = longest = 0;
        for (const Particle& particle : emitter->particles) {
            const float speed = Length(particle.velocity);
            slowest = std::min(slowest, speed); fastest = std::max(fastest, speed);
            shortest = std::min(shortest, particle.lifetime); longest = std::max(longest, particle.lifetime);
        }
        return emitter->particles.size();
    };
    float lo, hi, shortest, longest;
    CHECK(spread("Plain", lo, hi, shortest, longest) == 40);
    CHECK(std::fabs(lo - 4) < 1e-3f && std::fabs(hi - 4) < 1e-3f && shortest == 2 && longest == 2);
    CHECK(spread("Varied", lo, hi, shortest, longest) == 40);
    CHECK(hi < 4 * 0.3f);                    // drag: 3/s for half a second leaves under a quarter of the speed
    CHECK(hi > lo * 2);                       // and the particles started at different speeds
    CHECK(shortest >= 1 - 1e-3f && longest <= 2 + 1e-3f && longest - shortest > 0.4f);
    CHECK(Call(e, "component.set", R"J({"id":"Varied","type":"ParticleEmitter","values":{"drag":"x"}})J")["ok"].asBool() == false);
}

TEST(Light2DDarkness) {
    // Darkness2D on the camera covers the 2D view; Light2D entities cut soft holes; higher layers stay visible.
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("light2d"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},
      "Camera":{"projection":"orthographic","orthoSize":4,"clearColor":[1,1,1]},
      "Darkness2D":{"color":[0,0,0],"opacity":1,"layer":5}}})J");
    Call(e, "entity.create", R"J({"name":"Floor","components":{"Sprite":{"width":40,"height":40,"color":[1,1,1]}}})J");
    Call(e, "entity.create", R"J({"name":"Lamp","components":{"Transform":{"position":[0,0,0]},"Light2D":{"radius":2,"inner":0.5}}})J");
    Call(e, "entity.create", R"J({"name":"Marker","components":{"Transform":{"position":[-6,3,0]},
      "Sprite":{"width":1,"height":1,"alphaCutoff":0,"color":[1,0,0],"layer":6}}})J");
    RenderTarget shot;
    shot.Resize(160, 90);
    auto at = [&](float x, float y) {  // world position -> pixel (orthoSize 4: 8 units over 90 px)
        const int px = 80 + static_cast<int>(std::lround(x * 90.0f / 8.0f)), py = 45 - static_cast<int>(std::lround(y * 90.0f / 8.0f));
        return shot.color[static_cast<size_t>(py) * 160 + static_cast<size_t>(px)] & 0xFFFFFF;
    };
    e.RenderGameView(shot);
    CHECK(at(0, 0) == 0xFFFFFF);          // fully lit inside the inner radius
    CHECK(at(3.5f, 0) == 0x000000);       // dark outside the light
    const uint32_t edge = at(1.6f, 0) & 0xFF;
    CHECK(edge > 20 && edge < 235);       // soft falloff between
    CHECK(at(-6, 3) == 0x0000FF);         // a blended sprite on a higher layer is not darkened
    const EntityId floor = e.GetScene().FindByName("Floor");
    CHECK(shot.ids[45 * 160 + 150] == floor);  // the overlay is never picked
    // Lights move with their entity; a second light adds its hole.
    CHECK(Call(e, "component.set", R"J({"id":"Lamp","type":"Transform","values":{"position":[3.5,0,0]}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Dim","components":{"Transform":{"position":[-3,-2,0]},"Light2D":{"radius":1.5,"strength":0.5}}})J");
    e.RenderGameView(shot);
    CHECK(at(0, 0) == 0x000000 && at(3.5f, 0) == 0xFFFFFF);
    const uint32_t dim = at(-3, -2) & 0xFF;
    CHECK(dim > 110 && dim < 145);        // strength 0.5 removes half the darkness
    RenderView view;
    MakeSceneView(e.GetScene(), 160.0f / 90, view);
    if (e.EnableGpu(nullptr, &error)) {
        RenderTarget gpu;
        gpu.Resize(160, 90);
        e.Gpu()->Render(e.GetScene(), view, gpu);
        double difference = 0;
        for (size_t i = 0; i < shot.color.size(); ++i) for (int shift : {0, 8, 16})
            difference += std::abs(static_cast<int>((shot.color[i] >> shift) & 255) - static_cast<int>((gpu.color[i] >> shift) & 255));
        const double mean = difference / static_cast<double>(shot.color.size() * 3);
        std::printf("  Darkness2D software/GPU mean difference %.4f\n", mean);
        CHECK(mean < 3);
    } else std::printf("  SKIP Darkness2D GPU comparison (%s)\n", error.c_str());
    // Off switches and non-orthographic cameras draw no overlay.
    CHECK(Call(e, "component.set", R"J({"id":"Camera","type":"Darkness2D","values":{"enabled":false}})J")["ok"].asBool());
    e.RenderGameView(shot);
    CHECK(at(0, 0) == 0xFFFFFF);
}

TEST(GamePauseFreezesWorld) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("game_pause"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "script.write", R"J({"path":"scripts/probe.lua","source":"local P = {}\nfunction P:onStart() self.updates, self.elapsed, self.fired = 0, 0, 0 timer.every(0.1, function() self.fired = self.fired + 1 end) end\nfunction P:onUpdate(dt) self.updates = self.updates + 1 self.elapsed = self.elapsed + dt if input.pressed('P') then game.pause(not game.paused()) end end\nreturn P\n"})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Probe","components":{"Script":{"path":"scripts/probe.lua"}}})J");
    Call(e, "entity.create", R"J({"name":"Ball","components":{"Transform":{"position":[0,10,0]},"RigidBody2D":{},
      "Collider2D":{"shape":"circle","radius":0.5}}})J");
    Call(e, "entity.create", R"J({"name":"Dust","components":{"ParticleEmitter":{"rate":60,"lifetime":5}}})J");
    auto eval = [&](const char* code) {
        Json args = Json::MakeObject();
        args["code"] = code;
        args["entity"] = "Probe";
        return e.Call("script.eval", args)["result"]["value"].asNumber();
    };
    auto ballY = [&] { return e.GetScene().Get<Transform>(e.GetScene().FindByName("Ball"))->position.y; };
    CHECK(Call(e, "sim.step", R"J({"frames":30})J")["ok"].asBool());
    const float y = ballY();
    const double updates = eval("return self.updates"), fired = eval("return self.fired"), now = e.SimTime();
    const size_t dust = e.GetScene().Get<ParticleEmitter>(e.GetScene().FindByName("Dust"))->particles.size();
    CHECK(y < 10 && fired >= 3 && dust > 20);
    CHECK(Call(e, "game.pause", R"J({"paused":true})J")["result"]["paused"].asBool());
    CHECK(Call(e, "sim.state", "{}")["result"]["gamePaused"].asBool());
    CHECK(Call(e, "sim.step", R"J({"frames":30})J")["ok"].asBool());
    CHECK(ballY() == y);                                   // physics waits
    CHECK(eval("return self.fired") == fired);             // timers wait
    CHECK(e.SimTime() == now);
    CHECK(e.GetScene().Get<ParticleEmitter>(e.GetScene().FindByName("Dust"))->particles.size() == dust);
    CHECK(eval("return self.updates") == updates + 30);    // scripts keep running, with a zero step
    CHECK(std::fabs(eval("return self.elapsed") - 0.5) < 1e-3);
    // Scripts read input while paused and can resume themselves.
    CHECK(Call(e, "input.key", R"J({"key":"P","down":true})J")["ok"].asBool());
    CHECK(Call(e, "sim.step", R"J({"frames":1})J")["ok"].asBool());
    CHECK(Call(e, "input.key", R"J({"key":"P","down":false})J")["ok"].asBool());
    CHECK(!Call(e, "game.pause", "{}")["result"]["paused"].asBool());
    CHECK(Call(e, "sim.step", R"J({"frames":10})J")["ok"].asBool());
    CHECK(ballY() < y);
    // The pause does not outlive the play session.
    Call(e, "game.pause", R"J({"paused":true})J");
    Call(e, "sim.stop", "{}");
    CHECK(!Call(e, "game.pause", "{}")["result"]["paused"].asBool());
}

TEST(TimeDateOverride) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("time_date"), &error));
    Json today = Call(e, "time.date", "{}")["result"];
    CHECK(today["date"].asString().size() == 10 && !today["overridden"].asBool());
    CHECK(Call(e, "time.date", R"J({"set":"2031-02-28"})J")["result"]["overridden"].asBool());
    CHECK(Call(e, "script.eval", R"J({"code":"return time.date()"})J")["result"]["value"].asString() == "2031-02-28");
    CHECK(!Call(e, "time.date", R"J({"set":"tomorrow"})J")["ok"].asBool());
    CHECK(Call(e, "time.date", "{}")["result"]["date"].asString() == "2031-02-28");
    CHECK(Call(e, "time.date", R"J({"set":""})J")["result"]["date"].asString() == today["date"].asString());
}

TEST(UIPanelBlockInputAndChildLookup) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("ui_block"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Under","components":{"UIButton":{"text":"Under","anchor":"center","x":0,"y":0,"width":200,"height":80,"order":1}}})J");
    Call(e, "entity.create", R"J({"name":"Side","components":{"UIButton":{"text":"Side","anchor":"top-left","x":10,"y":10,"width":120,"height":60,"order":1}}})J");
    Call(e, "entity.create", R"J({"name":"Modal","components":{"UIPanel":{"anchor":"center","x":0,"y":0,"width":400,"height":300,"order":5,"visible":false,"blockInput":true}}})J");
    Call(e, "entity.create", R"J({"name":"Ok","parent":"Modal","components":{"UIButton":{"text":"Ok","anchor":"center","x":0,"y":0,"width":100,"height":40}}})J");
    Scene& scene = e.GetScene();
    const EntityId under = scene.FindByName("Under"), side = scene.FindByName("Side"), ok = scene.FindByName("Ok");
    CHECK(HitTestButton(scene, 640, 360, 1280, 720) == under);
    CHECK(Call(e, "component.set", R"J({"id":"Modal","type":"UIPanel","values":{"visible":true}})J")["ok"].asBool());
    CHECK(HitTestButton(scene, 640, 360, 1280, 720) == ok);              // children of the modal are on top of it
    CHECK(HitTestButton(scene, 640 + 90, 360 + 30, 1280, 720) == kNullEntity);  // the button under the modal is out of reach
    CHECK(HitTestButton(scene, 60, 40, 1280, 720) == side);              // outside the panel nothing is blocked
    CHECK(Call(e, "component.set", R"J({"id":"Modal","type":"UIPanel","values":{"blockInput":false}})J")["ok"].asBool());
    CHECK(HitTestButton(scene, 640 + 90, 360 + 30, 1280, 720) == under);
    // scene.children / scene.child
    Json children = Call(e, "script.eval", R"J({"code":"return scene.children(scene.find('Modal'))"})J")["result"]["value"];
    CHECK(children.size() == 1 && children[0].asInt() == static_cast<int>(ok));
    CHECK(Call(e, "script.eval", R"J({"code":"return scene.child(scene.find('Modal'), 'Ok')"})J")["result"]["value"].asInt() == static_cast<int>(ok));
    CHECK(Call(e, "script.eval", R"J({"code":"return scene.child(scene.find('Modal'), 'Nope') == nil"})J")["result"]["value"].asBool());
}

TEST(UIScrollMotionAndTweens) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("ui_motion"), &error));
    Call(e, "scene.new", R"J({"empty":true})J");
    // A 200 px tall scroll view with ten 50 px rows (10 px apart, 10 px padding): 610 px of content.
    Call(e, "entity.create", R"J({"name":"List","components":{"UIPanel":{"anchor":"top-left","x":100,"y":100,"width":300,"height":200},
      "UILayout":{"direction":"vertical","spacing":10,"padding":10,"crossAlign":"stretch"},"UIScroll":{"wheelStep":50}}})J");
    for (int i = 1; i <= 10; ++i) {
        Json args = Json::parse(R"J({"parent":"List","components":{"UIButton":{"text":"Row","height":50}}})J");
        args["name"] = "Row" + std::to_string(i);
        CHECK(e.Call("entity.create", args)["ok"].asBool());
    }
    Scene& scene = e.GetScene();
    auto rect = [&](const char* name) {
        const EntityId id = scene.FindByName(name);
        for (const UIRect& r : LayoutUI(scene, 1280, 720)) if (r.entity == id) return r;
        return UIRect{};
    };
    auto step = [&](int frames) {
        Json args = Json::MakeObject();
        args["frames"] = frames;
        CHECK(e.Call("sim.step", args)["ok"].asBool());
    };
    const EntityId list = scene.FindByName("List");
    CHECK(rect("List").scroll && std::fabs(rect("List").scrollMax - 410) < 0.5f);
    CHECK(std::fabs(rect("Row1").y - 110) < 0.5f);
    CHECK(HitTestButton(scene, 250, 130, 1280, 720) == scene.FindByName("Row1"));
    CHECK(HitTestButton(scene, 250, 340, 1280, 720) == kNullEntity);  // a row below the view is clipped away
    step(1);
    // Wheel over the list: two notches toward the user scroll 100 px; the engine keeps scroll inside the content.
    CHECK(Call(e, "input.mouse", R"J({"x":250,"y":200,"width":1280,"height":720,"wheel":-2})J")["ok"].asBool());
    step(1);
    CHECK(std::fabs(scene.Get<UIScroll>(list)->scroll - 100) < 1e-3f);
    CHECK(std::fabs(rect("Row1").y - 10) < 0.5f);
    CHECK(HitTestButton(scene, 250, 150, 1280, 720) == scene.FindByName("Row3"));
    CHECK(HitTestButton(scene, 250, 90, 1280, 720) == kNullEntity);  // Row2 continues above the view, where it is clipped
    CHECK(Call(e, "input.mouse", R"J({"wheel":-50})J")["ok"].asBool());
    step(1);
    CHECK(std::fabs(scene.Get<UIScroll>(list)->scroll - 410) < 1e-3f);
    CHECK(Call(e, "input.mouse", R"J({"x":900,"y":600,"width":1280,"height":720,"wheel":5})J")["ok"].asBool());
    step(1);
    CHECK(std::fabs(scene.Get<UIScroll>(list)->scroll - 410) < 1e-3f);  // the pointer is elsewhere
    CHECK(Call(e, "script.eval", R"J({"code":"return input.wheel()"})J")["result"]["value"].asNumber() == 0);  // cleared after the step
    // Dragging the gap between two rows moves the content with the pointer.
    Call(e, "input.mouse", R"J({"x":250,"y":295,"width":1280,"height":720,"button":"MouseLeft","down":true})J");
    step(1);
    Call(e, "input.mouse", R"J({"x":250,"y":355,"width":1280,"height":720})J");
    step(1);
    Call(e, "input.mouse", R"J({"button":"MouseLeft","down":false})J");
    step(1);
    CHECK(std::fabs(scene.Get<UIScroll>(list)->scroll - 350) < 1e-3f);

    // The bar: grabbing the thumb moves the content in proportion; clicking the track jumps there; neither clicks a row.
    CHECK(Call(e, "script.write", R"J({"path":"scripts/count.lua","source":"local C = {}\nfunction C:onClick() CLICKS = (CLICKS or 0) + 1 end\nreturn C\n"})J")["ok"].asBool());
    for (int i = 1; i <= 10; ++i) {
        Json args = Json::parse(R"J({"type":"Script","values":{"path":"scripts/count.lua"}})J");
        args["id"] = "Row" + std::to_string(i);
        CHECK(e.Call("component.add", args)["ok"].asBool());
    }
    scene.Get<UIScroll>(list)->scroll = 0;
    step(1);
    const UIThumb thumb = ScrollThumb(200, 610, 0, 1);
    const float barX = 100 + 300 - 6, top = 100 + thumb.at + thumb.length * 0.5f;
    auto pointer = [&](float x, float y, const char* extra) {
        Json args = Json::parse(std::string("{") + extra + "}");
        args["x"] = x; args["y"] = y; args["width"] = 1280; args["height"] = 720;
        CHECK(e.Call("input.mouse", args)["ok"].asBool());
        step(1);
    };
    pointer(barX, top, R"J("button":"MouseLeft","down":true)J");
    CHECK(scene.Get<UIScroll>(list)->scroll == 0);  // grabbing the thumb where it is does not move it
    pointer(barX, top + thumb.travel * 0.5f, "");
    CHECK(std::fabs(scene.Get<UIScroll>(list)->scroll - 205) < 1.0f);  // half the travel = half the content
    pointer(barX, top + 500, "");
    CHECK(std::fabs(scene.Get<UIScroll>(list)->scroll - 410) < 1e-3f);  // clamped at the end, also outside the view
    pointer(barX, top, R"J("button":"MouseLeft","down":false)J");
    pointer(barX, 100 + 100, R"J("button":"MouseLeft","down":true)J");   // the track: the thumb centres on the pointer
    CHECK(std::fabs(scene.Get<UIScroll>(list)->scroll - 205) < 1.0f);
    pointer(barX, 100 + 100, R"J("button":"MouseLeft","down":false)J");
    CHECK(Call(e, "script.eval", R"J({"code":"return CLICKS or 0"})J")["result"]["value"].asInt() == 0);
    pointer(250, 225, R"J("button":"MouseLeft","down":true)J");
    pointer(250, 225, R"J("button":"MouseLeft","down":false)J");
    CHECK(Call(e, "script.eval", R"J({"code":"return CLICKS or 0"})J")["result"]["value"].asInt() == 1);  // rows still click

    // UIMotion: plays when the element becomes visible, on real frames (also while the game is paused).
    Call(e, "entity.create", R"J({"name":"Dialog","components":{"UIPanel":{"anchor":"center","x":0,"y":0,"width":200,"height":100,"opacity":1,"visible":false},
      "UIMotion":{"enter":"slide-up","duration":0.5,"distance":60}}})J");
    Call(e, "entity.create", R"J({"name":"Label","parent":"Dialog","components":{"UIText":{"text":"Hi","anchor":"center","x":0,"y":0}}})J");
    step(2);
    Call(e, "game.pause", R"J({"paused":true})J");
    Call(e, "component.set", R"J({"id":"Dialog","type":"UIPanel","values":{"visible":true}})J");
    step(1);
    UIRect early = rect("Dialog"), label = rect("Label");
    CHECK(early.y > 310 + 40 && early.opacity < 0.2f && label.opacity < 0.2f);  // starts lower and transparent, children with it
    step(12);
    UIRect mid = rect("Dialog");
    CHECK(mid.y < early.y && mid.y > 310 && mid.opacity > early.opacity);
    step(30);
    CHECK(std::fabs(rect("Dialog").y - 310) < 0.01f && rect("Dialog").opacity == 1.0f);
    Call(e, "game.pause", R"J({"paused":false})J");

    // An element shown by a click handler (callbacks run late in the frame) is already at the start of its
    // entrance when that frame is drawn: it never appears at its final look for one frame first.
    CHECK(Call(e, "script.write", R"J({"path":"scripts/open.lua","source":"local O = {}\nfunction O:onClick() scene.set(scene.find('Popup'), 'UIPanel', {visible = true}) end\nreturn O\n"})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Popup","components":{"UIPanel":{"anchor":"bottom","x":0,"y":-20,"width":200,"height":60,"opacity":1,"visible":false},
      "UIMotion":{"enter":"fade","duration":0.3}}})J");
    Call(e, "entity.create", R"J({"name":"Opener","components":{"UIButton":{"text":"Open","anchor":"top-right","x":-20,"y":20,"width":120,"height":50},
      "Script":{"path":"scripts/open.lua"}}})J");
    step(2);
    CHECK(Call(e, "input.click", R"J({"x":1200,"y":45,"width":1280,"height":720})J")["result"]["button"].asInt() == static_cast<int>(scene.FindByName("Opener")));
    step(1);
    CHECK(scene.Get<UIPanel>(scene.FindByName("Popup"))->visible);
    CHECK(rect("Popup").opacity < 0.05f);
    step(30);
    CHECK(rect("Popup").opacity == 1.0f);

    // Buttons ease toward hoverScale / pressedScale.
    Call(e, "entity.create", R"J({"name":"Grow","components":{"UIButton":{"text":"Go","anchor":"top-left","x":600,"y":500,"width":100,"height":40,"hoverScale":1.2}}})J");
    step(1);
    CHECK(std::fabs(rect("Grow").w - 100) < 0.01f);
    Call(e, "input.mouse", R"J({"x":650,"y":520,"width":1280,"height":720})J");
    step(30);
    CHECK(std::fabs(rect("Grow").w - 120) < 0.5f && std::fabs(rect("Grow").x - 590) < 0.5f);

    // Lua tweens: unscaled by default, eased, with a completion callback.
    CHECK(Call(e, "script.eval", R"J({"code":"DONE = 0 tween.to(scene.find('Dialog'), 'UIPanel', {x = 100, color = {1, 0, 0}}, 0.5, {ease = 'linear', onDone = function() DONE = DONE + 1 end})"})J")["ok"].asBool());
    step(15);
    CHECK(std::fabs(scene.Get<UIPanel>(scene.FindByName("Dialog"))->x - 50) < 0.5f);
    Call(e, "game.pause", R"J({"paused":true})J");
    step(20);
    const UIPanel* panel = scene.Get<UIPanel>(scene.FindByName("Dialog"));
    CHECK(std::fabs(panel->x - 100) < 1e-3f && std::fabs(panel->color.r - 1) < 1e-3f && panel->color.g == 0);
    CHECK(Call(e, "script.eval", R"J({"code":"return DONE"})J")["result"]["value"].asInt() == 1);
    // scaled tweens wait with the world; tween.stop drops them
    CHECK(Call(e, "script.eval", R"J({"code":"tween.to(scene.find('Dialog'), 'UIPanel', {y = 80}, 0.25, {scaled = true})"})J")["ok"].asBool());
    step(30);
    CHECK(scene.Get<UIPanel>(scene.FindByName("Dialog"))->y == 0);
    Call(e, "game.pause", R"J({"paused":false})J");
    step(5);
    CHECK(scene.Get<UIPanel>(scene.FindByName("Dialog"))->y > 0);
    Call(e, "script.eval", R"J({"code":"tween.stop(scene.find('Dialog'))"})J");
    const float y = scene.Get<UIPanel>(scene.FindByName("Dialog"))->y;
    step(10);
    CHECK(scene.Get<UIPanel>(scene.FindByName("Dialog"))->y == y);
    CHECK(e.Scripts().Errors().empty());
}

TEST(WickboundSample) {
    // The survivor roguelite built from engine pieces: prefab entities with their own scripts, Box2D
    // colliders and sensors, Light2D darkness, UI scenes. Plays through the menu into a run and back.
    Engine e;
    std::string error;
    const std::string project = TestSourceDir() + "/samples/Wickbound";
    CHECK(e.Open(project, &error));
    for (const std::string& file : GameFiles(project)) {
        if (file.size() < 4 || file.substr(file.size() - 4) != ".lua") continue;
        Json args = Json::MakeObject();
        args["path"] = file;
        Json result = e.Call("script.check", args)["result"];
        CHECK(result["errors"].asInt() == 0 && result["warnings"].asInt() == 0);
    }
    CHECK(Call(e, "time.date", R"J({"set":"2030-05-05"})J")["ok"].asBool());
    auto step = [&](int frames) {
        Json args = Json::MakeObject();
        args["frames"] = frames;
        CHECK(e.Call("sim.step", args)["ok"].asBool());
    };
    auto eval = [&](const std::string& code) {
        Json args = Json::MakeObject();
        args["code"] = code;
        return e.Call("script.eval", args)["result"]["value"];
    };
    auto click = [&](const char* name) {  // a real pointer click on the named button
        Json layout = Call(e, "ui.layout", R"J({"width":1280,"height":720})J")["result"];
        const EntityId id = e.GetScene().FindByName(name);
        bool found = false;
        for (const Json& element : (layout.has("elements") ? layout["elements"] : layout).items()) {
            if (element["id"].asInt() != static_cast<int>(id)) continue;
            Json args = Json::MakeObject();
            args["x"] = element["center"][0]; args["y"] = element["center"][1]; args["width"] = 1280; args["height"] = 720;
            CHECK(e.Call("input.click", args)["result"]["button"].asInt() == static_cast<int>(id));
            found = true;
        }
        CHECK(found);
        step(3);
    };
    auto action = [&](const char* target, const char* name, const char* arg) {
        eval(std::string("scene.send(scene.find('") + target + "'), 'onAction', '" + name + "', " + arg + ")");
        step(2);
    };
    const char* world = "local W = require('scripts.lib.world') ";
    step(3);
    CHECK(e.RuntimeScene() == "scenes/menu.scene.json");
    RenderTarget title, run;
    for (RenderTarget* target : {&title, &run}) target->Resize(320, 180);
    e.RenderGameView(title);
    // Daily gift: 50 Glims today, nothing more until the date changes, 75 on the next day (streak).
    CHECK(eval("return require('scripts.lib.meta').dailyAvailable()").asBool());
    action("Menu", "dailyOpen", "nil");
    action("Menu", "dailyClaim", "nil");
    CHECK(eval("return require('scripts.lib.meta').data.glims").asInt() == 50);
    CHECK(!eval("return require('scripts.lib.meta').dailyAvailable()").asBool());
    Call(e, "time.date", R"J({"set":"2030-05-06"})J");
    CHECK(eval("return require('scripts.lib.meta').dailyAmount()").asInt() == 75);

    action("Menu", "nav", "'play'");
    click("Start");
    step(3);
    CHECK(e.RuntimeScene() == "scenes/run.scene.json");
    CHECK(eval(std::string(world) + "return W.game.phase").asString() == "playing");
    // The keeper is a CharacterBody2D with child entities: body sprite, lantern light, sensors and its first weapon.
    Scene& scene = e.GetScene();
    const EntityId player = scene.FindByName("Player");
    CHECK(scene.Get<CharacterBody2D>(player) != nullptr);
    CHECK(scene.Children(player).size() >= 7);
    CHECK(scene.Get<Light2D>(scene.FindByName("Lantern")) != nullptr && scene.Get<Darkness2D>(scene.FindByName("Camera")) != nullptr);
    CHECK(scene.Record(scene.FindByName("Weapon ember_bolt"))->parent == player);
    const EntityId playerBody = static_cast<EntityId>(eval(std::string(world) + "return W.player.body").asInt());
    const Sprite* keeperSprite = scene.Get<Sprite>(playerBody);
    CHECK(keeperSprite && keeperSprite->columns >= 4 && keeperSprite->rows >= 2);
    CHECK(keeperSprite->texture == "assets/sprites/animated/keeper_ada.png");
    CHECK(scene.Get<SpriteAnimation>(playerBody)->clip == "idle");
    CHECK(Call(e, "input.key", R"J({"key":"D","down":true})J")["ok"].asBool());
    step(12);
    CHECK(scene.Get<SpriteAnimation>(playerBody)->clip == "move");
    bool inMoveClip = false;
    for (const Json& frame : scene.Get<SpriteAnimation>(playerBody)->clips["move"]["frames"].items())
        if (frame.asInt() == scene.Get<Sprite>(playerBody)->frame) inMoveClip = true;
    CHECK(inMoveClip);
    step(228);
    CHECK(Call(e, "input.key", R"J({"key":"D","down":false})J")["ok"].asBool());
    CHECK(e.Scripts().Errors().empty());
    CHECK(scene.Get<Transform>(player)->position.x > 14);  // walked right at about 4.1 units/s
    CHECK(eval(std::string(world) + "return #W.enemyList").asInt() > 0 && scene.Pool<RigidBody2D>().size() > 3);
    e.RenderGameView(run);
    CHECK(run.Hash() != title.Hash());
    step(2);
    CHECK(scene.Get<SpriteAnimation>(playerBody)->clip == "idle");
    eval(std::string(world) + "for _, e in ipairs(W.enemyList) do if scene.has(e.body, 'SpriteAnimation') then e.stun = 0.5 break end end");
    step(2);
    CHECK(eval(std::string(world) + "for _, e in ipairs(W.enemyList) do if scene.has(e.body, 'SpriteAnimation') then return scene.get(e.body, 'SpriteAnimation').clip end end").asString() == "idle");
    for (const RenderItem& item : GatherRenderItems(scene, &e.Assets())) CHECK(!item.error);
    // An enemy placed on the keeper is seen by the Hurtbox sensor and hurts; the bolt weapon's trigger shots kill it.
    eval(std::string(world) + "W.spawn('enemy_shade', W.player.x + 0.2, W.player.y, {kind = 'shade'})");
    step(30);
    CHECK(eval(std::string(world) + "return W.player.hp").asNumber() < 100);
    step(180);
    CHECK(eval(std::string(world) + "return W.game.kills").asInt() >= 1);
    // A level-up pauses the world (game.pause) until a card is picked with key 1.
    eval(std::string(world) + "W.game:gainXp(40)");
    step(2);
    CHECK(eval(std::string(world) + "return W.game.phase").asString() == "levelup");
    CHECK(Call(e, "sim.state", "{}")["result"]["gamePaused"].asBool());
    const double frozen = eval(std::string(world) + "return W.game.time").asNumber();
    const int frozenFrame = scene.Get<Sprite>(playerBody)->frame;
    step(30);
    CHECK(eval(std::string(world) + "return W.game.time").asNumber() == frozen);
    CHECK(scene.Get<Sprite>(playerBody)->frame == frozenFrame);
    for (int guard = 0; guard < 6 && eval(std::string(world) + "return W.game.phase").asString() == "levelup"; ++guard) {
        CHECK(Call(e, "input.key", R"J({"key":"1","down":true})J")["ok"].asBool());
        step(2);
        CHECK(Call(e, "input.key", R"J({"key":"1","down":false})J")["ok"].asBool());
        step(2);
    }
    CHECK(eval(std::string(world) + "return W.game.phase").asString() == "playing");
    CHECK(!Call(e, "sim.state", "{}")["result"]["gamePaused"].asBool());
    // Standing in a brazier's trigger ring for three seconds lights it: a permanent Light2D.
    eval(std::string(world) + "local b = W.braziers[1] scene.set(W.player.id, 'Transform', {position = {x = b.x, y = b.y}})");
    step(200);
    CHECK(eval(std::string(world) + "return W.game.braziersLit").asInt() == 1);
    CHECK(eval(std::string(world) + "return scene.get(W.braziers[1].id, 'Light2D').strength").asNumber() == 1);
    CHECK(!eval(std::string(world) + "return scene.has(scene.child(W.braziers[1].id, 'Body'), 'SpriteAnimation')").asBool());
    CHECK(eval(std::string(world) + "return scene.get(scene.child(W.braziers[1].id, 'Fire'), 'Sprite').visible").asBool());
    CHECK(e.Scripts().Errors().empty());
    // Giving up returns to the menu scene with the result, and the run's Glims and achievement are saved.
    eval(std::string(world) + "W.game:giveUp()");
    step(3);
    CHECK(e.RuntimeScene() == "scenes/menu.scene.json");
    CHECK(e.GetScene().Get<UIPanel>(e.GetScene().FindByName("Screen:result"))->visible);
    CHECK(eval("return require('scripts.lib.meta').data.stats.runs").asInt() == 1);
    CHECK(eval("return require('scripts.lib.meta').data.achievements.first_light").asBool());
    CHECK(eval("return require('scripts.lib.meta').data.glims").asInt() > 100);
    CHECK(e.Scripts().Errors().empty());
}

TEST(ShaderGraphValidationAndReference) {
    Engine e;
    std::string error;
    CHECK(e.Open(TempProject("shader_graph"), &error));
    const Json graphJson = Json::parse(R"J({"format":"ownengine.shader","uniforms":{"frequency":4},
        "nodes":[{"op":"uv"},{"op":"uniform","name":"frequency"},{"op":"multiply","args":[0,1]},
        {"op":"fract","args":[2]},{"op":"constant","value":[0.5,0.5,0.5,0.5]},
        {"op":"step","args":[4,3]},{"op":"constant","value":[1,0.25,0.125,1]},
        {"op":"multiply","args":[5,6]}],"color":7,"emissive":6})J");
    Json create = Json::MakeObject();
    create["path"] = "materials/stripes.shader.json";
    create["graph"] = graphJson;
    CHECK(e.Call("shader.create", create)["ok"].asBool());
    CHECK(Call(e, "shader.check", R"J({"path":"materials/stripes.shader.json"})J")["result"]["nodes"].asInt() == 8);
    CHECK(!e.Call("shader.create", create)["ok"].asBool());
    CHECK(Call(e, "asset.list", R"J({"kind":"shader"})J")["result"].size() == 1);
    ShaderGraph graph;
    CHECK(CompileShaderGraph(graphJson, graph, &error));
    ShaderInputs inputs;
    inputs.uv = Vec4(0.2f,0.4f,0,1);
    auto surface = EvaluateShaderGraph(graph, inputs, graph.defaults);
    CHECK(surface.color.x == 1 && surface.color.y == 0.25f && surface.color.z == 0);
    CHECK(surface.emissive.x == 1 && surface.emissive.y == 0.25f);
    std::array<Vec4, ShaderGraph::kMaxUniforms> uniforms;
    CHECK(ShaderUniforms(graph, Json::parse(R"J({"frequency":2})J"), uniforms, &error));
    surface = EvaluateShaderGraph(graph, inputs, uniforms);
    CHECK(surface.color.x == 0 && surface.color.y == 0.25f);
    CHECK(!ShaderUniforms(graph, Json::parse(R"J({"typo":2})J"), uniforms, &error));
    CHECK(error.find("typo") != std::string::npos);
    const std::vector<std::string> invalid = {
        R"J({"nodes":[{"op":"add","args":[0,0]}],"color":0})J",
        R"J({"nodes":[{"op":"constant","value":[1,2,3]}],"color":0})J",
        R"J({"nodes":[{"op":"uniform","name":"missing"}],"color":0})J",
        R"J({"nodes":[{"op":"constant","value":1}],"color":0.5})J",
        R"J({"nodes":[{"op":"constant","value":1,"typo":true}],"color":0})J",
        R"J({"nodes":[{"op":"constant","value":1},{"op":"swizzle","args":[0],"value":[4,0,0,0]}],"color":1})J"
    };
    for (const std::string& text : invalid) {
        create["graph"] = Json::parse(text);
        create["overwrite"] = true;
        CHECK(e.Call("shader.create", create)["error"]["code"].asString() == "invalid_shader");
        CHECK(Call(e, "shader.check", R"J({"path":"materials/stripes.shader.json"})J")["result"]["nodes"].asInt() == 8);
    }
    create["path"] = "../outside.shader.json";
    create["graph"] = graphJson;
    CHECK(!e.Call("shader.create", create)["ok"].asBool());
    CHECK(CompileShaderGraph(Json::parse(R"J({"nodes":[{"op":"constant","value":65504},{"op":"constant","value":0},
        {"op":"divide","args":[0,1]},{"op":"multiply","args":[0,0]}],"color":2,"emissive":3})J"), graph, &error));
    surface = EvaluateShaderGraph(graph, inputs, graph.defaults);
    CHECK(surface.color.x == 0 && surface.emissive.x == 65504);
    CHECK(CompileShaderGraph(Json::parse(R"J({"nodes":[{"op":"uv"},{"op":"texture","args":[0]},
        {"op":"swizzle","args":[1],"value":[2,1,0,3]}],"color":2})J"), graph, &error));
    inputs.texture = [](float u, float v) { return Vec4(u,v,0.75f,0.5f); };
    surface = EvaluateShaderGraph(graph, inputs, graph.defaults);
    CHECK(surface.color.x == 0.75f && surface.color.y == 0.4f && surface.color.z == 0.2f && surface.color.w == 0.5f);
    CHECK(CompileShaderGraph(Json::parse(R"J({"nodes":[{"op":"time"},{"op":"sin"}],"color":1})J"), graph, &error) == false);
    CHECK(CompileShaderGraph(Json::parse(R"J({"nodes":[{"op":"time"},{"op":"sin","args":[0]}],"color":1})J"), graph, &error));
    inputs.time = 0.5f;
    surface = EvaluateShaderGraph(graph, inputs, graph.defaults);
    CHECK(std::fabs(surface.color.x - 0.47942554f) < 1e-6f);
    Json oversized = Json::MakeObject();
    oversized["nodes"] = Json::MakeArray();
    for (int i = 0; i < ShaderGraph::kMaxNodes + 1; ++i) oversized["nodes"].push(Json::parse(R"J({"op":"constant","value":1})J"));
    oversized["color"] = 0;
    CHECK(!CompileShaderGraph(oversized, graph, &error));
    CHECK(error.find("1..48") != std::string::npos);
}

TEST(ShaderGraphVertexOffset) {
    // Graph outputs `offset` (moves vertices in world space) and `normal` (lighting normal), in both renderers.
    ShaderGraph graph;
    std::string err;
    CHECK(CompileShaderGraph(Json::parse(R"J({"nodes":[{"op":"position"},{"op":"constant","value":2},
        {"op":"multiply","args":[0,1]}],"color":1,"offset":2,"normal":0})J"), graph, &err));
    CHECK(graph.offset == 2 && graph.normal == 0);
    ShaderInputs inputs;
    inputs.position = Vec4(1, 2, 3, 1);
    const Vec3 moved = ShaderVertexOffset(graph, inputs, graph.defaults);
    CHECK(moved.x == 2 && moved.y == 4 && moved.z == 6);
    CHECK(EvaluateShaderGraph(graph, inputs, graph.defaults).normal.z == 3);
    // The vertex stage has no texture.
    CHECK(!CompileShaderGraph(Json::parse(R"J({"nodes":[{"op":"uv"},{"op":"texture","args":[0]}],"color":1,"offset":1})J"), graph, &err));
    CHECK(err.find("texture") != std::string::npos);
    CHECK(!CompileShaderGraph(Json::parse(R"J({"nodes":[{"op":"uv"}],"color":0,"offset":3})J"), graph, &err));
    CHECK(GetBuiltinMesh("plane64")->positions.size() == 65 * 65 && GetBuiltinMesh("plane64")->TriangleCount() == 64 * 64 * 2);

    Engine e;
    CHECK(e.Open(TempProject("shader_offset"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    // The sheet's graph moves it from the origin to x 1..5 at height 1.
    Json created = Call(e, "shader.create", R"J({"path":"moved.shader.json","graph":{"uniforms":{"shift":[3,1,0,0]},
        "nodes":[{"op":"constant","value":[1,0,0,1]},{"op":"uniform","name":"shift"}],"color":0,"offset":1}})J");
    CHECK(created["ok"].asBool() && created["result"]["offset"].asInt() == 1 && created["result"]["normal"].asInt() == -1);
    CHECK(Call(e, "shader.create", R"J({"path":"sideways.shader.json","graph":{"uniforms":{"shift":[3,1,0,0]},
        "nodes":[{"op":"constant","value":[1,0,0,1]},{"op":"uniform","name":"shift"},{"op":"constant","value":[1,0,0,0]}],
        "color":0,"offset":1,"normal":2}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"moved.mat.json","values":{"shader":"moved.shader.json","roughness":1,"doubleSided":true}})J")["ok"].asBool());
    CHECK(Call(e, "material.create", R"J({"path":"sideways.mat.json","values":{"shader":"sideways.shader.json","roughness":1,"doubleSided":true}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[1.5,5,9],"rotation":[-29.05,0,0]},"Camera":{"clearColor":[0,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Sun","components":{"Transform":{"rotation":[-70,0,0]},"DirectionalLight":{"ambient":[0.1,0.1,0.1]}}})J");
    Call(e, "entity.create", R"J({"name":"Ground","components":{"Transform":{"position":[0,-1,0],"scale":[20,1,20]},"MeshRenderer":{"mesh":"plane","color":[1,1,1]}}})J");
    Call(e, "entity.create", R"J({"name":"Sheet","components":{"Transform":{"scale":[4,1,4]},"MeshRenderer":{"mesh":"plane64","material":"moved.mat.json"}}})J");
    const int w = 160, h = 120;
    RenderView view;
    MakeSceneView(e.GetScene(), static_cast<float>(w) / h, view);
    auto pixel = [&](const RenderTarget& t, Vec3 world) {
        const Vec4 clip = view.proj * view.view * Vec4(world, 1);
        const int x = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * w), y = static_cast<int>((0.5f - clip.y / clip.w * 0.5f) * h);
        return t.color[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
    };
    auto red = [](uint32_t c) { return static_cast<int>(c & 255); };
    auto green = [](uint32_t c) { return static_cast<int>((c >> 8) & 255); };
    const EntityId sheet = e.GetScene().FindByName("Sheet");
    auto check = [&](IRenderer& renderer, const char* name) {
        RenderTarget plain, sideways, outlined;
        for (RenderTarget* target : {&plain, &sideways, &outlined}) target->Resize(w, h);
        CHECK(Call(e, "component.set", R"J({"id":"Sheet","type":"MeshRenderer","values":{"material":"moved.mat.json"}})J")["ok"].asBool());
        view.highlight = kNullEntity;
        renderer.Render(e.GetScene(), view, plain);
        // The sheet is drawn where the graph moved it, not at the origin (white ground shows there).
        CHECK(red(pixel(plain, Vec3(3, 1, 0))) > 150 && green(pixel(plain, Vec3(3, 1, 0))) < 40);
        CHECK(green(pixel(plain, Vec3(-1, 0, 0))) > 150);
        // Its shadow moved with it: the ground below the new place is dark, below the old one lit.
        const int shaded = green(pixel(plain, Vec3(4.5f, -1, 0.8f))), lit = green(pixel(plain, Vec3(-0.5f, -1, 0.8f)));
        std::printf("  vertex offset (%s): shadowed ground %d, lit ground %d\n", name, shaded, lit);
        CHECK(lit > 150 && shaded < lit / 2);
        // The selection outline follows the moved surface.
        view.highlight = sheet;
        renderer.Render(e.GetScene(), view, outlined);
        view.highlight = kNullEntity;
        const Vec4 corner = view.proj * view.view * Vec4(1, 1, 2, 1);
        const int left = static_cast<int>((corner.x / corner.w * 0.5f + 0.5f) * w);
        int changed = 0, changedLeft = 0;
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
            if (plain.color[i] == outlined.color[i]) continue;
            ++changed;
            changedLeft += x < left - 6;
        }
        CHECK(changed > 20 && changedLeft == 0);
        // The `normal` output replaces the lighting normal: perpendicular to the sun leaves only ambient light.
        CHECK(Call(e, "component.set", R"J({"id":"Sheet","type":"MeshRenderer","values":{"material":"sideways.mat.json"}})J")["ok"].asBool());
        renderer.Render(e.GetScene(), view, sideways);
        CHECK(red(pixel(sideways, Vec3(3, 1, 0))) < red(pixel(plain, Vec3(3, 1, 0))) / 2);
        return plain;
    };
    const RenderTarget software = check(e.Renderer(), "software");
    // Depth of field reads the moved surface's depth (GPU: its own depth pass).
    CHECK(Call(e, "component.set", R"J({"id":"Sheet","type":"MeshRenderer","values":{"material":"moved.mat.json"}})J")["ok"].asBool());
    view.postProcess.dofRadius = 6;
    view.postProcess.dofFocus = 9.5f;
    view.postProcess.dofRange = 3;
    view.postProcess.dofFalloff = 4;
    RenderTarget softwareDof, gpuDof;
    softwareDof.Resize(w, h);
    gpuDof.Resize(w, h);
    e.Renderer().Render(e.GetScene(), view, softwareDof);
    view.postProcess.dofRadius = 0;
    auto difference = [](const RenderTarget& a, const RenderTarget& b) {
        double error = 0;
        for (size_t i = 0; i < a.color.size(); ++i) for (int shift : {0, 8, 16})
            error += std::abs(static_cast<int>((a.color[i] >> shift) & 255) - static_cast<int>((b.color[i] >> shift) & 255));
        return error / static_cast<double>(a.color.size() * 3);
    };
    if (e.EnableGpu(nullptr, &err)) {
        const RenderTarget gpu = check(*e.Gpu(), "gpu");
        const double mean = difference(software, gpu);
        CHECK(Call(e, "component.set", R"J({"id":"Sheet","type":"MeshRenderer","values":{"material":"moved.mat.json"}})J")["ok"].asBool());
        view.postProcess.dofRadius = 6;
        e.Gpu()->Render(e.GetScene(), view, gpuDof);
        const double meanDof = difference(softwareDof, gpuDof);
        std::printf("  vertex offset software/GPU mean difference %.4f, with depth of field %.4f\n", mean, meanDof);
        CHECK(mean < 1.5 && meanDof < 2.0);
    } else std::printf("  SKIP vertex offset GPU comparison (%s)\n", err.c_str());
}

TEST(CameraFxaa) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("camera_fxaa"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "material.create", R"J({"path":"white.mat.json","values":{"baseColor":[1,1,1],"unlit":true}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},"Camera":{"projection":"orthographic","clearColor":[0,0,0]},"PostProcess":{}}})J");
    CHECK(Call(e, "entity.create", R"J({"name":"Edge","components":{"Transform":{"rotation":[0,0,25],"scale":[4,1,1]},"MeshRenderer":{"material":"white.mat.json"}}})J")["ok"].asBool());
    Call(e, "entity.create", R"J({"name":"UI","components":{"UIPanel":{"anchor":"top-left","x":0,"y":0,"width":200,"height":200,"color":[1,1,1],"opacity":1}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    RenderTarget off, on, again;
    for (RenderTarget* target : {&off, &on, &again}) target->Resize(128, 72);
    e.Renderer().Render(e.GetScene(), view, off);
    CHECK(Call(e, "component.set", R"J({"id":"Camera","type":"PostProcess","values":{"fxaa":true}})J")["ok"].asBool());
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, on);
    SetMaxRenderThreads(4);
    e.Renderer().Render(e.GetScene(), view, again);
    SetMaxRenderThreads(16);
    CHECK(on.Hash() != off.Hash() && on.Hash() == again.Hash());
    CHECK(on.depth == off.depth && on.ids == off.ids);
    CHECK(on.color[10 * 128 + 10] == off.color[10 * 128 + 10]);
    CHECK(on.color.back() == off.color.back());
    int smoothed = 0;
    for (size_t i = 0; i < on.color.size(); ++i)
        if (on.color[i] != off.color[i] && (on.color[i] & 255) > 0 && (on.color[i] & 255) < 255) ++smoothed;
    CHECK(smoothed > 20);
    Scene restored;
    CHECK(restored.FromJson(e.GetScene().ToJson(), &err));
    CHECK(restored.Get<PostProcess>(restored.FindByName("Camera"))->fxaa);
    Call(e, "history.undo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK(again.Hash() == off.Hash());
    Call(e, "history.redo", "{}");
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    view.highlight = e.GetScene().FindByName("Edge");
    view.postProcess.fxaa = false;
    e.Renderer().Render(e.GetScene(), view, off);
    view.postProcess.fxaa = true;
    e.Renderer().Render(e.GetScene(), view, on);
    int outlinePixels = 0;
    for (size_t i = 0; i < off.color.size(); ++i) if ((off.color[i] & 0xFFFFFF) == 0x1A9EFF) {
        ++outlinePixels;
        CHECK(on.color[i] == off.color[i]);
    }
    CHECK(outlinePixels > 0);
    view.highlight = kNullEntity;
    if (e.EnableGpu(nullptr, &err)) {
        for (int width : {128, 97}) {
            for (RenderTarget* target : {&off, &on, &again}) target->Resize(width, width == 128 ? 72 : 55);
            view.postProcess.fxaa = false;
            e.Gpu()->Render(e.GetScene(), view, off);
            view.postProcess.fxaa = true;
            e.Gpu()->Render(e.GetScene(), view, on);
            CHECK(on.Hash() != off.Hash());
            CHECK(on.color[10 * width + 10] == off.color[10 * width + 10]);
            e.Renderer().Render(e.GetScene(), view, again);
            double error = 0;
            for (size_t i = 0; i < on.color.size(); ++i) for (int shift : {0, 8, 16})
                error += std::abs(static_cast<int>((on.color[i] >> shift) & 255) - static_cast<int>((again.color[i] >> shift) & 255));
            const double mean = error / static_cast<double>(on.color.size() * 3);
            std::printf("  FXAA width %d software/GPU mean difference %.4f\n", width, mean);
            CHECK(mean < 3);
            view.postProcess.fxaa = false;
            e.Gpu()->Render(e.GetScene(), view, again);
            CHECK(again.Hash() == off.Hash());
        }
    } else std::printf("  SKIP FXAA GPU comparison (%s)\n", err.c_str());
    view.postProcess.fxaa = true;
    view.clearColor = Color(0.25f, 0.5f, 0.75f);
    CHECK(Call(e, "entity.delete", R"J({"id":"Edge"})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(), view, on);
    view.postProcess.fxaa = false;
    e.Renderer().Render(e.GetScene(), view, off);
    CHECK(on.Hash() == off.Hash());
}

TEST(SpriteBillboardScript) {
    // Facing the camera is project code: samples/HD2D/scripts/billboards.lua turns tagged sprites, the
    // renderer only draws a Sprite along its entity's rotation.
    Engine e;
    std::string err, source;
    CHECK(e.Open(TempProject("sprite_billboards"), &err));
    CHECK(ReadTextFile(TestSourceDir() + "/samples/HD2D/scripts/billboards.lua", source));
    Json write = Json::MakeObject();
    write["path"] = "scripts/billboards.lua";
    write["source"] = source;
    CHECK(e.Call("script.write", write)["ok"].asBool());
    Call(e, "scene.new", R"J({"empty":true})J");
    CHECK(Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[8,7,0]},"Camera":{"fov":40}}})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Billboards","components":{"Script":{"path":"scripts/billboards.lua"}}})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"S","components":{"Transform":{"position":[0,0,0]},"Tag":{"tags":"prop, billboard"},
      "Sprite":{"width":1,"height":2,"pivotY":0,"color":[1,0,0]}}})J")["ok"].asBool());
    CHECK(Call(e, "entity.create", R"J({"name":"Plain","components":{"Transform":{"position":[0,0,3],"rotation":[0,30,0]},
      "Sprite":{"width":1,"height":1}}})J")["ok"].asBool());
    const Vec3 look = EulerLookDirection(Vec3(0, 1, 0) - Vec3(8, 7, 0));
    Json aim = Json::parse(R"J({"id":"Camera","type":"Transform","values":{}})J");
    aim["values"]["rotation"] = Json(Json::Array{look.x, look.y, look.z});
    CHECK(e.Call("component.set", aim)["ok"].asBool());
    RenderTarget shot;
    shot.Resize(128, 72);
    auto red = [&] {
        RenderView view = MakeLookAtView(Vec3(8, 7, 0), Vec3(0, 1, 0), 40.0f, 128.0f / 72);
        view.clearColor = Color(0, 0, 0);
        e.Renderer().Render(e.GetScene(), view, shot);
        int count = 0;
        for (uint32_t c : shot.color) count += (c & 0xFFFFFF) == 0x0000FF;
        return count;
    };
    CHECK(red() == 0);  // seen from the side: edge-on
    Call(e, "sim.step", R"J({"frames": 2})J");
    const Scene& scene = e.GetScene();
    CHECK(Near(scene.Get<Transform>(scene.FindByName("S"))->rotation, Vec3(0, look.y, 0), 1e-3f));
    CHECK(Near(scene.Get<Transform>(scene.FindByName("Plain"))->rotation, Vec3(0, 30, 0), 1e-6f));  // untagged sprites are left alone
    const int upright = red();
    CHECK(upright > 100);
    CHECK(shot.IdAt(64, 36) == scene.FindByName("S"));
    CHECK(Call(e, "component.set", R"J({"id":"S","type":"Tag","values":{"tags":"billboard-camera"}})J")["ok"].asBool());
    Call(e, "sim.step", R"J({"frames": 1})J");
    CHECK(Near(scene.Get<Transform>(scene.FindByName("S"))->rotation, Vec3(look.x, look.y, 0), 1e-3f));
    CHECK(red() > upright + upright / 10);  // no foreshortening from the camera's pitch
    CHECK(e.Scripts().Errors().empty());
    // The engine has no billboard switch on Sprite any more.
    CHECK(!Call(e, "component.set", R"J({"id":"S","type":"Sprite","values":{"billboard":"upright"}})J")["ok"].asBool());
}

TEST(SpriteCutoutShadows) {
    // A lit sprite with castShadows shadows the ground only behind its opaque texels, in both renderers.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("sprite_shadows"), &err));
    Image half;  // left texel opaque, right texel transparent
    half.width = 2;
    half.height = 1;
    half.rgba = {255, 255, 255, 255, 255, 255, 255, 0};
    CHECK(WritePng(JoinPath(e.ProjectDir(), "half.png"), half, true));
    Call(e, "scene.new", R"J({"empty":true})J");
    // Top-down view: screen right = +X, screen down = +Z, 8 pixels per unit.
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,20,0],"rotation":[-90,0,0]},
      "Camera":{"projection":"orthographic","orthoSize":4,"clearColor":[0,0,0]}}})J");
    Call(e, "entity.create", R"J({"name":"Sun","components":{"Transform":{"rotation":[-45,0,0]},
      "DirectionalLight":{"color":[1,1,1],"intensity":1,"ambient":[0.2,0.2,0.2],"shadowStrength":1}}})J");
    Call(e, "entity.create", R"J({"name":"Ground","components":{"Transform":{"scale":[20,1,20]},"MeshRenderer":{"mesh":"plane"}}})J");
    CHECK(Call(e, "entity.create", R"J({"name":"S","components":{"Transform":{"position":[0,0,0]},
      "Sprite":{"texture":"half.png","width":2,"height":2,"pivotY":0,"lit":true}}})J")["ok"].asBool());
    RenderView view;
    MakeSceneView(e.GetScene(), 1.0f, view);
    RenderTarget shot;
    shot.Resize(64, 64);
    auto level = [&](float x, float z) {
        return static_cast<int>(shot.color[static_cast<size_t>(32 + static_cast<int>(z * 8)) * 64 + static_cast<size_t>(32 + static_cast<int>(x * 8))] & 255);
    };
    e.Renderer().Render(e.GetScene(), view, shot);
    const int open = level(3, -1);
    CHECK(open > 100);
    CHECK(level(-0.5f, -1) == open && level(0.5f, -1) == open);  // castShadows is off by default
    CHECK(Call(e, "component.set", R"J({"id":"S","type":"Sprite","values":{"castShadows":true}})J")["ok"].asBool());
    e.Renderer().Render(e.GetScene(), view, shot);
    CHECK(level(-0.5f, -1) < open - 60);  // behind the opaque half
    CHECK(level(0.5f, -1) == open);       // the transparent half casts nothing
    CHECK(level(3, -1) == open);
    const uint64_t reference = shot.Hash();
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, shot);
    SetMaxRenderThreads(16);
    CHECK(shot.Hash() == reference);
    if (e.EnableGpu(nullptr, &err)) {
        e.Gpu()->Render(e.GetScene(), view, shot);
        const int gpuOpen = level(3, -1);
        CHECK(level(-0.5f, -1) < gpuOpen - 60);
        CHECK(std::abs(level(0.5f, -1) - gpuOpen) <= 2);
    } else std::printf("  SKIP sprite shadow GPU check (%s)\n", err.c_str());
}

TEST(CameraDepthOfField) {
    // PostProcess.dofRadius blurs surfaces away from dofFocus and leaves the focused ones untouched.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("camera_dof"), &err));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Transform":{"position":[0,0,10]},"Camera":{"clearColor":[0,0,0]},"PostProcess":{}}})J");
    Call(e, "entity.create", R"J({"name":"Near","components":{"Transform":{"position":[-3,0,0],"scale":[2,2,2]},"MeshRenderer":{"unlit":true}}})J");
    Call(e, "entity.create", R"J({"name":"Far","components":{"Transform":{"position":[12,0,-20],"scale":[8,8,8]},"MeshRenderer":{"unlit":true}}})J");
    Call(e, "entity.create", R"J({"name":"UI","components":{"UIPanel":{"anchor":"top-left","x":0,"y":0,"width":8,"height":8,"color":[1,1,1],"opacity":1}}})J");
    RenderView view;
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    RenderTarget off, on, again;
    for (RenderTarget* target : {&off, &on, &again}) target->Resize(128, 72);
    e.Renderer().Render(e.GetScene(), view, off);
    CHECK(Call(e, "component.set", R"J({"id":"Camera","type":"PostProcess","values":{"dofRadius":6,"dofFocus":10,"dofRange":2,"dofFalloff":5}})J")["ok"].asBool());
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, on);
    SetMaxRenderThreads(4);
    e.Renderer().Render(e.GetScene(), view, again);
    SetMaxRenderThreads(16);
    CHECK(on.Hash() != off.Hash() && on.Hash() == again.Hash());
    CHECK(on.depth == off.depth && on.ids == off.ids);
    // The focused cube and the black background around it are untouched; the far cube's edges turn soft.
    auto softPixels = [](const RenderTarget& a, const RenderTarget& b, int x0, int x1) {
        int count = 0;
        for (int y = 0; y < a.height; ++y) for (int x = x0; x < x1; ++x) {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(a.width) + static_cast<size_t>(x);
            count += a.color[i] != b.color[i];
        }
        return count;
    };
    CHECK(softPixels(on, off, 0, 64) == 0);
    CHECK(softPixels(on, off, 64, 128) > 40);
    int grey = 0;
    for (uint32_t c : on.color) grey += (c & 255) > 20 && (c & 255) < 235;
    CHECK(grey > 40);
    CHECK(on.color[0] == off.color[0] && (on.color[0] & 0xFFFFFF) == 0xFFFFFF);  // UI is drawn afterwards
    // Focusing on the far cube swaps the roles.
    view.postProcess.dofFocus = 28;
    view.postProcess.dofRange = 10;
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK(softPixels(again, off, 0, 64) > 20);
    CHECK(softPixels(again, off, 64, 128) == 0);
    view.postProcess.dofFocus = 10;
    view.postProcess.dofRange = 2;
    Scene restored;
    CHECK(restored.FromJson(e.GetScene().ToJson(), &err));
    CHECK(restored.Get<PostProcess>(restored.FindByName("Camera"))->dofRadius == 6);
    Call(e, "history.undo", "{}");
    RenderView neutral;
    MakeSceneView(e.GetScene(), 128.0f / 72, neutral);
    e.Renderer().Render(e.GetScene(), neutral, again);
    CHECK(again.Hash() == off.Hash());
    Call(e, "history.redo", "{}");
    if (e.EnableGpu(nullptr, &err)) {
        for (const char* tone : {"none", "reinhard"}) {
            view.postProcess.toneMapping = tone;
            view.postProcess.dofRadius = 0;
            e.Gpu()->Render(e.GetScene(), view, off);
            view.postProcess.dofRadius = 6;
            e.Gpu()->Render(e.GetScene(), view, on);
            // MSAA edge pixels of the focused cube carry the background's depth, so only its interior is exact.
            CHECK(on.color[36 * 128 + 45] == off.color[36 * 128 + 45] && (on.color[36 * 128 + 45] & 255) > 100);
            CHECK(softPixels(on, off, 64, 128) > 40);
            e.Renderer().Render(e.GetScene(), view, again);
            double error = 0;
            for (size_t i = 0; i < on.color.size(); ++i) for (int shift : {0, 8, 16})
                error += std::abs(static_cast<int>((on.color[i] >> shift) & 255) - static_cast<int>((again.color[i] >> shift) & 255));
            const double mean = error / static_cast<double>(on.color.size() * 3);
            std::printf("  depth of field (%s) software/GPU mean difference %.4f\n", tone, mean);
            CHECK(mean < 1.5);
            view.postProcess.dofRadius = 0;
            e.Gpu()->Render(e.GetScene(), view, again);
            CHECK(again.Hash() == off.Hash());
        }
    } else std::printf("  SKIP depth of field GPU comparison (%s)\n", err.c_str());
    // Orthographic cameras use the same view depth.
    CHECK(Call(e, "component.set", R"J({"id":"Camera","type":"Camera","values":{"projection":"orthographic","orthoSize":12}})J")["ok"].asBool());
    MakeSceneView(e.GetScene(), 128.0f / 72, view);
    e.Renderer().Render(e.GetScene(), view, on);
    view.postProcess.dofRadius = 0;
    e.Renderer().Render(e.GetScene(), view, off);
    CHECK(softPixels(on, off, 0, 64) == 0 && softPixels(on, off, 64, 128) > 40);
}

TEST(GpuRendererMatchesSoftware) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Showcase", &err));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    CHECK(&e.DisplayRenderer() == e.Gpu());
    const int w = 320, h = 180;
    RenderView view;
    MakeSceneView(e.GetScene(), static_cast<float>(w) / h, view);
    view.highlight = e.GetScene().FindByName("Crate 1");
    RenderTarget sw, gpu;
    sw.Resize(w, h);
    gpu.Resize(w, h);
    e.Renderer().Render(e.GetScene(), view, sw);
    e.Gpu()->Render(e.GetScene(), view, gpu);
    // Same image up to anti-aliasing and filtering: small mean difference, few outliers.
    double total = 0;
    int outliers = 0, orangeSw = 0, orangeGpu = 0;
    auto orange = [](uint32_t c) { return (c & 0xFF) > 230 && ((c >> 8) & 0xFF) > 140 && ((c >> 8) & 0xFF) < 175 && ((c >> 16) & 0xFF) < 50; };
    for (size_t i = 0; i < sw.color.size(); ++i) {
        int worst = 0;
        for (int ch = 0; ch < 3; ++ch) {
            int d = std::abs(static_cast<int>((sw.color[i] >> (8 * ch)) & 0xFF) - static_cast<int>((gpu.color[i] >> (8 * ch)) & 0xFF));
            total += d;
            worst = std::max(worst, d);
        }
        outliers += worst > 64;
        orangeSw += orange(sw.color[i]);
        orangeGpu += orange(gpu.color[i]);
    }
    double mean = total / (static_cast<double>(sw.color.size()) * 3.0);
    std::printf("  %s: mean channel difference %.2f, outliers %d of %zu\n", e.Gpu()->Name(), mean, outliers, sw.color.size());
    CHECK(mean < 6.0);
    CHECK(outliers < static_cast<int>(sw.color.size()) / 25);
    CHECK(orangeSw > 30 && orangeGpu > 30);  // selection outline on both
    // Rendering again (cached meshes/textures) gives the same frame.
    RenderTarget again;
    again.Resize(w, h);
    e.Gpu()->Render(e.GetScene(), view, again);
    CHECK(again.Hash() == gpu.Hash());
    // Clear and restore selection on reused targets: no stale outline in game frames.
    EntityId highlight = view.highlight;
    view.highlight = kNullEntity;
    e.Gpu()->Render(e.GetScene(), view, again);
    int orangeWithoutSelection = 0;
    for (uint32_t c : again.color) orangeWithoutSelection += orange(c);
    CHECK(orangeWithoutSelection < orangeGpu);
    view.highlight = highlight;
    e.Gpu()->Render(e.GetScene(), view, again);
    CHECK(again.Hash() == gpu.Hash());
    // render.screenshot {renderer: gpu} goes through the same renderer.
    Json shot = Call(e, "render.screenshot", R"J({"renderer": "gpu", "width": 64, "height": 36, "inline": false})J");
    CHECK(shot["ok"].asBool() && shot["result"]["renderer"].asString() == e.Gpu()->Name());
    CHECK(!Call(e, "render.screenshot", R"J({"renderer": "vulkan", "inline": false})J")["ok"].asBool());
}

TEST(AnimatedSkinRenderers) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("skin_renderers"), &err));
    CHECK(CopyFileTo(TestSourceDir() + "/samples/Showcase/assets/models/fox.glb", JoinPath(e.ProjectDir(), "assets/models/fox.glb")));
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Fox","components":{"Transform":{"scale":[0.012,0.012,0.012]},"MeshRenderer":{"mesh":"assets/models/fox.glb","color":[1,1,1]},"Animator":{"clip":"Walk","playing":false}}})J");
    Call(e, "entity.create", R"J({"name":"Sun","components":{"Transform":{"rotation":[-65,25,0]},"DirectionalLight":{"shadowStrength":0.8}}})J");
    Call(e, "entity.create", R"J({"name":"Ground","components":{"Transform":{"scale":[8,1,8],"position":[0,-0.01,0]},"MeshRenderer":{"mesh":"plane","color":[0.5,0.5,0.5]}}})J");
    EntityId fox = e.GetScene().FindByName("Fox");
    RenderView view = MakeLookAtView(Vec3(2,1.5f,2), Vec3(0,0.45f,0), 45, 320.0f/180);
    view.drawUI = false;
    RenderTarget sw, again, gpu;
    sw.Resize(320,180);
    again.Resize(320,180);
    gpu.Resize(320,180);
    e.Renderer().Render(e.GetScene(), view, sw);
    uint64_t start = sw.Hash();
    Call(e, "component.set", R"J({"id":"Fox","type":"Animator","values":{"time":0.25}})J");
    SetMaxRenderThreads(1);
    e.Renderer().Render(e.GetScene(), view, sw);
    SetMaxRenderThreads(16);
    e.Renderer().Render(e.GetScene(), view, again);
    CHECK(sw.Hash() != start && sw.Hash() == again.Hash() && CountId(sw, fox) > 500);
    CHECK(WritePng("build/animated-walk-software.png", sw.ToImage(), true));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    for (const char* clip : {"Survey", "Walk", "Run"}) {
        Json args = Json::parse(R"J({"id":"Fox","type":"Animator","values":{"time":0.25,"playing":false}})J");
        args["values"]["clip"] = clip;
        e.Call("component.set", args);
        for (const char* shading : {"smooth", "flat"}) {
            Json material = Json::parse(R"J({"id":"Fox","type":"MeshRenderer","values":{}})J");
            material["values"]["shading"] = shading;
            e.Call("component.set", material);
            e.Renderer().Render(e.GetScene(), view, sw);
            e.Gpu()->Render(e.GetScene(), view, gpu);
            double total = 0, interior = 0;
            int interiorCount = 0, outliers = 0;
            for (int y = 1; y < 179; ++y) for (int x = 1; x < 319; ++x) {
                size_t i = static_cast<size_t>(y)*320 + static_cast<size_t>(x);
                bool inside = sw.ids[i] == fox && sw.ids[i-1] == fox && sw.ids[i+1] == fox &&
                              sw.ids[i-320] == fox && sw.ids[i+320] == fox;
                int worst = 0;
                for (int ch = 0; ch < 3; ++ch) {
                    int diff = std::abs(static_cast<int>((sw.color[i] >> (8*ch)) & 255) - static_cast<int>((gpu.color[i] >> (8*ch)) & 255));
                    total += diff;
                    if (inside) interior += diff;
                    worst = std::max(worst, diff);
                }
                interiorCount += inside;
                outliers += worst > 64;
            }
            double mean = total / (sw.color.size()*3.0);
            double foxMean = interiorCount ? interior / (interiorCount*3.0) : 255;
            std::printf("  skin %s/%s: mean %.2f, Fox interior %.2f, outliers %d\n", clip, shading, mean, foxMean, outliers);
            CHECK(mean < 2 && foxMean < 8 && outliers < static_cast<int>(sw.color.size())/100);
            e.Gpu()->Render(e.GetScene(), view, again);
            CHECK(again.Hash() == gpu.Hash());
            if (std::string(clip) == "Walk" && std::string(shading) == "smooth")
                CHECK(WritePng("build/animated-walk-gpu.png", gpu.ToImage(), true));
        }
    }
    view.highlight = fox;
    e.Renderer().Render(e.GetScene(), view, sw);
    e.Gpu()->Render(e.GetScene(), view, gpu);
    auto orange = [](uint32_t c) { return (c & 255) > 230 && ((c >> 8) & 255) > 140 && ((c >> 8) & 255) < 175 && ((c >> 16) & 255) < 50; };
    CHECK(std::count_if(sw.color.begin(), sw.color.end(), orange) > 30);
    CHECK(std::count_if(gpu.color.begin(), gpu.color.end(), orange) > 30);
}

TEST(WebGamePak) {
    // `oe package --web` data: the page unpacks game.pak into /game.
    std::string dir = TestSourceDir() + "/samples/Hello";
    std::vector<std::string> files = GameFiles(dir);
    CHECK(std::find(files.begin(), files.end(), "project.json") != files.end());
    CHECK(std::find(files.begin(), files.end(), "AGENTS.md") == files.end());
    std::string pak = "build/test_web/game.pak";
    std::string err;
    CHECK(WriteGamePak(dir, files, pak, &err));
    std::vector<unsigned char> bytes;
    CHECK(ReadBinaryFile(pak, bytes) && bytes.size() > 12);
    CHECK(std::string(bytes.begin(), bytes.begin() + 8) == "OEPAK001");
    uint32_t n = bytes[8] | (bytes[9] << 8) | (bytes[10] << 16) | (static_cast<uint32_t>(bytes[11]) << 24);
    Json index = Json::parse(std::string(bytes.begin() + 12, bytes.begin() + 12 + n), &err);
    CHECK(err.empty() && index["files"].size() == files.size());
    for (const Json& f : index["files"].items()) {
        std::vector<unsigned char> original;
        CHECK(ReadBinaryFile(dir + "/" + f["path"].asString(), original));
        size_t at = 12 + n + static_cast<size_t>(f["offset"].asNumber());
        CHECK(original.size() == static_cast<size_t>(f["size"].asNumber()));
        CHECK(at + original.size() <= bytes.size() && std::equal(original.begin(), original.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at)));
    }
    RemoveAll("build/test_web");
}

TEST(AndroidGamePakExtract) {
    // The Android player unpacks assets/game.pak into its data folder.
    std::string dir = TestSourceDir() + "/samples/Hello";
    std::vector<std::string> files = GameFiles(dir);
    std::string err;
    CHECK(WriteGamePak(dir, files, "build/test_android/game.pak", &err));
    std::vector<unsigned char> pak;
    CHECK(ReadBinaryFile("build/test_android/game.pak", pak));
    CHECK(ExtractGamePak(pak, "build/test_android/game", &err));
    for (const std::string& f : files) {
        std::vector<unsigned char> a, b;
        CHECK(ReadBinaryFile(dir + "/" + f, a) && ReadBinaryFile("build/test_android/game/" + f, b) && a == b);
    }
    Engine e;
    CHECK(e.Open("build/test_android/game", &err));
    // Paths that would leave the folder are refused.
    auto pakWith = [](const std::string& path) {
        std::string index = "{\"files\":[{\"path\":\"" + path + "\",\"offset\":0,\"size\":1}]}";
        std::vector<unsigned char> p = {'O', 'E', 'P', 'A', 'K', '0', '0', '1'};
        for (int i = 0; i < 4; ++i) p.push_back(static_cast<unsigned char>((index.size() >> (8 * i)) & 0xFF));
        p.insert(p.end(), index.begin(), index.end());
        p.push_back('x');
        return p;
    };
    CHECK(ExtractGamePak(pakWith("ok/file.txt"), "build/test_android/x", &err));
    for (const char* bad : {"../evil.txt", "a/../../evil.txt", "/abs.txt", "c:/evil.txt", ".."}) {
        CHECK(!ExtractGamePak(pakWith(bad), "build/test_android/x", &err));
    }
    CHECK(!ExtractGamePak(std::vector<unsigned char>{'n', 'o', 'p', 'e'}, "build/test_android/x", &err));
    RemoveAll("build/test_android");
}

TEST(ZipWriterAlignment) {
    // APK rules: stored entries 4-byte aligned, native libraries page aligned.
    ZipWriter zip;
    const std::string a = "hello", b(1000, 'b');
    zip.AddStored("a.txt", reinterpret_cast<const unsigned char*>(a.data()), a.size(), 4);
    zip.AddStored("lib/arm64-v8a/libx.so", reinterpret_cast<const unsigned char*>(b.data()), b.size(), 16384);
    zip.AddStored("empty", nullptr, 0, 4);
    std::vector<unsigned char> bytes = zip.Finish();
    std::vector<ZipEntry> entries;
    std::string err;
    CHECK(ReadZip(bytes, entries, &err) && entries.size() == 3);
    if (entries.size() == 3) {
        CHECK(entries[0].name == "a.txt" && std::string(entries[0].data.begin(), entries[0].data.end()) == a);
        CHECK(entries[0].crc == Crc32(reinterpret_cast<const unsigned char*>(a.data()), a.size()));
        CHECK(entries[1].size == b.size() && entries[2].data.empty());
        // Data offsets: find each entry's bytes in the archive.
        auto offsetOf = [&](const ZipEntry& e) {
            auto it = std::search(bytes.begin(), bytes.end(), e.data.begin(), e.data.end());
            return static_cast<size_t>(it - bytes.begin());
        };
        CHECK(offsetOf(entries[0]) % 4 == 0);
        CHECK(offsetOf(entries[1]) % 16384 == 0);
        // Raw copies keep the data (as when merging archives).
        ZipWriter copy;
        for (const ZipEntry& e : entries) copy.AddRaw(e);
        std::vector<ZipEntry> again;
        CHECK(ReadZip(copy.Finish(), again, &err) && again.size() == 3 && again[1].data == entries[1].data);
    }
    CHECK(Crc32(reinterpret_cast<const unsigned char*>("123456789"), 9) == 0xCBF43926u);
    CHECK(!ReadZip(std::vector<unsigned char>(30, 0), entries, &err));
}

TEST(AndroidApk) {
    CHECK(IsValidAndroidPackageName("com.example.game") && IsValidAndroidPackageName("a.b_2"));
    CHECK(!IsValidAndroidPackageName("game") && !IsValidAndroidPackageName("com.1game") && !IsValidAndroidPackageName("com..x") &&
          !IsValidAndroidPackageName("com.my-game") && !IsValidAndroidPackageName("com.x."));
    CHECK(DefaultAndroidPackageName("My Game!") == "com.ownengine.mygame");
    CHECK(DefaultAndroidPackageName("2048") == "com.ownengine.game2048");
    CHECK(IsValidAndroidPackageName(DefaultAndroidPackageName("한글")));

    AndroidAppInfo app;
    app.packageName = "com.example.hello";
    app.label = "Hello \xEC\x95\x88\xEB\x85\x95";  // UTF-8 label -> UTF-16 in the string pool
    app.orientation = "portrait";
    app.hasIcon = true;
    std::vector<unsigned char> manifest = BuildAndroidManifest(app);
    CHECK(manifest.size() > 8 && manifest[0] == 0x03 && manifest[1] == 0x00);  // RES_XML_TYPE
    CHECK((static_cast<size_t>(manifest[4]) | (static_cast<size_t>(manifest[5]) << 8) | (static_cast<size_t>(manifest[6]) << 16) |
           (static_cast<size_t>(manifest[7]) << 24)) == manifest.size());
    auto hasUtf16 = [](const std::vector<unsigned char>& data, const std::u16string& text) {
        std::vector<unsigned char> needle;
        for (char16_t c : text) {
            needle.push_back(static_cast<unsigned char>(c & 0xFF));
            needle.push_back(static_cast<unsigned char>(c >> 8));
        }
        return std::search(data.begin(), data.end(), needle.begin(), needle.end()) != data.end();
    };
    CHECK(hasUtf16(manifest, u"android.app.NativeActivity") && hasUtf16(manifest, u"oe_player") && hasUtf16(manifest, u"com.example.hello"));
    CHECK(hasUtf16(manifest, u"Hello \uC548\uB155"));
    std::vector<unsigned char> arsc = BuildAndroidResources(app.packageName);
    CHECK(arsc.size() > 12 && arsc[0] == 0x02 && hasUtf16(arsc, u"res/drawable/icon.png"));

    // A complete (unsigned) APK with a stand-in library.
    std::string err;
    std::vector<unsigned char> so(5000, 0x7f);
    CreateDirectories("build/test_apk/arm64-v8a");
    FILE* f = std::fopen("build/test_apk/arm64-v8a/liboe_player.so", "wb");
    CHECK(f && std::fwrite(so.data(), 1, so.size(), f) == so.size());
    if (f) std::fclose(f);
    ApkContents contents;
    contents.app = app;
    contents.nativeLibs = {{"arm64-v8a", "build/test_apk/arm64-v8a/liboe_player.so"}};
    contents.gamePak = {'O', 'E', 'P', 'A', 'K', '0', '0', '1', 0, 0, 0, 0};
    CHECK(WriteUnsignedApk(contents, "build/test_apk/out.apk", &err));
    std::vector<unsigned char> apk;
    std::vector<ZipEntry> entries;
    CHECK(ReadBinaryFile("build/test_apk/out.apk", apk) && ReadZip(apk, entries, &err));
    std::set<std::string> names;
    for (const ZipEntry& e : entries) names.insert(e.name);
    for (const char* n : {"AndroidManifest.xml", "resources.arsc", "res/drawable/icon.png", "lib/arm64-v8a/liboe_player.so", "assets/game.pak", "assets/game.id"}) {
        CHECK(names.count(n) == 1);
    }
    CHECK(entries.size() >= 4 && entries[3].data == so);
    // The same contents as an app bundle (Google Play): proto manifest + resources in base/.
    CHECK(WriteUnsignedAppBundle(contents, "build/test_apk/out.aab", &err));
    std::vector<unsigned char> aab;
    CHECK(ReadBinaryFile("build/test_apk/out.aab", aab) && ReadZip(aab, entries, &err));
    names.clear();
    for (const ZipEntry& e : entries) names.insert(e.name);
    for (const char* n : {"BundleConfig.pb", "base/manifest/AndroidManifest.xml", "base/resources.pb", "base/res/drawable/icon.png",
                          "base/lib/arm64-v8a/liboe_player.so", "base/assets/game.pak", "base/assets/game.id"}) {
        CHECK(names.count(n) == 1);
    }
    auto has = [](const std::vector<unsigned char>& data, const std::string& text) {
        return std::search(data.begin(), data.end(), text.begin(), text.end(),
                           [](unsigned char x, char y) { return x == static_cast<unsigned char>(y); }) != data.end();
    };
    std::vector<unsigned char> proto = BuildAndroidManifestProto(app);
    CHECK(proto.size() > 2 && proto[0] == 0x0A);  // XmlNode.element, length-delimited
    CHECK(has(proto, "android.app.NativeActivity") && has(proto, "oe_ANativeActivity_onCreate") && has(proto, app.label));
    CHECK(has(BuildAndroidResourcesProto(app.packageName), "res/drawable/icon.png"));
    CHECK(!has(proto, "android.permission.INTERNET"));
    CHECK(!hasUtf16(BuildAndroidManifest(app), u"android.permission.INTERNET"));
    app.network = true;
    CHECK(has(BuildAndroidManifestProto(app), "android.permission.INTERNET"));
    CHECK(hasUtf16(BuildAndroidManifest(app), u"android.permission.INTERNET"));
    contents.app.packageName = "nope";
    CHECK(!WriteUnsignedApk(contents, "build/test_apk/bad.apk", &err));
    CHECK(!WriteUnsignedAppBundle(contents, "build/test_apk/bad.aab", &err));
    RemoveAll("build/test_apk");
}

// A scratch folder (with a space in its name: command lines must quote it) for process fixtures.
std::string ProcessFixtureDir(const char* name) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(TestSourceDir()) / "build/test_projects" / (std::string("oe_tests ") + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    CHECK(CreateDirectories(dir.generic_string()));
    return dir.generic_string();
}

TEST(ProcessRunsChildren) {
    std::string error;
    if (!PlatformProcessSupported()) {
        CHECK(PlatformFindExecutable("cmd").empty());
        CHECK(!PlatformStartProcess(ProcessOptions(), &error) && !error.empty());
        return;
    }
#if defined(_WIN32)
    struct Output {
        std::vector<std::string> out, err;
        int exitCode = -1;
    };
    auto finish = [](Process& process) {
        Output output;
        std::string line;
        bool isStderr = false;
        while (process.ReadLine(line, isStderr)) (isStderr ? output.err : output.out).push_back(line);
        for (int i = 0; i < 500 && process.Running(&output.exitCode); ++i) PlatformSleep(0.01);
        return output;
    };
    const std::string cmd = PlatformFindExecutable("cmd");
    CHECK(cmd.size() > 8 && cmd.find('\\') == std::string::npos && FileExists(cmd));
    CHECK(PlatformFindExecutable(cmd) == cmd);  // a path is checked as given
    CHECK(PlatformFindExecutable("oe-no-such-program").empty() && PlatformFindExecutable("").empty());

    // Both streams, line breaks stripped, the exit code.
    ProcessOptions options;
    options.executable = cmd;
    options.arguments = {"/d", "/c", "echo out& echo err>&2& exit 3"};
    options.pipeStdin = false;
    std::unique_ptr<Process> process = PlatformStartProcess(options, &error);
    CHECK(process);
    if (!process) return;
    Output output = finish(*process);
    CHECK(output.out == std::vector<std::string>{"out"} && output.err == std::vector<std::string>{"err"} && output.exitCode == 3);
    CHECK(!process->Running(nullptr) && !process->Write("x", &error));  // no stdin pipe was requested

    // stdin reaches the child; closing it ends a filter.
    options.executable = PlatformFindExecutable("findstr");
    options.arguments = {"x"};
    options.pipeStdin = true;
    process = PlatformStartProcess(options, &error);
    CHECK(process);
    if (!process) return;
    CHECK(process->Running(nullptr));
    CHECK(process->Write("x1\nskip\nx2\n", &error));
    process->CloseInput();
    output = finish(*process);
    CHECK(output.out == (std::vector<std::string>{"x1", "x2"}) && output.err.empty() && output.exitCode == 0);

    // Added environment variables and the working directory.
    const std::string dir = ProcessFixtureDir("process");
    options.executable = cmd;
    options.arguments = {"/d", "/c", "echo %OE_PROCESS_TEST%& cd"};
    options.environment = {{"OE_PROCESS_TEST", "value 1"}, {"OE_PROCESS_OTHER", ""}};
    options.workingDirectory = dir;
    options.pipeStdin = false;
    process = PlatformStartProcess(options, &error);
    CHECK(process);
    if (!process) return;
    output = finish(*process);
    CHECK(output.out.size() == 2 && output.out[0] == "value 1" && output.exitCode == 0);
    if (output.out.size() == 2) CHECK(std::filesystem::equivalent(std::filesystem::u8path(output.out[1]), std::filesystem::u8path(dir)));
    options.environment.clear();
    options.workingDirectory.clear();

    // Kill ends the whole tree: the pipes reach their end although ping would run for 30 s.
    options.arguments = {"/d", "/c", "ping -n 30 127.0.0.1"};
    process = PlatformStartProcess(options, &error);
    CHECK(process);
    if (!process) return;
    std::string line;
    bool isStderr = true;
    CHECK(process->ReadLine(line, isStderr) && !isStderr && process->Running(nullptr));  // ping is running and printing
    const double killedAt = PlatformTimeSeconds();
    process->Kill();
    output = finish(*process);
    CHECK(!process->Running(nullptr) && PlatformTimeSeconds() - killedAt < 5.0);
    process.reset();  // destroying a finished process does not hang

    // Command scripts (npm's .cmd shims) run through cmd.exe with every argument quoted.
    const std::string shim = JoinPath(dir, "shim.cmd");
    CHECK(WriteTextFile(shim, "@echo off\necho first=%~1\necho \"second=%~2\"\nexit /b 7\n"));
    options.executable = shim;
    options.arguments = {"a b", "x&y|z"};
    process = PlatformStartProcess(options, &error);
    CHECK(process);
    if (!process) return;
    output = finish(*process);
    CHECK(output.out == (std::vector<std::string>{"first=a b", "\"second=x&y|z\""}) && output.exitCode == 7);
    for (const char* unsafe : {"100%", "say \"hi\"", "wow!", "two\nlines"}) {  // cmd.exe would expand or split these
        options.arguments = {unsafe};
        error.clear();
        CHECK(!PlatformStartProcess(options, &error) && !error.empty());
    }

    options.executable = JoinPath(dir, "missing.exe");
    options.arguments.clear();
    error.clear();
    CHECK(!PlatformStartProcess(options, &error) && error.find("not found") != std::string::npos);
    options.executable = cmd;
    options.workingDirectory = JoinPath(dir, "missing");
    CHECK(!PlatformStartProcess(options, &error));
    CHECK(RemoveAll(dir));
#endif
}

#if OE_TEAM
TEST(TeamBackendParsing) {
    CHECK(ParseCliVersion("2.1.288 (Claude Code)\n") == "2.1.288");
    CHECK(ParseCliVersion("codex-cli 0.159.3") == "0.159.3");
    CHECK(ParseCliVersion("tool2 build 7, v1.20.") == "1.20");
    CHECK(ParseCliVersion("no version 42").empty() && ParseCliVersion("").empty());
    CHECK(ValidModelId("gpt-6.1-sol") && ValidModelId("claude-opus-5-5[1m]") && ValidModelId("a:b_c"));
    CHECK(!ValidModelId("") && !ValidModelId("a b") && !ValidModelId("a\"b") && !ValidModelId("a%b") && !ValidModelId(std::string(65, 'a')));

    // Listed entries in priority order; hidden ones and ids unfit for a command line are dropped.
    std::vector<ModelInfo> models = ParseCodexModelCache(R"J({"client_version":"0.160.0","models":[
        {"slug":"second","display_name":"Second","visibility":"list","priority":5,"context_window":128000},
        {"slug":"hidden","display_name":"Hidden","visibility":"hide","priority":1},
        {"slug":"bad id","visibility":"list","priority":2},
        {"slug":"first","visibility":"list","priority":1,"context_window":272000}]})J");
    CHECK(models.size() == 2);
    if (models.size() == 2) {
        CHECK(models[0].id == "first" && models[0].label == "first" && models[0].isDefault && models[0].contextWindow == 272000);
        CHECK(models[1].id == "second" && models[1].label == "Second" && !models[1].isDefault && models[1].contextWindow == 128000);
    }
    CHECK(ParseCodexModelCache("{\"models\":7}").empty() && ParseCodexModelCache("not json").empty());

    std::vector<Backend> builtin = BuiltinBackends();
    CHECK(builtin.size() == 3 && builtin[0].id == "claude" && builtin[1].id == "codex" && builtin[2].id == "gemini");
    for (const Backend& backend : builtin) {
        int defaults = 0;
        for (const ModelInfo& model : backend.models) {
            CHECK(ValidModelId(model.id) && !model.label.empty());
            defaults += model.isDefault ? 1 : 0;
        }
        CHECK(defaults == (backend.models.empty() ? 0 : 1) && !backend.installHint.empty() && !backend.executable.empty());
    }

    // User overrides: executable and catalog replaced; an invalid file changes nothing.
    const std::string dir = ProcessFixtureDir("backends");
    const std::string file = JoinPath(dir, "backends.json");
    std::string error;
    std::vector<Backend> backends = builtin;
    CHECK(ApplyBackendOverrides(file, backends, &error) && error.empty());  // a missing file is fine
    CHECK(WriteTextFile(file, R"J({"claude":{"supportsImages":true},"codex":{"executable":"C:/tools/codex.cmd","models":[{"id":"m1"},{"id":"m2","label":"Two","default":true,"contextWindow":1000}]}})J"));
    CHECK(!backends[0].supportsImages && ApplyBackendOverrides(file, backends, &error) && backends[0].supportsImages);
    CHECK(backends[1].executable == "C:/tools/codex.cmd" && backends[1].discoverModels == nullptr && backends[1].models.size() == 2);
    if (backends[1].models.size() == 2) {
        CHECK(backends[1].models[0].label == "m1" && backends[1].models[1].label == "Two");
        CHECK(backends[1].models[1].isDefault && backends[1].models[1].contextWindow == 1000);
    }
    CHECK(backends[0].executable == "claude" && backends[0].models.size() == 4 && backends[0].models[1].id == "opus" && backends[0].models[1].label == "Opus");
    for (const char* invalid : {"[]", "{\"nobody\":{}}", "{\"claude\":{\"models\":[{\"id\":\"a b\"}]}}", "{\"claude\":{\"executable\":\"\"}}", "{\"claude\":{\"supportsImages\":1}}",
                                "{\"claude\":{\"executable\":\"x\"},\"codex\":{\"models\":[{\"id\":\"a\",\"default\":true},{\"id\":\"b\",\"default\":true}]}}", "{"}) {
        backends = builtin;
        error.clear();
        CHECK(WriteTextFile(file, invalid));
        CHECK(!ApplyBackendOverrides(file, backends, &error) && error.find("backends.json") != std::string::npos);
        CHECK(backends[0].executable == "claude" && backends[1].models.size() == builtin[1].models.size());
    }
    CHECK(RemoveAll(dir));
}

TEST(TeamBackendDetection) {
    Backend missing;
    missing.id = "missing";
    missing.displayName = "Missing";
    missing.executable = "oe-no-such-agent-cli";
    missing.installHint = "npm install -g missing";
    missing.models = {{"m", "M", true, 0}};
    if (!PlatformProcessSupported()) {
        BackendState state = DetectBackend(missing);
        CHECK(state.status == BackendStatus::Unavailable && !state.message.empty() && state.models.size() == 1);
        return;
    }
#if defined(_WIN32)
    // Stand-ins for agent CLIs: real ones are not run by the tests (login, network).
    const std::string dir = ProcessFixtureDir("backends detect");
    auto shim = [&](const char* id, const std::string& script) {
        Backend backend;
        backend.id = id;
        backend.displayName = std::string("Fake ") + id;
        backend.executable = JoinPath(dir, std::string(id) + ".cmd");
        backend.installHint = "npm install -g fake";
        backend.models = {{"catalog", "Catalog", true, 0}};
        CHECK(WriteTextFile(backend.executable, "@echo off\n" + script));
        return backend;
    };
    std::vector<Backend> backends = {shim("good", "if not \"%~1\"==\"--version\" exit /b 9\necho fake-cli 1.2.3\n"),
                                     shim("failing", "echo please log in 1>&2\nexit /b 2\n"), shim("slow", "ping -n 30 127.0.0.1 >nul\n"), missing,
                                     shim("lingering", "echo linger-cli 4.5.6\nping -n 30 127.0.0.1 >nul\n"), shim("oldnode", "echo needs node 20.1, found 18.0.0\nexit /b 1\n")};
    backends[0].discoverModels = [] { return std::vector<ModelInfo>{{"found", "Found", true, 5000}}; };
    backends[0].supportsImages = true;
    backends[1].discoverModels = backends[0].discoverModels;  // not asked: the CLI does not run

    const double started = PlatformTimeSeconds();
    std::vector<BackendState> states = DetectBackends(backends, 3.0);  // generous: cmd.exe starts slowly on a busy machine
    CHECK(PlatformTimeSeconds() - started < 15.0);  // the slow one was killed, not awaited
    CHECK(states.size() == 6);
    if (states.size() != 6) return;
    // A CLI that printed its version and keeps running (an update check) is installed; a version
    // number inside a failure message is not a version.
    CHECK(states[4].status == BackendStatus::Installed && states[4].version == "4.5.6" && states[4].message.empty());
    CHECK(states[5].status == BackendStatus::Error && states[5].version.empty() && states[5].message.find("code 1") != std::string::npos);
    backends.resize(4);
    CHECK(states[0].status == BackendStatus::Installed && states[0].version == "1.2.3" && states[0].message.empty());
    CHECK(states[0].modelsDiscovered && states[0].models.size() == 1 && states[0].models[0].id == "found");
    CHECK(states[0].path.find("good.cmd") != std::string::npos && states[0].path.find('\\') == std::string::npos);
    CHECK(states[1].status == BackendStatus::Error && states[1].message.find("code 2") != std::string::npos);
    CHECK(states[1].message.find("please log in") != std::string::npos && !states[1].modelsDiscovered && states[1].models[0].id == "catalog");
    CHECK(states[2].status == BackendStatus::Error && states[2].message.find("did not finish") != std::string::npos);
    CHECK(states[3].status == BackendStatus::NotInstalled && states[3].path.empty() && states[3].message.find("PATH") != std::string::npos);

    // The command: cached for the session, detected again on refresh.
    Engine e;
    backends.erase(backends.begin() + 2);  // keep the command test fast
    RegisterTeamCommands(e.Commands(), backends);
    Json result = Call(e, "team.backends");
    CHECK(result["ok"].asBool() && result["result"].size() == 3);
    const Json good = result["result"][0];
    CHECK(good["id"].asString() == "good" && good["name"].asString() == "Fake good" && good["status"].asString() == "installed");
    CHECK(good["version"].asString() == "1.2.3" && good["path"].asString() == states[0].path && !good.has("message"));
    CHECK(good["modelSource"].asString() == "discovered" && good["supportsImages"].asBool() && good["installHint"].asString() == "npm install -g fake");
    CHECK(good["models"].size() == 1 && good["models"][0]["id"].asString() == "found" && good["models"][0]["label"].asString() == "Found");
    CHECK(good["models"][0]["default"].asBool() && good["models"][0]["contextWindow"].asInt() == 5000);
    const Json failing = result["result"][1];
    CHECK(failing["status"].asString() == "error" && !failing.has("version") && failing["modelSource"].asString() == "catalog");
    CHECK(!failing["models"][0].has("contextWindow"));
    const Json absent = result["result"][2];
    CHECK(absent["status"].asString() == "notInstalled" && !absent.has("path") && absent["models"].size() == 1);
    CHECK(WriteTextFile(backends[0].executable, "@echo off\necho fake-cli 2.0.0\n"));
    CHECK(Call(e, "team.backends")["result"][0]["version"].asString() == "1.2.3");
    CHECK(Call(e, "team.backends", R"({"refresh":true})")["result"][0]["version"].asString() == "2.0.0");
    CHECK(!Call(e, "team.backends", R"({"refresh":"yes"})")["ok"].asBool());
    CHECK(e.UndoDepth() == 0);  // not a scene edit

    // async: returns at once with "detecting", a later call has the result (the editor polls it).
    {
        Engine background;
        RegisterTeamCommands(background.Commands(), backends);
        result = Call(background, "team.backends", R"({"async":true})");
        CHECK(result["ok"].asBool() && result["result"].size() == 3 && result["result"][0]["status"].asString() == "detecting");
        CHECK(result["result"][0]["models"][0]["id"].asString() == "catalog" && !result["result"][0].has("version"));
        const double end = PlatformTimeSeconds() + 20.0;
        while (PlatformTimeSeconds() < end && Call(background, "team.backends", R"({"async":true})")["result"][0]["status"].asString() == "detecting") PlatformSleep(0.02);
        result = Call(background, "team.backends", R"({"async":true})");
        CHECK(result["result"][0]["status"].asString() == "installed" && result["result"][0]["version"].asString() == "2.0.0");
        CHECK(result["result"][2]["status"].asString() == "notInstalled");
        CHECK(Call(background, "team.backends", R"({"async":true,"refresh":true})")["result"][0]["status"].asString() == "detecting");
        CHECK(Call(background, "team.backends")["result"][0]["status"].asString() == "installed");  // a plain call waits for it
    }
    CHECK(RemoveAll(dir));
#endif
}
TEST(TeamStoreProfiles) {
    const std::string project = TempProject("team_store");
    const std::string file = JoinPath(project, ".oe/team/team.json");
    std::string error, text;
    Engine e;
    CHECK(e.Open(project, &error));
    RegisterTeamCommands(e.Commands(), BuiltinBackends());
    auto code = [&](const char* command, const char* args) { return Call(e, command, args)["error"]["code"].asString(); };

    Json team = Call(e, "team.list")["result"];
    CHECK(team["agents"].size() == 0 && team["lead"].asString() == "" && team["maxConcurrent"].asInt() == 3 && team["maxHops"].asInt() == 4);
    CHECK(!FileExists(file));  // reading creates nothing
    const double firstRevision = team["revision"].asNumber();
    Json presets = Call(e, "team.presets")["result"];
    CHECK(presets.size() == TeamAvatarPresets().size() && presets.size() >= 1 && presets[0].asString() == "fox");

    // Defaults: id from the name, first backend and its default model, first free picture, lead.
    Json mina = Call(e, "team.add", R"J({"name":" Mina Kim ","description":"Gameplay programmer.","instructions":"Work only in scripts/.\nRun script.check."})J");
    CHECK(mina["ok"].asBool());
    CHECK(mina["result"]["id"].asString() == "mina-kim" && mina["result"]["name"].asString() == "Mina Kim");
    CHECK(mina["result"]["avatar"].asString() == "preset:fox" && !mina["result"].has("avatarPath"));
    CHECK(mina["result"]["backend"].asString() == "claude" && mina["result"]["model"].asString() == "sonnet" && mina["result"]["access"].asString() == "edit");
    Json jun = Call(e, "team.add", R"J({"name":"준","backend":"codex","model":"","access":"full"})J")["result"];
    CHECK(jun["id"].asString() == "agent" && jun["avatar"].asString().rfind("preset:", 0) == 0 && jun["model"].asString() == "" && jun["access"].asString() == "full");
    CHECK(Call(e, "team.add", R"J({"name":"all!","avatar":"preset:fox"})J")["result"]["id"].asString() == "all-2");  // "all" is the everyone mention
    team = Call(e, "team.list")["result"];
    CHECK(team["agents"].size() == 3 && team["lead"].asString() == "mina-kim" && team["revision"].asNumber() > firstRevision);
    CHECK(team["agents"][2]["avatar"].asString() == "preset:fox");

    // Roster order.
    team = Call(e, "team.move", R"J({"id":"all!","index":0})J")["result"];
    CHECK(team["agents"][0]["id"].asString() == "all-2" && team["agents"][1]["id"].asString() == "mina-kim" && team["lead"].asString() == "mina-kim");
    team = Call(e, "team.move", R"J({"id":"all-2","index":99})J")["result"];
    CHECK(team["agents"][0]["id"].asString() == "mina-kim" && team["agents"][2]["id"].asString() == "all-2");
    CHECK(Call(e, "team.move", R"J({"id":"all-2","index":-1})J")["error"]["code"].asString() == "invalid_argument");
    CHECK(Call(e, "team.move", R"J({"id":"ghost","index":0})J")["error"]["code"].asString() == "unknown_agent");

    // The file is ordered and leaves defaults out.
    CHECK(ReadTextFile(file, text));
    Json stored = Json::parse(text);
    CHECK(stored["version"].asInt() == 1 && stored["lead"].asString() == "mina-kim" && !stored.has("maxConcurrent") && !stored.has("maxHops") && !stored.has("revision"));
    CHECK(stored["agents"][0].members()[0].first == "id" && stored["agents"][0]["model"].asString() == "sonnet" && !stored["agents"][0].has("access"));
    CHECK(!stored["agents"][1].has("model") && !stored["agents"][1].has("description") && stored["agents"][1]["access"].asString() == "full");
    CHECK(!FileExists(file + ".tmp"));

    // Caller mistakes change nothing.
    CHECK(code("team.add", R"J({"name":"mina kim"})J") == "name_taken");
    CHECK(code("team.add", R"J({"name":"ALL"})J") == "invalid_argument" && code("team.add", R"J({"name":"  "})J") == "invalid_argument");
    CHECK(code("team.add", R"J({"name":"A","backend":"nope"})J") == "unknown_backend");
    CHECK(code("team.add", R"J({"name":"A","model":"a b"})J") == "invalid_model");
    CHECK(code("team.add", R"J({"name":"A","avatar":"preset:dragon"})J") == "unknown_preset" && code("team.add", R"J({"name":"A","avatar":"file"})J") == "unknown_preset");
    CHECK(code("team.add", R"J({"name":"A","access":"admin"})J") == "invalid_argument" && code("team.add", R"J({"name":"two\nlines"})J") == "invalid_argument");
    CHECK(code("team.add", R"J({"description":"no name"})J") == "missing_argument");
    Json args = Json::MakeObject();
    args["name"] = std::string(41, 'x');
    CHECK(e.Call("team.add", args)["error"]["code"].asString() == "invalid_argument");
    args["name"] = std::string("\xEA\xB0\x80", 3) + std::string("\xEA\xB0\x80", 3);  // characters are counted, not bytes
    args["description"] = std::string(401, 'd');
    CHECK(e.Call("team.add", args)["error"]["code"].asString() == "invalid_argument");
    args["description"] = std::string(400, 'd');
    args["name"] = "bad \xFF utf8";
    CHECK(e.Call("team.add", args)["error"]["code"].asString() == "invalid_argument");
    CHECK(code("team.update", R"J({"id":"mina-kim","values":{"id":"other"}})J") == "invalid_argument");
    CHECK(code("team.update", R"J({"id":"mina-kim","values":{"name":7}})J") == "invalid_argument");
    CHECK(code("team.update", R"J({"id":"mina-kim","values":{"name":"준"}})J") == "name_taken");
    CHECK(code("team.update", R"J({"id":"mina-kim","values":{"avatar":"file"}})J") == "invalid_argument");
    CHECK(code("team.update", R"J({"id":"ghost","values":{}})J") == "unknown_agent" && code("team.get", R"J({"id":"ghost"})J") == "unknown_agent");
    std::string unchanged;
    CHECK(ReadTextFile(file, unchanged) && unchanged == text);

    // Partial updates; the id is stable and a new backend brings its default model.
    Json updated = Call(e, "team.update", R"J({"id":"Mina Kim","values":{"backend":"codex"}})J")["result"];
    CHECK(updated["id"].asString() == "mina-kim" && updated["backend"].asString() == "codex" && updated["model"].asString() == "");  // nothing detected: the CLI's default
    CHECK(updated["description"].asString() == "Gameplay programmer.");
    updated = Call(e, "team.update", R"J({"id":"mina-kim","values":{"name":"MINA","model":"custom-1","access":"read","backend":"claude"}})J")["result"];
    CHECK(updated["id"].asString() == "mina-kim" && updated["name"].asString() == "MINA" && updated["model"].asString() == "custom-1" && updated["access"].asString() == "read");
    CHECK(Call(e, "team.update", R"J({"id":"mina-kim","values":{"name":"Mina"}})J")["ok"].asBool());  // only the case changed
    CHECK(Call(e, "team.get", R"J({"id":"mina"})J")["result"]["id"].asString() == "mina-kim");
    CHECK(Call(e, "team.get", R"J({"id":"준"})J")["result"]["id"].asString() == "agent");

    // Settings.
    CHECK(Call(e, "team.settings")["result"]["lead"].asString() == "mina-kim");
    Json settings = Call(e, "team.settings", R"J({"lead":"준","maxConcurrent":2,"maxHops":0})J")["result"];
    CHECK(settings["lead"].asString() == "agent" && settings["maxConcurrent"].asInt() == 2 && settings["maxHops"].asInt() == 0);
    CHECK(code("team.settings", R"J({"maxConcurrent":9})J") == "invalid_argument" && code("team.settings", R"J({"maxHops":-1})J") == "invalid_argument");
    CHECK(code("team.settings", R"J({"lead":"ghost"})J") == "unknown_agent");
    CHECK(Call(e, "team.settings")["result"]["maxConcurrent"].asInt() == 2);

    // Another session (the editor next to `oe exec`) sees the same team and its changes.
    {
        Engine other;
        CHECK(other.Open(project, &error));
        RegisterTeamCommands(other.Commands(), BuiltinBackends());
        team = Call(other, "team.list")["result"];
        CHECK(team["agents"].size() == 3 && team["lead"].asString() == "agent" && team["maxConcurrent"].asInt() == 2 && team["maxHops"].asInt() == 0);
        CHECK(team["agents"][0]["name"].asString() == "Mina" && team["agents"][0]["instructions"].asString() == "Work only in scripts/.\nRun script.check.");
        PlatformSleep(0.05);  // file times tell a changed file
        CHECK(Call(other, "team.remove", R"J({"id":"준"})J")["result"]["removed"].asString() == "agent");
    }
    team = Call(e, "team.list")["result"];
    CHECK(team["agents"].size() == 2 && team["lead"].asString() == "");  // the removed agent was the lead
    CHECK(code("team.remove", R"J({"id":"agent"})J") == "unknown_agent");

    // At most 16 agents; pictures repeat once all presets are taken.
    for (int i = 0; i < 14; ++i) {
        args = Json::MakeObject();
        args["name"] = "Bot " + std::to_string(i);
        CHECK(e.Call("team.add", args)["ok"].asBool());
    }
    CHECK(Call(e, "team.list")["result"]["agents"].size() == 16 && code("team.add", R"J({"name":"One too many"})J") == "team_full");
    CHECK(Call(e, "team.get", R"J({"id":"bot-13"})J")["result"]["avatar"].asString() == "preset:fox");

    // An invalid file is shown as an empty team and is never overwritten.
    PlatformSleep(0.05);
    CHECK(WriteTextFile(file, "{ broken"));
    team = Call(e, "team.list")["result"];
    CHECK(team["agents"].size() == 0 && team["error"].asString().find("team.json") != std::string::npos);
    CHECK(code("team.add", R"J({"name":"A"})J") == "team_file_invalid" && code("team.settings", R"J({"maxHops":1})J") == "team_file_invalid");
    CHECK(ReadTextFile(file, text) && text == "{ broken");
    PlatformSleep(0.05);
    CHECK(WriteTextFile(file, R"J({"version":1,"lead":"a","future":true,"agents":[{"id":"a","name":"A","avatar":"preset:later","backend":"someday","extra":1}]})J"));
    team = Call(e, "team.list")["result"];  // unknown keys, backend and preset keep loading
    CHECK(!team.has("error") && team["agents"].size() == 1 && team["agents"][0]["backend"].asString() == "someday" && team["lead"].asString() == "a");
    for (const char* invalid : {R"J({"version":2,"agents":[]})J", R"J({"agents":[{"id":"A B","name":"A","avatar":"preset:fox"}]})J",
                                R"J({"agents":[{"id":"a","name":"A","avatar":"preset:fox"},{"id":"a","name":"B","avatar":"preset:fox"}]})J",
                                R"J({"lead":"nobody","agents":[]})J", R"J({"agents":[{"id":"a","name":"A","avatar":"../x"}]})J", R"J([])J"}) {
        PlatformSleep(0.05);
        CHECK(WriteTextFile(file, invalid));
        CHECK(Call(e, "team.list")["result"].has("error"));
    }
    CHECK(e.UndoDepth() == 0 && !e.Dirty());  // the team is not part of the scene
}

TEST(TeamAvatars) {
    auto pixel = [](const Image& image, int x, int y) {
        const uint8_t* p = &image.rgba[(static_cast<size_t>(y) * static_cast<size_t>(image.width) + static_cast<size_t>(x)) * 4];
        return std::vector<int>{p[0], p[1], p[2], p[3]};
    };
    auto filled = [](int width, int height, const std::function<std::vector<int>(int, int)>& color) {
        Image image;
        image.width = width;
        image.height = height;
        image.rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::vector<int> c = color(x, y);
                for (int k = 0; k < 4; ++k) image.rgba[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4 + static_cast<size_t>(k)] = static_cast<uint8_t>(c[static_cast<size_t>(k)]);
            }
        }
        return image;
    };
    const std::vector<int> red = {255, 0, 0, 255}, blue = {0, 0, 255, 255};

    // Center crop: of a 512x256 picture split at x = 256 the middle square is half red, half blue.
    const Image wide = filled(512, 256, [&](int x, int) { return x < 256 ? red : blue; });
    Image avatar = MakeAvatarImage(wide);
    CHECK(avatar.width == kAvatarSize && avatar.height == kAvatarSize && avatar.rgba.size() == 256u * 256u * 4u);
    CHECK(pixel(avatar, 4, 128) == red && pixel(avatar, 127, 4) == red && pixel(avatar, 128, 250) == blue && pixel(avatar, 251, 128) == blue);
    // Downscale averages; upscale keeps hard edges; transparent pixels do not tint their neighbours.
    avatar = MakeAvatarImage(filled(512, 512, [](int x, int y) { return (x + y) % 2 ? std::vector<int>{255, 255, 255, 255} : std::vector<int>{0, 0, 0, 255}; }));
    CHECK(std::abs(pixel(avatar, 100, 100)[0] - 128) <= 1 && pixel(avatar, 100, 100)[3] == 255);
    avatar = MakeAvatarImage(filled(2, 2, [](int x, int y) { return x == 0 && y == 0 ? std::vector<int>{255, 255, 255, 255} : std::vector<int>{0, 0, 0, 0}; }));
    CHECK(pixel(avatar, 127, 127) == (std::vector<int>{255, 255, 255, 255}) && pixel(avatar, 128, 127)[3] == 0 && pixel(avatar, 127, 128)[3] == 0);
    avatar = MakeAvatarImage(filled(4, 2, [](int x, int) { return x < 2 ? std::vector<int>{200, 100, 50, 255} : std::vector<int>{0, 0, 0, 0}; }));
    CHECK(MakeAvatarImage(Image()).rgba.empty());

    const std::string project = TempProject("team_avatars");
    const std::string outside = ProcessFixtureDir("avatars");  // pictures come from anywhere on the PC
    std::string error;
    Engine e;
    CHECK(e.Open(project, &error));
    RegisterTeamCommands(e.Commands(), BuiltinBackends());
    CHECK(Call(e, "team.add", R"J({"name":"Mina"})J")["ok"].asBool());
    auto setAvatar = [&](const char* key, const std::string& value) {
        Json args = Json::MakeObject();
        args["id"] = "mina";
        args[key] = value;
        return e.Call("team.avatar", args);
    };
    const std::string picture = JoinPath(outside, "Photo.PNG"), stored = JoinPath(project, ".oe/team/avatars/mina.png");
    CHECK(WritePng(picture, wide, true));
    Json result = setAvatar("source", picture);
    CHECK(result["ok"].asBool() && result["result"]["avatar"].asString() == "file" && result["result"]["avatarPath"].asString() == ".oe/team/avatars/mina.png");
    std::vector<unsigned char> bytes;
    Texture texture;
    CHECK(ReadBinaryFile(stored, bytes) && DecodeImage(bytes.data(), bytes.size(), texture, &error));
    CHECK(texture.width == kAvatarSize && texture.height == kAvatarSize);
    if (texture.width == kAvatarSize && texture.height == kAvatarSize) {
        Image decoded;
        decoded.width = decoded.height = kAvatarSize;
        decoded.rgba.resize(texture.texels.size() * 4);
        std::memcpy(decoded.rgba.data(), texture.texels.data(), decoded.rgba.size());
        CHECK(pixel(decoded, 4, 128) == red && pixel(decoded, 251, 128) == blue);
    }
    CHECK(!FileExists(stored + ".tmp") && FileExists(picture));  // the source is only read
    std::string text;
    CHECK(ReadTextFile(JoinPath(project, ".oe/team/team.json"), text) && Json::parse(text)["agents"][0]["avatar"].asString() == "file");

    // Refused pictures leave the stored one alone.
    const std::string notes = JoinPath(outside, "notes.txt"), garbage = JoinPath(outside, "garbage.png"), huge = JoinPath(outside, "huge.png");
    CHECK(WriteTextFile(notes, "hello") && WriteTextFile(garbage, "not a png"));
    CHECK(WritePng(huge, filled(8193, 1, [&](int, int) { return red; }), true));
    CHECK(setAvatar("source", notes)["error"]["code"].asString() == "invalid_image");
    CHECK(setAvatar("source", garbage)["error"]["code"].asString() == "invalid_image");
    CHECK(setAvatar("source", huge)["error"]["code"].asString() == "invalid_image");
    CHECK(setAvatar("source", JoinPath(outside, "missing.png"))["error"]["code"].asString() == "not_found");
    CHECK(setAvatar("preset", "dragon")["error"]["code"].asString() == "unknown_preset");
    CHECK(Call(e, "team.avatar", R"J({"id":"mina"})J")["error"]["code"].asString() == "invalid_argument");
    CHECK(Call(e, "team.avatar", R"J({"id":"mina","preset":"fox","source":"x.png"})J")["error"]["code"].asString() == "invalid_argument");
    CHECK(Call(e, "team.avatar", R"J({"id":"ghost","preset":"fox"})J")["error"]["code"].asString() == "unknown_agent");
    CHECK(Call(e, "team.get", R"J({"id":"mina"})J")["result"]["avatar"].asString() == "file" && FileExists(stored));

    // A preset replaces the upload; removing an agent removes its picture.
    result = setAvatar("preset", "fox");
    CHECK(result["result"]["avatar"].asString() == "preset:fox" && !result["result"].has("avatarPath") && !FileExists(stored));
    CHECK(setAvatar("source", picture)["ok"].asBool() && FileExists(stored));
    CHECK(Call(e, "team.update", R"J({"id":"mina","values":{"avatar":"preset:fox"}})J")["ok"].asBool() && !FileExists(stored));
    CHECK(setAvatar("source", picture)["ok"].asBool() && FileExists(stored));
    CHECK(Call(e, "team.remove", R"J({"id":"mina"})J")["ok"].asBool() && !FileExists(stored));
    CHECK(RemoveAll(outside));
}
TEST(TeamStreamParsers) {
    auto parse = [](void (*parser)(const std::string&, TurnEvents&), const char* line) {
        TurnEvents events;
        parser(line, events);
        return events;
    };
    using Kind = TurnEvent::Kind;

    // Lines recorded from Claude Code 2.1.288 (`-p --output-format stream-json --verbose`), shortened.
    TurnEvents events = parse(ParseClaudeLine, R"J({"type":"system","subtype":"init","cwd":"C:\\p","session_id":"9cded71b-2279-4cb4-b3a2-42e1b27ba20d","tools":["Read"],"model":"claude-haiku-4-5-20251001","permissionMode":"acceptEdits"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Session && events[0].text == "9cded71b-2279-4cb4-b3a2-42e1b27ba20d");
    CHECK(parse(ParseClaudeLine, R"J({"type":"system","subtype":"hook_started","hook_name":"SessionStart:startup","session_id":"9cded71b"})J").empty());
    CHECK(parse(ParseClaudeLine, R"J({"type":"system","subtype":"thinking_tokens","estimated_tokens":50,"session_id":"9cded71b"})J").empty());
    CHECK(parse(ParseClaudeLine, R"J({"type":"rate_limit_event","rate_limit_info":{"status":"allowed_warning"},"session_id":"9cded71b"})J").empty());
    events = parse(ParseClaudeLine, R"J({"type":"assistant","message":{"model":"claude-haiku-4-5-20251001","role":"assistant","content":[{"type":"thinking","thinking":"","signature":"ErQH"}],"stop_reason":null},"parent_tool_use_id":null,"session_id":"9cded71b"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Thinking);
    events = parse(ParseClaudeLine, R"J({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"toolu_01RK","name":"Read","input":{"file_path":"C:\\p\\hello.txt"},"caller":{"type":"direct"}}]},"parent_tool_use_id":null,"session_id":"9cded71b"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Tool && events[0].text == "Reading" && events[0].detail == "C:\\p\\hello.txt");
    CHECK(events.size() == 1 && events[0].detailIsPath && events[0].itemId == "toolu_01RK");
    events = parse(ParseClaudeLine, R"J({"type":"user","message":{"role":"user","content":[{"tool_use_id":"toolu_01RK","type":"tool_result","content":"1\tbanana-42\n2\t"}]},"parent_tool_use_id":null,"session_id":"9cded71b"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Thinking);
    events = parse(ParseClaudeLine, R"J({"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"banana-42\n\nDONE"}]},"parent_tool_use_id":null,"session_id":"9cded71b"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Text && events[0].text == "banana-42\n\nDONE");
    events = parse(ParseClaudeLine, R"J({"duration_api_ms":4917,"stop_reason":"end_turn","session_id":"9cded71b-2279-4cb4-b3a2-42e1b27ba20d","total_cost_usd":0.0293026,"is_error":false,"num_turns":2,"subtype":"success","result":"banana-42\n\nDONE","type":"result","duration_ms":5456})J");
    CHECK(events.size() == 2 && events[0].kind == Kind::Session && events[1].kind == Kind::Done && events[1].text == "banana-42\n\nDONE");
    CHECK(events.size() == 2 && std::abs(events[1].costUsd - 0.0293026) < 1e-9);
    // Other tools, a delegated agent's text (not the reply) and failed results.
    events = parse(ParseClaudeLine, R"J({"type":"assistant","message":{"content":[{"type":"tool_use","id":"a","name":"mcp__ownengine__component_set","input":{"id":"Player","type":"Transform","values":{}}},{"type":"tool_use","id":"b","name":"Bash","input":{"command":"build.bat"}},{"type":"tool_use","id":"c","name":"Edit","input":{"file_path":"scripts/x.lua"}},{"type":"tool_use","id":"d","name":"mcp__ownengine__entity_set_parent","input":{"id":7}},{"type":"tool_use","id":"e","name":"TodoWrite","input":{}}]}})J");
    CHECK(events.size() == 5);
    if (events.size() == 5) {
        CHECK(events[0].text == "Engine:" && events[0].detail == "component.set Player" && !events[0].detailIsPath);
        CHECK(events[1].text == "Running" && events[1].detail == "build.bat" && events[2].text == "Editing" && events[2].detail == "scripts/x.lua");
        CHECK(events[3].detail == "entity.set_parent 7" && events[4].text == "Using" && events[4].detail == "TodoWrite");
    }
    events = parse(ParseClaudeLine, R"J({"type":"assistant","message":{"content":[{"type":"text","text":"sub"},{"type":"tool_use","id":"s","name":"Grep","input":{"pattern":"jump"}}]},"parent_tool_use_id":"toolu_parent"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Tool && events[0].text == "Searching" && events[0].detail == "jump");
    events = parse(ParseClaudeLine, R"J({"type":"result","subtype":"success","is_error":true,"result":"Not logged in · Please run /login","session_id":"s1"})J");
    CHECK(events.size() == 2 && events[1].kind == Kind::Failed && events[1].text.find("Not logged in") == 0);
    events = parse(ParseClaudeLine, R"J({"type":"result","subtype":"error_max_turns","is_error":false})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Failed && events[0].text.find("error_max_turns") != std::string::npos);

    // Lines recorded from codex-cli 0.159.3 (`exec --json`).
    events = parse(ParseCodexLine, R"J({"type":"thread.started","thread_id":"01a101f0-7fc8-7651-a1bf-b87b4d7ebb01"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Session && events[0].text == "01a101f0-7fc8-7651-a1bf-b87b4d7ebb01");
    events = parse(ParseCodexLine, R"J({"type":"turn.started"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Thinking);
    events = parse(ParseCodexLine, R"J({"type":"item.completed","item":{"id":"item_0","type":"agent_message","text":"I’ll read hello.txt.\n"}})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Text && events[0].text == "I\xE2\x80\x99ll read hello.txt.\n");
    const char* commandStarted = R"J({"type":"item.started","item":{"id":"item_1","type":"command_execution","command":"\"C:\\\\WINDOWS\\\\System32\\\\WindowsPowerShell\\\\v1.0\\\\powershell.exe\" -Command 'Get-Content -LiteralPath hello.txt -Raw'","aggregated_output":"","exit_code":null,"status":"in_progress"}})J";
    events = parse(ParseCodexLine, commandStarted);
    CHECK(events.size() == 1 && events[0].kind == Kind::Tool && events[0].text == "Running" && events[0].itemId == "item_1");
    CHECK(events.size() == 1 && events[0].detail.find("powershell.exe\" -Command 'Get-Content") != std::string::npos);
    events = parse(ParseCodexLine, R"J({"type":"item.completed","item":{"id":"item_1","type":"command_execution","command":"powershell","aggregated_output":"banana-42\n\r\n","exit_code":0,"status":"completed"}})J");
    CHECK(events.size() == 2 && events[0].kind == Kind::Tool && events[0].itemId == "item_1" && events[1].kind == Kind::Thinking);
    events = parse(ParseCodexLine, R"J({"type":"turn.completed","usage":{"input_tokens":31220,"cached_input_tokens":27904,"output_tokens":62}})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Done && events[0].text.empty() && events[0].costUsd == 0.0);
    events = parse(ParseCodexLine, R"J({"type":"item.started","item":{"id":"m","type":"mcp_tool_call","server":"ownengine","tool":"scene_summary","arguments":{}}})J");
    CHECK(events.size() == 1 && events[0].text == "Engine:" && events[0].detail == "scene.summary");
    events = parse(ParseCodexLine, R"J({"type":"item.completed","item":{"id":"f","type":"file_change","changes":[{"path":"scripts/player.lua","kind":"update"}],"status":"completed"}})J");
    CHECK(events.size() == 2 && events[0].text == "Editing" && events[0].detail == "scripts/player.lua" && events[0].detailIsPath);
    events = parse(ParseCodexLine, R"J({"type":"turn.failed","error":{"message":"stream disconnected"}})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Failed && events[0].text == "stream disconnected");
    events = parse(ParseCodexLine, R"J({"type":"error","message":"401 Unauthorized"})J");
    CHECK(events.size() == 1 && events[0].kind == Kind::Failed && events[0].text == "401 Unauthorized");

    // A stream never fails a turn by its shape: cut, foreign and non-JSON lines give nothing.
    for (auto parser : {&ParseClaudeLine, &ParseCodexLine}) {
        for (const char* line : {"", "plain text", "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"te", "[1,2]", "{\"type\":7}", "{\"type\":\"future.event\",\"x\":1}",
                                 "{\"type\":\"assistant\",\"message\":\"text\"}", "{\"type\":\"item.completed\",\"item\":7}"}) {
            CHECK(parse(parser, line).empty());
        }
    }

    // Command lines: argv only, no user text, values a .cmd shim accepts (no double quotes).
    TurnRequest request;
    request.executable = "C:/bin/claude.exe";
    request.model = "sonnet";
    request.systemPromptFile = ".oe/team/tmp/mina.system.md";
    ProcessOptions options = ClaudeBuildTurn(request);
    CHECK(options.executable == request.executable);
    CHECK(options.arguments == (std::vector<std::string>{"-p", "--output-format", "stream-json", "--verbose", "--model", "sonnet", "--append-system-prompt-file",
                                                         ".oe/team/tmp/mina.system.md", "--permission-mode", "acceptEdits"}));
    request.model.clear();
    request.access = "read";
    request.sessionId = "abc-123";
    request.mcpConfigFile = ".oe/team/tmp/mina.mcp.json";
    options = ClaudeBuildTurn(request);
    CHECK(options.arguments == (std::vector<std::string>{"-p", "--output-format", "stream-json", "--verbose", "--append-system-prompt-file", ".oe/team/tmp/mina.system.md",
                                                         "--permission-mode", "plan", "--mcp-config", ".oe/team/tmp/mina.mcp.json", "--strict-mcp-config", "--resume", "abc-123"}));
    request.access = "edit";
    options = ClaudeBuildTurn(request);
    CHECK(std::find(options.arguments.begin(), options.arguments.end(), "mcp__ownengine") != options.arguments.end());
    request.access = "full";
    options = ClaudeBuildTurn(request);
    CHECK(std::find(options.arguments.begin(), options.arguments.end(), "bypassPermissions") != options.arguments.end());

    request = TurnRequest();
    request.executable = "C:/npm/codex.cmd";
    request.model = "gpt-6.1-sol";
    options = CodexBuildTurn(request);
    CHECK(options.arguments == (std::vector<std::string>{"exec", "--json", "--skip-git-repo-check", "-m", "gpt-6.1-sol", "-c", "sandbox_mode='workspace-write'", "-"}));
    request.model.clear();
    request.access = "full";
    request.sessionId = "01a101f0-7fc8";
    request.oeExecutable = "C:/Program Files/oe/oe.exe";
    request.apiPort = 7777;
    options = CodexBuildTurn(request);
    CHECK(options.arguments == (std::vector<std::string>{"exec", "resume", "--json", "--skip-git-repo-check", "-c", "sandbox_mode='danger-full-access'", "-c",
                                                         "mcp_servers.ownengine.command='C:/Program Files/oe/oe.exe'", "-c",
                                                         "mcp_servers.ownengine.args=['mcp','--connect','7777']", "-c",
                                                         "mcp_servers.ownengine.default_tools_approval_mode='approve'", "01a101f0-7fc8", "-"}));
    request.agentId = "mina";
    options = CodexBuildTurn(request);
    CHECK(std::find(options.arguments.begin(), options.arguments.end(), "mcp_servers.ownengine.args=['mcp','--connect','7777','--agent','mina']") != options.arguments.end());
    request.access = "read";  // a read-only agent may not change the scene through the editor either
    options = CodexBuildTurn(request);
    CHECK(std::find(options.arguments.begin(), options.arguments.end(), "mcp_servers.ownengine.default_tools_approval_mode='approve'") == options.arguments.end());
    CHECK(std::find(options.arguments.begin(), options.arguments.end(), "sandbox_mode='read-only'") != options.arguments.end());
    for (const std::string& argument : options.arguments) CHECK(argument.find_first_of("\"%!") == std::string::npos);
    CHECK(ValidSessionId("01a101f0-7fc8_x") && !ValidSessionId("") && !ValidSessionId("a b") && !ValidSessionId("a'b") && !ValidSessionId(std::string(65, 'a')));

    // Mentions: ids and names without regard to case, the longest match, not inside a word.
    std::vector<AgentProfile> agents(3);
    agents[0].id = "mina";
    agents[0].name = "Mina";
    agents[1].id = "mina-kim";
    agents[1].name = "Mina Kim";
    agents[2].id = "agent";
    agents[2].name = "\xEC\xA4\x80";  // a Korean name; the id has no letters of it
    bool all = false;
    CHECK(FindMentions("@Mina add a jump", agents, all) == std::vector<size_t>{0} && !all);
    CHECK(FindMentions("hey @mina kim, and @MINA.", agents, all) == (std::vector<size_t>{1, 0}));
    CHECK(FindMentions("@mina-kim @mina @Mina", agents, all) == (std::vector<size_t>{1, 0}));
    CHECK(FindMentions("\xEC\xA4\x80 and @\xEC\xA4\x80!", agents, all) == std::vector<size_t>{2});
    CHECK(FindMentions("mail user@mina.io, @minas, @min, @ mina", agents, all).empty() && !all);
    CHECK(FindMentions("@all please, @mina first", agents, all) == std::vector<size_t>{0} && all);
    CHECK(FindMentions("@allies", agents, all).empty() && !all);
}

TEST(TeamSessionTurns) {
    if (!PlatformProcessSupported()) return;
#if defined(_WIN32)
    const std::string project = TempProject("team_session");
    const std::string dir = ProcessFixtureDir("team session");  // stand-in CLIs; agents write what they received here
    std::string error, text;
    // A stand-in agent CLI: a command script that saves its prompt and arguments, then prints a
    // recorded Claude stream. Real CLIs are not run by the tests (login, network, cost).
    auto fake = [&](const char* id, const std::string& script) {
        Backend backend;
        backend.id = id;
        backend.displayName = std::string("Fake ") + id;
        backend.executable = JoinPath(dir, std::string(id) + ".cmd");
        backend.installHint = "npm install -g fake";
        backend.models = {{"m", "M", true, 0}};
        backend.systemPromptFlag = true;
        backend.buildTurn = [](const TurnRequest& request) {
            ProcessOptions options;
            options.executable = request.executable;
            options.arguments = {request.sessionId.empty() ? "new" : request.sessionId, request.model, request.access, request.systemPromptFile.empty() ? "nofile" : request.systemPromptFile,
                                 request.mcpConfigFile.empty() ? "nomcp" : request.mcpConfigFile};
            return options;
        };
        backend.parseLine = &ParseClaudeLine;
        CHECK(WriteTextFile(backend.executable, "@echo off\n" + script));
        return backend;
    };
    const std::string save = "findstr \"^\" > \"%~dp0%OE_TEAM_AGENT%.prompt.txt\"\necho %*> \"%~dp0%OE_TEAM_AGENT%.args.txt\"\n"
                             "echo hop%OE_TEAM_HOP%h> \"%~dp0%OE_TEAM_AGENT%.hop.txt\"\n";
    std::vector<Backend> backends = {fake("echo", save + "type \"%~dp0reply.jsonl\"\n"), fake("plain", save + "type \"%~dp0reply.jsonl\"\n"),
                                     fake("slow", "findstr \"^\" >nul\ntype \"%~dp0tool.jsonl\"\nping -n 30 127.0.0.1 >nul\n"),
                                     fake("crash", "findstr \"^\" >nul\necho boom 1>&2\nexit /b 3\n"), fake("denied", "findstr \"^\" >nul\ntype \"%~dp0denied.jsonl\"\nexit /b 1\n"),
                                     fake("missing", ""), fake("later", ""),
                                     fake("ping", save + "type \"%~dp0ping.jsonl\"\n"), fake("pong", save + "type \"%~dp0pong.jsonl\"\n"),
                                     fake("painter", save + "copy /y \"%~dp0pic.png\" \".oe\\team\\images\\made.png\" >nul\ntype \"%~dp0reply.jsonl\"\n")};
    backends[1].systemPromptFlag = false;
    backends[5].executable = "oe-no-such-agent-cli";
    backends[6].buildTurn = nullptr;  // detected, but no turn adapter yet
    const std::string toolLine = R"J({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Read","input":{"file_path":")J" + project + R"J(/scripts/player.lua"}}]}})J";
    CHECK(WriteTextFile(JoinPath(dir, "tool.jsonl"), toolLine + "\n"));
    CHECK(WriteTextFile(JoinPath(dir, "reply.jsonl"), std::string(R"J({"type":"system","subtype":"init","session_id":"sess-1"})J") + "\nnot json\n" + toolLine + "\n" +
                                                         R"J({"type":"user","message":{}})J" + "\n" + R"J({"type":"assistant","message":{"content":[{"type":"text","text":"Done it."}]}})J" + "\n" +
                                                         R"J({"type":"result","subtype":"success","is_error":false,"result":"Done it.","session_id":"sess-1","total_cost_usd":0.25})J" + "\n"));
    CHECK(WriteTextFile(JoinPath(dir, "denied.jsonl"), R"J({"type":"result","subtype":"success","is_error":true,"result":"Not logged in","session_id":"sess-9"})J" "\n"));

    Engine e;
    CHECK(e.Open(project, &error));
    std::shared_ptr<TeamHandle> team = RegisterTeamCommands(e.Commands(), backends);
    for (const char* agent : {R"J({"name":"Mina","backend":"echo","description":"Gameplay."})J", R"J({"name":"Jun","backend":"plain","instructions":"Be brief."})J", R"J({"name":"Sloth","backend":"slow"})J",
                              R"J({"name":"Crash","backend":"crash"})J", R"J({"name":"Denied","backend":"denied"})J", R"J({"name":"Ghost","backend":"missing"})J",
                              R"J({"name":"Later","backend":"later"})J"}) {
        CHECK(Call(e, "team.add", agent)["ok"].asBool());
    }
    auto stateOf = [&](const std::string& id) {
        const Json state = Call(e, "team.state")["result"];
        for (const Json& agent : state["agents"].items()) {
            if (agent["id"].asString() == id) return agent;
        }
        return Json();
    };
    auto until = [&](const std::function<bool()>& done) {
        const double end = PlatformTimeSeconds() + 30.0;
        while (PlatformTimeSeconds() < end && !done()) PlatformSleep(0.02);
        return done();
    };
    auto settled = [&] {
        const Json state = Call(e, "team.state")["result"];
        for (const Json& agent : state["agents"].items()) {
            if (agent["state"].asString() == "queued") return false;
        }
        return state["running"].asInt() == 0;
    };
    auto lastMessage = [&] {
        const Json messages = Call(e, "team.messages", R"J({"limit":1})J")["result"]["messages"];
        return messages.size() ? messages[0] : Json();
    };
    auto fileText = [&](const std::string& name) {
        std::string content;
        ReadTextFile(JoinPath(dir, name), content);
        return content;
    };
    auto code = [&](const char* command, const char* args) { return Call(e, command, args)["error"]["code"].asString(); };

    // Before any turn: who can work.
    CHECK(stateOf("mina")["state"].asString() == "idle" && stateOf("mina")["activity"].asString() == "" && !stateOf("mina").has("error"));
    CHECK(stateOf("ghost")["state"].asString() == "offline" && stateOf("ghost")["error"].asString().find("not installed") != std::string::npos);
    CHECK(stateOf("ghost")["hint"].asString().find("npm install -g fake") != std::string::npos);
    CHECK(stateOf("later")["state"].asString() == "offline" && stateOf("later")["error"].asString().find("cannot run turns yet") != std::string::npos);
    CHECK(Call(e, "team.messages")["result"]["messages"].size() == 0 && !FileExists(JoinPath(project, ".oe/team/chat.jsonl")));

    // A message without a mention goes to the lead (the first agent).
    Json sent = Call(e, "team.send", R"J({"text":"hello team\nsecond line"})J");
    CHECK(sent["ok"].asBool() && sent["result"]["started"].size() == 1 && sent["result"]["started"][0].asString() == "mina");
    CHECK(sent["result"]["queued"].size() == 0 && sent["result"]["offline"].size() == 0);
    CHECK(sent["result"]["message"]["id"].asInt() == 1 && sent["result"]["message"]["from"].asString() == "user" && sent["result"]["message"]["to"][0].asString() == "mina");
    CHECK(sent["result"]["message"]["kind"].asString() == "message" && sent["result"]["message"]["time"].asString().size() == 20);
    CHECK(until(settled));
    Json reply = lastMessage();
    CHECK(reply["id"].asInt() == 2 && reply["from"].asString() == "mina" && reply["to"][0].asString() == "user" && reply["kind"].asString() == "message");
    CHECK(reply["text"].asString() == "Done it." && reply["turn"]["tools"].asInt() == 1 && reply["turn"]["costUsd"].asNumber() == 0.25);
    CHECK(reply["turn"]["steps"].size() == 1 && reply["turn"]["steps"][0].asString() == "Reading scripts/player.lua" && reply["turn"]["seconds"].asNumber() >= 0.0);
    CHECK(stateOf("mina")["state"].asString() == "idle" && stateOf("mina")["tools"].asInt() == 0);
    // What the CLI got: the prompt on stdin, only generated values as arguments, the context as a file.
    CHECK(fileText("mina.prompt.txt").find("[User] hello team") != std::string::npos && fileText("mina.prompt.txt").find("second line") != std::string::npos);
    CHECK(fileText("mina.args.txt").find(R"J("new" "m" "edit" ".oe/team/tmp/mina.system.md" "nomcp")J") == 0);
    CHECK(ReadTextFile(JoinPath(project, ".oe/team/tmp/mina.system.md"), text));
    CHECK(text.find("You are Mina (@mina)") == 0 && text.find("Gameplay.") != std::string::npos && text.find("- @jun - Jun") != std::string::npos);
    CHECK(text.find("No editor is attached") != std::string::npos);
    CHECK(ReadTextFile(JoinPath(project, ".oe/team/sessions.json"), text) && Json::parse(text)["mina"]["session"].asString() == "sess-1");
    CHECK(Json::parse(text)["mina"]["seen"].asInt() == 1 && Json::parse(text)["mina"]["backend"].asString() == "echo");

    // The next turn resumes the conversation and gets only what is new; with an API port the agent can attach to the editor.
    team->host.apiPort = 7777;
    team->host.oeExecutable = backends[0].executable;  // any existing file stands in for `oe`
    CHECK(Call(e, "team.send", R"J({"text":"@MINA again"})J")["result"]["started"].size() == 1);
    CHECK(until(settled));
    CHECK(fileText("mina.args.txt").find(R"J("sess-1" "m" "edit" ".oe/team/tmp/mina.system.md" ".oe/team/tmp/mina.mcp.json")J") == 0);
    CHECK(fileText("mina.prompt.txt").find("[User] @MINA again") != std::string::npos && fileText("mina.prompt.txt").find("hello team") == std::string::npos);
    CHECK(ReadTextFile(JoinPath(project, ".oe/team/tmp/mina.mcp.json"), text));
    const Json mcp = Json::parse(text)["mcpServers"]["ownengine"];
    CHECK(mcp["command"].asString() == backends[0].executable && mcp["args"].size() == 5 && mcp["args"][2].asString() == "7777");
    CHECK(mcp["args"][3].asString() == "--agent" && mcp["args"][4].asString() == "mina");  // the editor shows who made an edit
    CHECK(ReadTextFile(JoinPath(project, ".oe/team/tmp/mina.system.md"), text) && text.find("`ownengine` MCP server") != std::string::npos);
    team->host.apiPort = 0;

    // A CLI without a system prompt flag gets the context once, on stdin, and sees what teammates said.
    CHECK(Call(e, "team.send", R"J({"text":"@Jun hi"})J")["result"]["started"][0].asString() == "jun");
    CHECK(until(settled));
    text = fileText("jun.prompt.txt");
    CHECK(text.find("You are Jun (@jun)") == 0 && text.find("Be brief.") != std::string::npos && text.find("[User] @Jun hi") != std::string::npos);
    CHECK(text.find("[User] hello team") != std::string::npos && text.find("[Mina] Done it.") != std::string::npos);
    CHECK(fileText("jun.args.txt").find(R"J("new" "m" "edit" "nofile" "nomcp")J") == 0);
    CHECK(Call(e, "team.send", R"J({"text":"@jun more"})J")["ok"].asBool() && until(settled));
    text = fileText("jun.prompt.txt");
    CHECK(text.find("You are Jun") == std::string::npos && text.find("[User] @jun more") != std::string::npos && text.find("@Jun hi") == std::string::npos);
    CHECK(text.find("[Jun]") == std::string::npos);  // its own replies are not repeated to it

    // A running turn: state, activity, task; a second message waits; cancel kills the process tree.
    CHECK(Call(e, "team.send", R"J({"text":"@Sloth work on the player\ndetails"})J")["result"]["started"][0].asString() == "sloth");
    CHECK(until([&] { return stateOf("sloth")["state"].asString() == "working"; }));
    Json sloth = stateOf("sloth");
    CHECK(sloth["activity"].asString() == "Reading scripts/player.lua" && sloth["task"].asString() == "@Sloth work on the player" && sloth["tools"].asInt() == 1);
    CHECK(Call(e, "team.state")["result"]["running"].asInt() == 1);
    const double revision = Call(e, "team.state")["result"]["revision"].asNumber();
    sent = Call(e, "team.send", R"J({"text":"@sloth one more thing"})J");
    CHECK(sent["result"]["started"].size() == 0 && sent["result"]["queued"][0].asString() == "sloth");
    CHECK(Call(e, "team.state")["result"]["revision"].asNumber() > revision);
    const double cancelStarted = PlatformTimeSeconds();
    Json cancelled = Call(e, "team.cancel", R"J({"id":"Sloth"})J");
    CHECK(cancelled["result"]["cancelled"].size() == 1 && cancelled["result"]["cancelled"][0].asString() == "sloth" && PlatformTimeSeconds() - cancelStarted < 5.0);
    CHECK(stateOf("sloth")["state"].asString() == "idle" && settled());  // the waiting message was dropped too
    CHECK(lastMessage()["kind"].asString() == "notice" && lastMessage()["text"].asString() == "Sloth was stopped." && lastMessage()["from"].asString() == "sloth");
    CHECK(Call(e, "team.cancel", R"J({"id":"sloth"})J")["result"]["cancelled"].size() == 0);

    // maxConcurrent: the second agent waits for a free slot.
    CHECK(Call(e, "team.settings", R"J({"maxConcurrent":1})J")["ok"].asBool());
    CHECK(Call(e, "team.send", R"J({"text":"@sloth go"})J")["result"]["started"].size() == 1);
    sent = Call(e, "team.send", R"J({"text":"@mina you too"})J");
    CHECK(sent["result"]["started"].size() == 0 && sent["result"]["queued"][0].asString() == "mina");
    CHECK(stateOf("mina")["state"].asString() == "queued" && stateOf("mina")["activity"].asString() == "Waiting for a free slot");
    CHECK(Call(e, "team.cancel", R"J({"id":"sloth"})J")["ok"].asBool());
    CHECK(until([&] { return lastMessage()["from"].asString() == "mina" && lastMessage()["kind"].asString() == "message"; }));
    CHECK(Call(e, "team.settings", R"J({"maxConcurrent":3})J")["ok"].asBool());

    // Failures: a crash (exit code and stderr), an error the CLI reports, a CLI that is not installed.
    CHECK(Call(e, "team.send", R"J({"text":"@Crash go"})J")["result"]["started"][0].asString() == "crash" && until(settled));
    reply = lastMessage();
    CHECK(reply["kind"].asString() == "error" && reply["from"].asString() == "crash" && reply["text"].asString().find("exited with code 3") != std::string::npos);
    CHECK(reply["text"].asString().find("boom") != std::string::npos && reply["hint"].asString().find("crash.cmd") != std::string::npos && !reply.has("turn"));
    CHECK(stateOf("crash")["state"].asString() == "error" && stateOf("crash")["error"].asString() == reply["text"].asString());
    CHECK(Call(e, "team.send", R"J({"text":"@denied go"})J")["ok"].asBool() && until(settled));
    CHECK(lastMessage()["kind"].asString() == "error" && lastMessage()["text"].asString() == "Not logged in");
    CHECK(ReadTextFile(JoinPath(project, ".oe/team/sessions.json"), text) && !Json::parse(text)["denied"].has("session"));
    sent = Call(e, "team.send", R"J({"text":"@Ghost and @later, are you there?"})J");
    CHECK(sent["result"]["offline"].size() == 2 && sent["result"]["started"].size() == 0 && sent["result"]["message"]["to"].size() == 2);
    CHECK(lastMessage()["kind"].asString() == "notice" && lastMessage()["text"].asString().find("Later is offline") == 0);

    // @all reaches everyone who can work; the sender is told who cannot.
    sent = Call(e, "team.send", R"J({"text":"@all status?"})J");
    CHECK(sent["result"]["started"].size() == 3 && sent["result"]["offline"].size() == 2 && sent["result"]["queued"].size() == 2);  // maxConcurrent 3 of 5
    CHECK(until([&] { return stateOf("sloth")["state"].asString() == "working"; }));
    CHECK(Call(e, "team.cancel", R"J({"all":true})J")["result"]["cancelled"].size() >= 1 && until(settled));

    // Nobody addressed; invalid calls.
    CHECK(Call(e, "team.settings", R"J({"lead":""})J")["ok"].asBool());
    sent = Call(e, "team.send", R"J({"text":"anyone?"})J");
    CHECK(sent["ok"].asBool() && sent["result"]["message"]["to"].size() == 0 && lastMessage()["text"].asString().find("Nobody was addressed") == 0);
    CHECK(lastMessage()["from"].asString() == "system");
    CHECK(code("team.send", R"J({"text":"  \n"})J") == "invalid_argument" && code("team.send", R"J({"text":"hi","from":"nobody"})J") == "unknown_agent");
    CHECK(code("team.cancel", "{}") == "invalid_argument" && code("team.cancel", R"J({"id":"mina","all":true})J") == "invalid_argument");
    CHECK(code("team.cancel", R"J({"id":"nobody"})J") == "unknown_agent" && code("team.messages", R"J({"limit":0})J") == "invalid_argument");
    Json args = Json::MakeObject();
    args["text"] = std::string(32769, 'x');
    CHECK(e.Call("team.send", args)["error"]["code"].asString() == "invalid_argument");
    // A teammate as the sender does not address itself.
    sent = Call(e, "team.send", R"J({"text":"@mina @jun review please","from":"mina"})J");
    CHECK(sent["result"]["message"]["from"].asString() == "mina" && sent["result"]["started"].size() == 1 && sent["result"]["started"][0].asString() == "jun");
    CHECK(until(settled));

    // Paging through the chat.
    const Json page = Call(e, "team.messages", R"J({"after":1,"limit":2})J")["result"];
    CHECK(page["messages"].size() == 2 && page["messages"][0]["id"].asInt() == 2 && page["messages"][1]["id"].asInt() == 3);
    const int last = page["last"].asInt();
    CHECK(page["first"].asInt() == 1);
    CHECK(last > 10 && lastMessage()["id"].asInt() == last);
    args = Json::MakeObject();
    args["after"] = last;
    CHECK(e.Call("team.messages", args)["result"]["messages"].size() == 0);

    // Attachments: a project file is referred to in place, a file from elsewhere is copied into
    // .oe/team/attachments/; the agent is told the paths.
    {
        Image picture;
        picture.width = 4;
        picture.height = 2;
        picture.rgba.assign(4 * 2 * 4, 200);
        const std::string outsideImage = JoinPath(dir, "shot one.png"), outsideText = JoinPath(dir, "notes.txt");
        CHECK(WritePng(outsideImage, picture, true) && WritePng(JoinPath(dir, "pic.png"), picture, true) && WriteTextFile(outsideText, "remember the jump"));
        args = Json::MakeObject();
        args["text"] = "@mina look at these";
        args["attachments"] = Json::MakeArray();
        for (const std::string& file : {std::string("project.json"), outsideImage, outsideText}) args["attachments"].push(file);
        sent = e.Call("team.send", args);
        CHECK(sent["ok"].asBool() && sent["result"]["message"]["attachments"].size() == 3);
        const Json attached = sent["result"]["message"]["attachments"];
        const std::string id = std::to_string(sent["result"]["message"]["id"].asInt());
        CHECK(attached[0]["type"].asString() == "file" && attached[0]["path"].asString() == "project.json" && !attached[0].has("width"));
        CHECK(attached[1]["type"].asString() == "image" && attached[1]["path"].asString() == ".oe/team/attachments/" + id + "-2-shot_one.png");
        CHECK(attached[1]["width"].asInt() == 4 && attached[1]["height"].asInt() == 2);
        CHECK(attached[2]["type"].asString() == "file" && attached[2]["path"].asString() == ".oe/team/attachments/" + id + "-3-notes.txt");
        CHECK(ReadTextFile(JoinPath(project, attached[2]["path"].asString()), text) && text == "remember the jump" && FileExists(outsideText));
        CHECK(until(settled));
        text = fileText("mina.prompt.txt");
        CHECK(text.find("[User] @mina look at these") != std::string::npos && text.find("Attached file: project.json") != std::string::npos);
        CHECK(text.find("Attached image: .oe/team/attachments/" + id + "-2-shot_one.png") != std::string::npos);
        CHECK(lastMessage()["from"].asString() == "mina" && !lastMessage().has("attachments"));  // the agent made no picture

        args["text"] = "";  // files alone are a message
        args["attachments"] = Json::MakeArray();
        args["attachments"].push("project.json");
        sent = e.Call("team.send", args);
        CHECK(sent["ok"].asBool() && sent["result"]["message"]["text"].asString() == "" && until(settled));
        args["attachments"].push(JoinPath(dir, "missing.png"));
        CHECK(e.Call("team.send", args)["error"]["code"].asString() == "not_found");
        args["attachments"] = Json::MakeArray();
        args["attachments"].push(dir);  // a folder is not a file
        CHECK(e.Call("team.send", args)["error"]["code"].asString() == "not_found");
        args["attachments"] = Json::MakeArray();
        for (int i = 0; i < 9; ++i) args["attachments"].push("project.json");
        CHECK(e.Call("team.send", args)["error"]["code"].asString() == "invalid_argument");
        CHECK(code("team.send", R"J({"text":"x","attachments":[7]})J") == "invalid_argument");
        CHECK(code("team.send", R"J({"text":"x","attachments":"project.json"})J") == "invalid_argument");

        // A picture the turn wrote into .oe/team/images/ comes back attached to the reply, once.
        CHECK(Call(e, "team.add", R"J({"name":"Painter","backend":"painter"})J")["ok"].asBool());
        CHECK(Call(e, "team.send", R"J({"text":"@painter draw"})J")["result"]["started"].size() == 1 && until(settled));
        reply = lastMessage();
        CHECK(reply["from"].asString() == "painter" && reply["attachments"].size() == 1 && reply["attachments"][0]["type"].asString() == "image");
        CHECK(reply["attachments"][0]["path"].asString() == ".oe/team/images/made.png" && reply["attachments"][0]["width"].asInt() == 4);
        CHECK(FileExists(JoinPath(project, ".oe/team/images/made.png")));
        CHECK(Call(e, "team.send", R"J({"text":"@mina and you?"})J")["ok"].asBool() && until(settled));
        CHECK(lastMessage()["from"].asString() == "mina" && !lastMessage().has("attachments"));
        CHECK(Call(e, "team.remove", R"J({"id":"painter"})J")["ok"].asBool());
    }

    // Agent to agent: a teammate mentioned in a reply gets a turn; the chain a person's message
    // starts is cut after maxHops such turns.
    {
        auto result = [](const char* answer) { return std::string(R"J({"type":"result","subtype":"success","is_error":false,"result":")J") + answer + R"J(","session_id":"s2"})J" + "\n"; };
        CHECK(WriteTextFile(JoinPath(dir, "ping.jsonl"), result("Done my part. Your turn @pong, and @ping is me.")) && WriteTextFile(JoinPath(dir, "pong.jsonl"), result("Back to @Ping.")));
        CHECK(Call(e, "team.add", R"J({"name":"Ping","backend":"ping"})J")["ok"].asBool() && Call(e, "team.add", R"J({"name":"Pong","backend":"pong"})J")["ok"].asBool());
        CHECK(Call(e, "team.settings", R"J({"maxHops":2})J")["ok"].asBool());
        const int base = lastMessage()["id"].asInt();
        sent = Call(e, "team.send", R"J({"text":"@ping start"})J");
        CHECK(sent["result"]["started"].size() == 1 && !sent["result"]["message"].has("hop"));
        CHECK(until([&] { return settled() && lastMessage()["kind"].asString() == "notice"; }));
        args = Json::MakeObject();
        args["after"] = base;
        const Json chain = e.Call("team.messages", args)["result"]["messages"];
        // person -> ping (hop 1, calls pong) -> pong (hop 2, calls ping) -> ping (hop 3: over the limit, notice)
        CHECK(chain.size() == 5);
        if (chain.size() == 5) {
            CHECK(chain[1]["from"].asString() == "ping" && chain[1]["hop"].asInt() == 1 && chain[1]["to"].size() == 2 && chain[1]["to"][1].asString() == "pong");
            CHECK(chain[2]["from"].asString() == "pong" && chain[2]["hop"].asInt() == 2 && chain[2]["to"][0].asString() == "ping");
            CHECK(chain[3]["from"].asString() == "ping" && chain[3]["hop"].asInt() == 3);
            CHECK(chain[4]["kind"].asString() == "notice" && chain[4]["text"].asString().find("Not handed on to Pong") == 0 && chain[4]["text"].asString().find("maxHops") != std::string::npos);
        }
        CHECK(fileText("pong.prompt.txt").find("[Ping] Done my part. Your turn @pong") != std::string::npos && fileText("pong.hop.txt").find("hop1h") == 0);
        CHECK(fileText("ping.hop.txt").find("hop2h") == 0);  // its second turn answered pong's hop-2 message
        // maxHops 0: agents never call each other.
        CHECK(Call(e, "team.settings", R"J({"maxHops":0})J")["ok"].asBool());
        CHECK(Call(e, "team.send", R"J({"text":"@pong once"})J")["ok"].asBool() && until([&] { return settled() && lastMessage()["kind"].asString() == "notice"; }));
        CHECK(lastMessage()["text"].asString().find("Not handed on to Ping") == 0);
        // A message an agent posts itself (team.send through the API, with its id) is that agent's,
        // whatever `from` says, and continues the chain of its running turn.
        CHECK(Call(e, "team.send", R"J({"text":"@sloth work"})J")["result"]["started"].size() == 1);
        CHECK(until([&] { return stateOf("sloth")["state"].asString() == "working"; }));
        auto posted = e.PostCall("team.send", Json::parse(R"J({"text":"@pong can you look?","from":"user"})J"), "sloth");
        e.RunPostedJobs();
        Json byAgent = posted.get();
        CHECK(byAgent["ok"].asBool() && byAgent["result"]["message"]["from"].asString() == "sloth" && byAgent["result"]["message"]["hop"].asInt() == 1);
        CHECK(byAgent["result"]["started"].size() == 0 && lastMessage()["kind"].asString() == "notice" && lastMessage()["text"].asString().find("Not handed on to Pong") == 0);
        CHECK(e.RemoteCallAgent().empty());
        CHECK(Call(e, "team.settings", R"J({"maxHops":4})J")["ok"].asBool());
        posted = e.PostCall("team.send", Json::parse(R"J({"text":"@pong now?"})J"), "sloth");
        e.RunPostedJobs();
        byAgent = posted.get();
        CHECK(byAgent["result"]["message"]["hop"].asInt() == 1 && byAgent["result"]["started"].size() == 1 && byAgent["result"]["started"][0].asString() == "pong");
        // An id that is not in the team is not an agent: the call is the person's.
        posted = e.PostCall("team.send", Json::parse(R"J({"text":"hello nobody in particular"})J"), "stranger");
        e.RunPostedJobs();
        byAgent = posted.get();
        CHECK(byAgent["result"]["message"]["from"].asString() == "user" && !byAgent["result"]["message"].has("hop"));
        CHECK(Call(e, "team.cancel", R"J({"all":true})J")["ok"].asBool() && until(settled));
        CHECK(Call(e, "team.remove", R"J({"id":"ping"})J")["ok"].asBool() && Call(e, "team.remove", R"J({"id":"pong"})J")["ok"].asBool());
    }

    // A turn that never ends is stopped at the timeout; the host's update drives turns without commands.
    team->host.turnTimeoutSeconds = 0.3;
    CHECK(Call(e, "team.send", R"J({"text":"@sloth forever"})J")["result"]["started"].size() == 1);
    const std::string chatFile = JoinPath(project, ".oe/team/chat.jsonl");
    CHECK(until([&] {
        team->update();
        return ReadTextFile(chatFile, text) && text.find("without finishing") != std::string::npos;
    }));
    CHECK(lastMessage()["kind"].asString() == "error" && lastMessage()["from"].asString() == "sloth" && stateOf("sloth")["state"].asString() == "error");
    team->host.turnTimeoutSeconds = 1800.0;

    // `wait` blocks until the reply is there (one-shot use from `oe exec`).
    sent = Call(e, "team.send", R"J({"text":"@mina quick","wait":30})J");
    CHECK(sent["result"]["replies"].size() == 1 && sent["result"]["replies"][0]["from"].asString() == "mina" && sent["result"]["state"]["running"].asInt() == 0);

    // Removing an agent stops its turn and forgets its conversation.
    CHECK(Call(e, "team.send", R"J({"text":"@sloth again"})J")["result"]["started"].size() == 1);
    CHECK(Call(e, "team.remove", R"J({"id":"sloth"})J")["ok"].asBool() && settled() && stateOf("sloth").isNull());
    CHECK(Call(e, "team.remove", R"J({"id":"mina"})J")["ok"].asBool());
    CHECK(ReadTextFile(JoinPath(project, ".oe/team/sessions.json"), text) && !Json::parse(text).has("mina") && Json::parse(text).has("jun"));

    // The chat and the conversations survive a restart; team.clear empties them.
    const size_t count = Call(e, "team.messages", R"J({"limit":500})J")["result"]["messages"].size();
    {
        Engine other;
        CHECK(other.Open(project, &error));
        RegisterTeamCommands(other.Commands(), backends);
        const Json result = Call(other, "team.messages", R"J({"limit":500})J")["result"];
        CHECK(result["messages"].size() == count && result["last"].asInt() == lastMessage()["id"].asInt());
        CHECK(result["messages"][1]["turn"]["steps"][0].asString() == "Reading scripts/player.lua");
        CHECK(Call(other, "team.send", R"J({"text":"@jun after restart","wait":30})J")["result"]["replies"].size() == 1);
        CHECK(fileText("jun.args.txt").find("\"sess-1\" ") == 0);  // resumed from sessions.json
        CHECK(Call(other, "team.clear")["ok"].asBool());
        CHECK(Call(other, "team.messages")["result"]["first"].asInt() == 0);
        CHECK(Call(other, "team.messages")["result"]["messages"].size() == 0 && !FileExists(chatFile) && !FileExists(JoinPath(project, ".oe/team/sessions.json")));
        CHECK(Call(other, "team.list")["result"]["agents"].size() == 5);  // profiles are kept
    }
    CHECK(e.UndoDepth() == 0 && !e.Dirty());
    CHECK(RemoveAll(dir));
#endif
}
#endif

TEST(AgentInstructionsInSync) {
    // AGENTS.md (read by most coding agents) and CLAUDE.md must say the same thing (docs/DESIGN.md).
    std::string agents, claude;
    CHECK(ReadTextFile(TestSourceDir() + "/AGENTS.md", agents) && ReadTextFile(TestSourceDir() + "/CLAUDE.md", claude));
    CHECK(!agents.empty() && agents == claude);
    CHECK(claude.find("docs/DESIGN.md") != std::string::npos);
}

TEST(ScriptCheckAndParams) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/FPS", &err));
    // Syntax error: line and message, nothing runs.
    Json r = e.Call("script.check", Json::parse(R"J({"path": "scripts/bad.lua", "source": "local T = {}\nfunction T:onUpdate()\n  print(1\nend\nreturn T"})J"));
    CHECK(r["ok"].asBool() && !r["result"]["ok"].asBool());
    CHECK(r["result"]["diagnostics"][0]["line"].asInt() == 4 && r["result"]["diagnostics"][0]["severity"].asString() == "error");
    // Compiles, but a global is assigned and an unknown one is read.
    r = e.Call("script.check", Json::parse(R"J({"source": "local T = {}\nfunction T:onUpdate(dt)\n  hits = 1\n  lgo.info(math.floor(dt))\n  log.info(scene.find('A'))\nend\nreturn T"})J"));
    CHECK(r["result"]["ok"].asBool() && r["result"]["warnings"].asInt() == 2);
    CHECK(r["result"]["diagnostics"][0]["line"].asInt() == 3 && r["result"]["diagnostics"][1]["line"].asInt() == 4);
    // Every sample script is clean.
    for (const char* path : {"scripts/fps_player.lua", "scripts/target.lua"}) {
        r = e.Call("script.check", Json(Json::Object{{"path", Json(path)}}));
        CHECK(r["ok"].asBool() && r["result"]["errors"].asInt() == 0 && r["result"]["warnings"].asInt() == 0);
    }
    // Params: name, type, default, options from comparisons, trailing comment.
    r = e.Call("script.params", Json::parse(R"J({"path": "scripts/target.lua"})J"));
    CHECK(r["ok"].asBool() && r["result"].size() == 5);
    const Json& move = r["result"][0];
    CHECK(move["name"].asString() == "move" && move["type"].asString() == "string" && move["default"].asString() == "none");
    CHECK(move["options"].size() == 3);
    r = e.Call("script.params", Json::parse(R"J({"source": "local R = {}\nfunction R:onStart()\n  local p = self.params\n  self.v = p.spin or {0, 45, 0} -- deg/s\n  self.on = self.params.enabled or true\nend\nfunction R:onUpdate()\n  local p = self:position()\n  print(p.x)\nend\nreturn R"})J"));
    CHECK(r["result"].size() == 2);  // p.x in another function is not a param
    CHECK(r["result"][0]["type"].asString() == "vec3" && r["result"][0]["description"].asString() == "deg/s");
    CHECK(r["result"][1]["type"].asString() == "boolean");
}

TEST(EditorMathDecompose) {
    // Gizmo edits turn a world matrix back into Transform fields.
    const Vec3 cases[][3] = {
        {{1, 2, 3}, {10, 20, 30}, {1, 1, 1}},
        {{-4, 0.5f, 7}, {-80, 135, -170}, {2, 0.5f, 3}},
        {{0, 0, 0}, {0, 90, 0}, {1, 2, 1}},  // gimbal lock
        {{0, 0, 0}, {45, -90, 10}, {1, 1, 1}},
    };
    for (const auto& c : cases) {
        Mat4 m = Mat4::TRS(c[0], c[1], c[2]);
        Vec3 p, r, sc;
        DecomposeTRS(m, p, r, sc);
        Mat4 back = Mat4::TRS(p, r, sc);
        float worst = 0;
        for (int i = 0; i < 16; ++i) worst = std::max(worst, std::fabs(back.m[i] - m.m[i]));
        CHECK(worst < 1e-4f);
        CHECK(Near(p, c[0], 1e-5f));
        CHECK(Near(sc, c[2], 1e-4f));
    }
    CHECK(std::fabs(NearestAngle(-181.0f, 179.0f) - 179.0f) < 1e-4f);
    CHECK(std::fabs(NearestAngle(350.0f, 0.0f) + 10.0f) < 1e-4f);
}

TEST(EditorCameraRays) {
    EditorCamera cam;
    cam.target = Vec3(1, 0, -2);
    cam.Orbit(30, -10);
    CHECK(Near(cam.Eye() + cam.Forward() * cam.distance, cam.target, 1e-4f));
    RenderView v = MakeLookAtView(cam.Eye(), cam.target, cam.fov, 16.0f / 9.0f);
    Vec3 o, d, hit;
    ScreenRay(v.view, v.proj, 320, 180, 640, 360, o, d);  // center pixel looks at the target
    CHECK(Length(Cross(d, cam.Forward())) < 1e-3f);
    CHECK(RayPlane(o, d, Vec3(0, 0, 0), Vec3(0, 1, 0), hit) && Near(hit, cam.target, 1e-3f));
    Vec3 eye = cam.Eye();
    cam.Look(20, 5);  // fly look keeps the eye in place
    CHECK(Near(cam.Eye(), eye, 1e-3f));
    cam.Set2D(true);
    CHECK(Near(cam.Forward(), Vec3(0, 0, -1), 1e-6f));
    // 2D grid: powers of ten, never denser than the minimum pixel spacing.
    CHECK(std::fabs(Grid2DStep(0.01f) - 0.1f) < 1e-5f);
    CHECK(std::fabs(Grid2DStep(0.1f) - 1.0f) < 1e-4f);
    CHECK(std::fabs(Grid2DStep(0.25f) - 10.0f) < 1e-3f);
    // Levels fade in from the minimum spacing to ten times that.
    CHECK(Grid2DAlpha(kGrid2DMinPixels) == 0.0f && Grid2DAlpha(kGrid2DMinPixels * 10.0f) == 1.0f);
    CHECK(Grid2DAlpha(kGrid2DMinPixels * 5.5f) > 0.45f && Grid2DAlpha(kGrid2DMinPixels * 5.5f) < 0.55f);
}

#if OE_NATIVE_EDITOR
WindowEvent KeyEvent(WindowKey key, bool down, bool ctrl = false) {
    WindowEvent e;
    e.type = WindowEvent::Type::Key;
    e.key = key;
    e.down = down;
    e.ctrl = ctrl;
    return e;
}

std::vector<WindowEvent> CtrlChord(WindowKey key) {
    return {KeyEvent(WindowKey::Control, true, true), KeyEvent(key, true, true), KeyEvent(key, false, true), KeyEvent(WindowKey::Control, false, false)};
}

// String literals in `text`, with C escapes (\" \\ \n) resolved.
std::vector<std::string> StringLiterals(const std::string& text) {
    std::vector<std::string> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string lit;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                ++i;
                lit += text[i] == 'n' ? '\n' : text[i];
            } else {
                lit += text[i];
            }
        }
        out.push_back(lit);
    }
    return out;
}

TEST(EditorTranslations) {
    CHECK(EditorCatalogProblems().empty());
    for (const std::string& p : EditorCatalogProblems()) std::printf("  catalog: %s\n", p.c_str());
    CHECK(ParseEditorLanguage("ko-KR") == EditorLanguage::Korean);
    CHECK(ParseEditorLanguage("ja_JP.UTF-8") == EditorLanguage::Japanese);
    CHECK(ParseEditorLanguage("fr") == EditorLanguage::English);
    // Every string the editor code passes to Tr()/TrId() has a translation.
    std::set<std::string> missing;
    for (const std::string& file : ListFiles(TestSourceDir() + "/engine/editor", ".cpp", false)) {
        if (file.find("EditorText.cpp") != std::string::npos) continue;
        std::string src;
        CHECK(ReadTextFile(file, src));
        for (size_t at = src.find("Tr"); at != std::string::npos; at = src.find("Tr", at + 2)) {
            bool call = src.compare(at, 3, "Tr(") == 0 || src.compare(at, 5, "TrId(") == 0;
            if (!call || (at > 0 && (std::isalnum(static_cast<unsigned char>(src[at - 1])) || src[at - 1] == '_'))) continue;
            // Arguments up to the matching ')', skipping string literals.
            size_t open = src.find('(', at), end = open;
            int depth = 0;
            for (; end < src.size(); ++end) {
                char c = src[end];
                if (c == '"') {
                    for (++end; end < src.size() && src[end] != '"'; ++end) {
                        if (src[end] == '\\') ++end;
                    }
                } else if (c == '(') {
                    ++depth;
                } else if (c == ')' && --depth == 0) {
                    break;
                }
            }
            for (const std::string& lit : StringLiterals(src.substr(open, end - open))) {
                if (!EditorCatalogHas(lit)) missing.insert(lit);
            }
        }
        // Create menu presets: {"key", "Group", "Label", ...}
        size_t presets = src.find("const Preset kPresets[]");
        if (presets != std::string::npos) {
            std::string block = src.substr(presets, src.find("};", presets) - presets);
            for (size_t line = block.find("\n    {\""); line != std::string::npos; line = block.find("\n    {\"", line + 1)) {
                std::vector<std::string> lits = StringLiterals(block.substr(line, block.find('\n', line + 1) - line));
                for (size_t k = 1; k < 3 && k < lits.size(); ++k) {
                    if (lits[k] != "2D" && lits[k] != "UI" && !EditorCatalogHas(lits[k])) missing.insert(lits[k]);
                }
            }
        }
    }
    for (const std::string& m : missing) std::printf("  no translation: %s\n", m.c_str());
    CHECK(missing.empty());
    SetEditorLanguage(EditorLanguage::Korean);
    CHECK(std::string(Tr("Save")) == "저장" && TrId("Hierarchy") == "계층###Hierarchy");
    SetEditorLanguage(EditorLanguage::Japanese);
    CHECK(std::string(Tr("Save")) == "保存" && std::string(Tr("not in the catalog")) == "not in the catalog");
    SetEditorLanguage(EditorLanguage::English);
}

TEST(NativeEditorScriptProblems) {
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("editor_scripts"), &err));
    Call(e, "script.write", R"J({"path": "scripts/warn.lua", "source": "local T = {}\nfunction T:onStart()\n  score = 0\n  lgo.info('x')\nend\nreturn T"})J");
    Call(e, "script.write", R"J({"path": "scripts/broken.lua", "source": "local T = {}\nfunction T:onStart()\n  print(1\nend\nreturn T"})J");
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    NativeEditor ed(e, nullptr, options);
    CHECK(ed.Init(&err));
    RenderTarget img;
    auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            ed.Update({}, 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(ed.DrawToImage(img));
        }
    };
    frames(3);
    ed.OpenScript("scripts/warn.lua");
    ed.OpenScript("scripts/broken.lua");
    frames(2);
    std::vector<std::string> warn = ed.ScriptProblems("scripts/warn.lua");
    CHECK(warn.size() == 2 && warn[0].rfind("warning 3:", 0) == 0 && warn[1].rfind("warning 4:", 0) == 0);
    std::vector<std::string> broken = ed.ScriptProblems("scripts/broken.lua");
    CHECK(broken.size() == 1 && broken[0].rfind("error 4:", 0) == 0);
}



TEST(NativeEditorNetworkVisibilityAndLegacyLayout) {
    std::string project = TempProject("network_panel_legacy"), error;
    Engine engine; CHECK(engine.Open(project, &error)); CHECK(!engine.NetworkEnabled());
    if (!engine.EnableGpu(nullptr, &error)) { RemoveAll(project); return; }
    const uint64_t sessions = Session::InstancesCreated(), sockets = PlatformNetSocketsCreated();
    NativeEditor::Options options; options.language = "en";
    options.layoutFile = JoinPath(project, ".oe/editor.ini");
    RenderTarget image; std::string settings;
    auto draw = [&](NativeEditor& editor, int count) {
        for (int i = 0; i < count; ++i) {
            editor.Update({}, 1280, 720, 1.0f, Engine::kFixedDt); CHECK(editor.DrawToImage(image));
        }
    };
    {
        NativeEditor editor(engine, nullptr, options); CHECK(editor.Init(&error)); draw(editor, 4);
        ImGuiWindow* network = ImGui::FindWindowByName("###Network"); CHECK(network && network->Active);
        editor.FocusNetworkPanel(); draw(editor, 2);
        CHECK(network && network->DockTabIsVisible);
        CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/network-panel-disabled.png"), image.ToImage()));
    }
    CHECK(ReadTextFile(options.layoutFile, settings));
    // Reproduce a pre-M7 layout: no Network window/key and the original 255-bit panel mask.
    std::string legacy; bool skipWindow = false;
    std::istringstream input(settings);
    for (std::string line; std::getline(input, line);) {
        if (line.compare(0, 1, "[") == 0) skipWindow = line.compare(0, 8, "[Window]") == 0 && line.find("###Network") != std::string::npos;
        if (skipWindow || line.compare(0, 13, "NetworkPanel=") == 0) continue;
        legacy += line.compare(0, 7, "Panels=") == 0 ? "Panels=255\n" : line + "\n";
    }
    CHECK(WriteTextFile(options.layoutFile, legacy));
    {
        NativeEditor editor(engine, nullptr, options); CHECK(editor.Init(&error)); draw(editor, 4);
        ImGuiWindow* network = ImGui::FindWindowByName("###Network");
        ImGuiWindow* inspector = ImGui::FindWindowByName("###Inspector");
        CHECK(network && network->Active && inspector && network->DockId == inspector->DockId);
        editor.FocusNetworkPanel(); draw(editor, 2); CHECK(network && network->DockTabIsVisible);
    }
    CHECK(ReadTextFile(options.layoutFile, settings)); CHECK(settings.find("NetworkPanel=1") != std::string::npos);
    size_t flag = settings.find("NetworkPanel=1"); if (flag != std::string::npos) settings.replace(flag, 14, "NetworkPanel=0");
    CHECK(WriteTextFile(options.layoutFile, settings));
    {
        NativeEditor editor(engine, nullptr, options); CHECK(editor.Init(&error)); draw(editor, 4);
        ImGuiWindow* network = ImGui::FindWindowByName("###Network"); CHECK(!network || !network->Active);
        editor.FocusNetworkPanel(); draw(editor, 2);
        network = ImGui::FindWindowByName("###Network"); CHECK(network && network->Active && network->DockTabIsVisible);
    }
    CHECK(Session::InstancesCreated() == sessions && PlatformNetSocketsCreated() == sockets);
    RemoveAll(project);
}

TEST(NativeEditorNetworkPreviewInputAndStop) {
    std::string project = SyncProject("editor_network", "tcp"), error;
    Engine engine; CHECK(engine.Open(project, &error)); engine.GetScene().Clear();
    CHECK(Call(engine, "entity.create", R"({"name":"Counter","components":{"Transform":{},"Script":{"path":"scripts/sync_test.lua"}}})")["ok"].asBool());
    CHECK(Call(engine, "entity.create", R"({"name":"Camera","components":{"Transform":{"position":[0,3,8],"rotation":[-15,0,0]},"Camera":{}}})")["ok"].asBool());
    if (!engine.EnableGpu(nullptr, &error)) { RemoveAll(project); return; }
    NativeEditor::Options options; options.language = "en";
    {
        NativeEditor editor(engine, nullptr, options); CHECK(editor.Init(&error)); CHECK(editor.SetNetworkPlayers(3));
        RenderTarget image;
        auto frames = [&](int count, std::vector<WindowEvent> events = {}) {
            for (int i = 0; i < count; ++i) {
                editor.Update(i == 0 ? events : std::vector<WindowEvent>(), 1280, 720, 1.0f, Engine::kFixedDt);
                CHECK(editor.DrawToImage(image));
            }
        };
        frames(4); frames(50, CtrlChord(WindowKey::P));
        CHECK(engine.LocalPeer(2)); CHECK(Call(engine, "net.state")["result"]["sync"]["state"].asString() == "running");
        CHECK(editor.SetGamePeer(1)); editor.FocusGameView(true);
        frames(30, {KeyEvent(WindowKey::W, true)});
        CHECK(engine.LocalPeer(1)->Input().IsDown("W")); CHECK(!engine.Input().IsDown("W"));
        CHECK(editor.SetGamePeer(2)); CHECK(!engine.LocalPeer(1)->Input().IsDown("W")); editor.FocusGameView(true);
        editor.WindowInput().SetAxis("LeftX", 0.575f); editor.WindowInput().down.insert("GamepadA"); frames(2);
        CHECK(engine.LocalPeer(2)->Input().IsDown("GamepadA")); CHECK(!engine.LocalPeer(1)->Input().IsDown("GamepadA"));
        editor.FocusGameView(false); CHECK(!engine.LocalPeer(2)->Input().IsDown("GamepadA"));
        CHECK(engine.GameData()["total"].asNumber() > 10);
        CHECK(Call(engine, "net.simulate", R"({"latencyFrames":2})")["ok"].asBool());
        editor.FocusNetworkPanel(); frames(2);
        CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/m7-editor-test.png"), image.ToImage()));
        frames(6, CtrlChord(WindowKey::P)); CHECK(!engine.InPlaySession()); CHECK(!engine.LocalPeer(1));
    }
    RemoveAll(project);
}

TEST(NativeEditorClipboardHistoryPrefabAndDialogs) {
    struct DialogWindow : Window {
        FileDialogResult next; FileDialogOptions last; int calls=0;
        bool PumpEvents(InputState&) override { return true; }
        void Present(const RenderTarget&) override {}
        int Width() const override { return 1280; }
        int Height() const override { return 720; }
        void SetTitle(const std::string&) override {}
        FileDialogResult ChooseFile(const FileDialogOptions& options) override { ++calls; last=options; return next; }
    } window;
    Engine e; std::string error;
    CHECK(e.Open(TempProject("editor_p7"),&error));
    CHECK(Call(e,"scene.new",R"({"empty":true})")["ok"].asBool());
    CHECK(Call(e,"entity.create",R"({"name":"Root","components":{"Transform":{},"MeshRenderer":{}}})")["ok"].asBool());
    CHECK(Call(e,"entity.create",R"({"name":"Child","parent":"Root"})")["ok"].asBool());
    CHECK(Call(e,"prefab.create",R"({"id":"Root","path":"prefabs/editor.prefab.json"})")["ok"].asBool());
    CHECK(Call(e,"scene.save",R"({"path":"scenes/original.scene.json"})")["ok"].asBool());
    if (!e.EnableGpu(nullptr,&error)) { std::printf("  SKIP P7 editor GPU (%s)\n",error.c_str()); return; }
    NativeEditor::Options options; options.language="en";
    NativeEditor editor(e,&window,options); CHECK(editor.Init(&error));
    // Restore platform callbacks on scope exit; test clipboard never touches the user's OS clipboard.
    struct ClipboardGuard {
        decltype(ImGuiPlatformIO::Platform_GetClipboardTextFn) get;
        decltype(ImGuiPlatformIO::Platform_SetClipboardTextFn) set;
        void* user; std::string text;
        ClipboardGuard() {
            auto& platform=ImGui::GetPlatformIO(); get=platform.Platform_GetClipboardTextFn;
            set=platform.Platform_SetClipboardTextFn; user=platform.Platform_ClipboardUserData;
            platform.Platform_ClipboardUserData=this;
            platform.Platform_GetClipboardTextFn=[](ImGuiContext*) {
                return static_cast<ClipboardGuard*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->text.c_str();
            };
            platform.Platform_SetClipboardTextFn=[](ImGuiContext*,const char* value) {
                static_cast<ClipboardGuard*>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->text=value;
            };
        }
        ~ClipboardGuard() {
            auto& platform=ImGui::GetPlatformIO(); platform.Platform_GetClipboardTextFn=get;
            platform.Platform_SetClipboardTextFn=set; platform.Platform_ClipboardUserData=user;
        }
    } clipboard;
    RenderTarget image;
    auto frames=[&](int count,std::vector<WindowEvent> events={}) {
        for (int frame=0;frame<count;++frame) {
            editor.Update(frame==0?events:std::vector<WindowEvent>(),1280,720,1,Engine::kFixedDt);
            CHECK(editor.DrawToImage(image));
        }
    };
    frames(4); editor.Select(e.GetScene().FindByName("Root")); frames(1);
    const size_t count=e.GetScene().Entities().size();
    frames(4,CtrlChord(WindowKey::C));
    CHECK(Json::parse(clipboard.text)["entities"].size()==2);
    frames(4,CtrlChord(WindowKey::V));
    CHECK(e.GetScene().Entities().size()==count+2);
    CHECK(editor.Selected()==e.GetScene().FindByName("Root Copy"));
    frames(4,CtrlChord(WindowKey::Z));
    CHECK(e.GetScene().Entities().size()==count);
    editor.FocusHistoryPanel(); frames(2);
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR,"build/editor-p7.png"),image.ToImage()));
    // Use the editor save workflow to refresh its dirty-state cache after undo.
    frames(4,CtrlChord(WindowKey::S)); CHECK(!e.Dirty());
    editor.EditPrefab("prefabs/editor.prefab.json"); frames(2);
    CHECK(e.EditingPrefab()=="prefabs/editor.prefab.json" && editor.Selected()!=kNullEntity);
    CHECK(Call(e,"entity.rename",R"({"id":"Child","name":"SavedChild"})")["ok"].asBool()); frames(1);
    frames(4,CtrlChord(WindowKey::S)); CHECK(!e.Dirty());
    editor.ClosePrefab(); frames(2);
    CHECK(e.EditingPrefab().empty() && e.GetScene().FindByName("Child")!=kNullEntity);
    editor.EditPrefab("prefabs/editor.prefab.json"); frames(2);
    CHECK(e.GetScene().FindByName("SavedChild")!=kNullEntity);
    CHECK(Call(e,"entity.rename",R"({"id":"SavedChild","name":"Discarded"})")["ok"].asBool()); frames(1);
    editor.ClosePrefab(); frames(2);
    CHECK(!e.EditingPrefab().empty()); // unsaved edits require the editor prompt
    auto saveModal=[]() -> ImGuiWindow* {
        for (ImGuiWindow* item : GImGui->Windows)
            if (item->Active && std::strstr(item->Name,"Save changes?") && ImGui::IsPopupOpen(item->PopupId,ImGuiPopupFlags_AnyPopupLevel))
                return item;
        return nullptr;
    };
    auto clickPrompt=[&](int button) {
        ImGuiWindow* modal=saveModal(); CHECK(modal!=nullptr);
        if (!modal) return;
        const ImGuiStyle& style=ImGui::GetStyle(); const float font=ImGui::GetFontSize();
        WindowEvent move; move.type=WindowEvent::Type::MouseMove;
        move.x=modal->Pos.x+style.WindowPadding.x+font*3+static_cast<float>(button)*(font*6+style.ItemSpacing.x);
        move.y=modal->Pos.y+modal->Size.y-style.WindowPadding.y-(font+style.FramePadding.y*2)*0.5f;
        WindowEvent click=move; click.type=WindowEvent::Type::MouseButton; click.button=0; click.down=true;
        frames(2,{move,click}); click.down=false; frames(2,{click});
    };
    clickPrompt(1);
    CHECK(e.EditingPrefab().empty());
    if (!e.EditingPrefab().empty()) { Call(e,"prefab.close",R"({"discard":true})"); frames(2,{KeyEvent(WindowKey::Escape,true),KeyEvent(WindowKey::Escape,false)}); }
    // Failed source saves keep the pending close and modal open; Cancel preserves the edit context.
    for (bool deleteRoot : {false,true}) {
        editor.EditPrefab("prefabs/editor.prefab.json"); frames(2);
        CHECK(e.EditingPrefab()=="prefabs/editor.prefab.json");
        if (deleteRoot) CHECK(Call(e,"entity.delete",R"({"id":"Root"})")["ok"].asBool());
        else CHECK(Call(e,"entity.create",R"({"name":"ExtraRoot"})")["ok"].asBool());
        frames(1); editor.ClosePrefab(); frames(2);
        CHECK(saveModal()!=nullptr);
        clickPrompt(0);
        CHECK(e.EditingPrefab()=="prefabs/editor.prefab.json" && e.Dirty());
        CHECK(saveModal()!=nullptr);
        CHECK(editor.LastNotice().find("prefab_save_failed")!=std::string::npos ||
              editor.LastNotice().find(deleteRoot?"deleted":"single prefab root")!=std::string::npos);
        clickPrompt(2);
        CHECK(saveModal()==nullptr && !e.EditingPrefab().empty());
        CHECK(Call(e,"history.undo","{}")["ok"].asBool()); frames(1);
        CHECK(e.GetScene().Entities().size()==2);
        frames(4,CtrlChord(WindowKey::S)); CHECK(!e.Dirty());
        editor.ClosePrefab(); frames(2);
        CHECK(e.EditingPrefab().empty());
    }
    window.next.status=FileDialogResult::Status::Selected;
    window.next.path=JoinPath(e.ProjectDir(),"scenes/chosen.scene.json");
    editor.BrowseFile(NativeEditor::FilePurpose::SaveScene); frames(1);
    CHECK(window.last.save && window.last.extension=="scene.json");
    CHECK(FileExists(window.next.path) && !e.Dirty());
    editor.BrowseFile(NativeEditor::FilePurpose::OpenScene); frames(2);
    CHECK(!window.last.save && editor.LastNotice().find("Opened")!=std::string::npos);
    const std::string scene=e.GetScene().ToJson().dump(); const uint64_t revision=e.Revision();
    window.next.status=FileDialogResult::Status::Cancelled;
    for (auto purpose : {NativeEditor::FilePurpose::OpenScene,NativeEditor::FilePurpose::SaveScene,NativeEditor::FilePurpose::ImportAsset})
        editor.BrowseFile(purpose);
    CHECK(e.GetScene().ToJson().dump()==scene && e.Revision()==revision);
    window.next.status=FileDialogResult::Status::Error; window.next.error="Injected chooser failure";
    editor.BrowseFile(NativeEditor::FilePurpose::OpenScene); CHECK(editor.LastNotice()==window.next.error);
    window.next.status=FileDialogResult::Status::Unavailable;
    editor.BrowseFile(NativeEditor::FilePurpose::OpenScene); CHECK(editor.LastNotice().find("unavailable")!=std::string::npos);
    window.next.status=FileDialogResult::Status::Selected; window.next.path=AbsolutePath("build/outside.scene.json");
    editor.BrowseFile(NativeEditor::FilePurpose::SaveScene);
    CHECK(e.GetScene().ToJson().dump()==scene && e.Revision()==revision);
    Image imported; imported.width=imported.height=2; imported.rgba.assign(16,255);
    CHECK(WritePng(AbsolutePath("build/editor-p7-import.png"),imported));
    window.next.path=AbsolutePath("build/editor-p7-import.png");
    editor.BrowseFile(NativeEditor::FilePurpose::ImportAsset); frames(1);
    CHECK(Call(e,"asset.list",R"({"kind":"texture"})")["result"].size()>0);
    CHECK(e.GetScene().ToJson().dump()==scene && e.Revision()==revision);
    std::filesystem::remove(window.next.path);
}

#if OE_TEAM
TEST(EditorAvatarPresets) {
    // Every preset name the team offers has a picture embedded in the editor.
    CHECK(!TeamAvatarPresets().empty());
    for (const std::string& preset : TeamAvatarPresets()) {
        const unsigned char* data = nullptr;
        size_t size = 0;
        Texture texture;
        std::string error;
        CHECK(EditorAvatarPreset(preset, &data, &size) && data && size > 100);
        CHECK(data && DecodeImage(data, size, texture, &error) && texture.width == 128 && texture.height == 128);
        CHECK(FileExists(TestSourceDir() + "/engine/editor/avatars/" + preset + ".png"));
    }
    CHECK(!EditorAvatarPreset("dragon", nullptr, nullptr) && !EditorAvatarPreset("", nullptr, nullptr));
}

TEST(NativeEditorTeam) {
    const std::string project = TempProject("editor_team");
    const std::string dir = ProcessFixtureDir("editor team");
    std::string error;
    // Stand-in CLIs: one installed (a script that prints a version), one that is not on this PC.
    Backend good;
    good.id = "good";
    good.displayName = "Good CLI";
    good.executable = JoinPath(dir, "good.cmd");
    good.installHint = "npm install -g good";
    good.models = {{"m1", "Model One", true, 0}, {"m2", "Model Two", false, 0}};
    good.supportsImages = true;
    good.buildTurn = [](const TurnRequest& request) {
        ProcessOptions options;
        options.executable = request.executable;
        return options;
    };
    good.parseLine = &ParseClaudeLine;
    CHECK(WriteTextFile(good.executable, "@echo off\necho good-cli 1.2.3\n"));
    Backend missing = good;
    missing.id = "missing";
    missing.displayName = "Missing CLI";
    missing.executable = "oe-no-such-agent-cli";
    missing.models = {{"x", "X", true, 0}};

    Engine e;
    CHECK(e.Open(project, &error));
    std::shared_ptr<TeamHandle> team = RegisterTeamCommands(e.Commands(), {good, missing});
    CHECK(Call(e, "team.add", R"J({"name":"Mina","backend":"good","description":"Gameplay programmer."})J")["ok"].asBool());
    CHECK(Call(e, "team.add", R"J({"name":"Sora","backend":"missing"})J")["ok"].asBool());
    if (!e.EnableGpu(nullptr, &error)) {
        std::printf("  SKIP editor team (%s)\n", error.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    int ticks = 0;
    options.onFrame = [&] {
        ++ticks;
        team->update();
    };
    NativeEditor editor(e, nullptr, options);
    CHECK(editor.Init(&error));
    RenderTarget image;
    auto frames = [&](int count, std::vector<WindowEvent> events = {}) {
        for (int frame = 0; frame < count; ++frame) {
            editor.Update(frame == 0 ? events : std::vector<WindowEvent>(), 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(editor.DrawToImage(image));
        }
    };
    auto typed = [](const char* text) {
        std::vector<WindowEvent> events;
        for (const char* c = text; *c; ++c) {
            WindowEvent event;
            event.type = WindowEvent::Type::Text;
            event.codepoint = static_cast<uint32_t>(static_cast<unsigned char>(*c));
            events.push_back(event);
        }
        return events;
    };
    const std::vector<WindowEvent> enter = {KeyEvent(WindowKey::Enter, true), KeyEvent(WindowKey::Enter, false)};
    const std::vector<WindowEvent> escape = {KeyEvent(WindowKey::Escape, true), KeyEvent(WindowKey::Escape, false)};
    auto modal = [](const char* id) -> ImGuiWindow* {
        for (ImGuiWindow* window : GImGui->Windows) {
            if (window->Active && std::strstr(window->Name, id) && ImGui::IsPopupOpen(window->PopupId, ImGuiPopupFlags_AnyPopupLevel)) return window;
        }
        return nullptr;
    };
    auto agents = [&] { return Call(e, "team.list")["result"]["agents"]; };

    // The Team tab waits behind the Hierarchy; the editor's frame keeps the team updated.
    frames(4);
    ImGuiWindow* panel = ImGui::FindWindowByName("###Team");
    ImGuiWindow* hierarchy = ImGui::FindWindowByName("###Hierarchy");
    CHECK(panel && hierarchy && panel->DockId == hierarchy->DockId && hierarchy->DockTabIsVisible && ticks >= 4);
    editor.FocusTeamPanel();
    frames(3);
    panel = ImGui::FindWindowByName("###Team");
    CHECK(panel && panel->Active && panel->DockTabIsVisible);
#if defined(_WIN32)
    // Detection runs in the background while frames keep coming.
    // (A busy machine can make the stand-in's first start fail or time out: ask again then.)
    const double end = PlatformTimeSeconds() + 60.0;
    for (std::string status = "detecting"; PlatformTimeSeconds() < end && status != "installed";) {
        status = Call(e, "team.backends", status == "detecting" ? R"({"async":true})" : R"({"async":true,"refresh":true})")["result"][0]["status"].asString();
        frames(1);
        PlatformSleep(0.02);
    }
    frames(20);  // the panel polls every quarter second of editor time
    const Json detected = Call(e, "team.backends", R"({"async":true})")["result"][0];
    CHECK(detected["status"].asString() == "installed");
    if (detected["status"].asString() != "installed") std::printf("  backend: %s\n", detected.dump().c_str());

    // Add an agent through the dialog: the name field has the focus, Enter saves.
    editor.EditAgent("");
    frames(3);
    CHECK(modal("###Agent") != nullptr);
    CHECK(!editor.SetAgentBackend("missing"));  // a CLI that is not installed cannot be chosen
    CHECK(!editor.SetAgentBackend("nobody") && editor.SetAgentBackend("good"));
    frames(2, typed("Kai"));
    frames(3, enter);
    CHECK(modal("###Agent") == nullptr && agents().size() == 3);
    Json kai = Call(e, "team.get", R"J({"id":"kai"})J")["result"];
    CHECK(kai["name"].asString() == "Kai" && kai["backend"].asString() == "good" && kai["model"].asString() == "m1" && kai["access"].asString() == "edit");
    CHECK(kai["avatar"].asString() == "preset:fox");

    // A refused save (the name is taken) keeps the dialog open; Escape closes it without saving.
    editor.EditAgent("");
    frames(3);
    frames(2, typed("mina"));
    frames(3, enter);
    CHECK(modal("###Agent") != nullptr && agents().size() == 3);
    frames(3, escape);
    CHECK(modal("###Agent") == nullptr && agents().size() == 3);

    // Editing keeps what was not touched.
    editor.EditAgent("mina");
    frames(3);
    CHECK(modal("###Agent") != nullptr);
    frames(3, enter);
    CHECK(modal("###Agent") == nullptr);
    const Json mina = Call(e, "team.get", R"J({"id":"mina"})J")["result"];
    CHECK(mina["name"].asString() == "Mina" && mina["description"].asString() == "Gameplay programmer." && mina["model"].asString() == "m1");

    // Deleting asks first.
    editor.RemoveAgent("kai");
    frames(3);
    CHECK(modal("###Remove Agent") != nullptr && agents().size() == 3);
    frames(3, escape);
    CHECK(modal("###Remove Agent") == nullptr && agents().size() == 3);
    editor.RemoveAgent("kai");
    frames(3);
    frames(3, enter);
    CHECK(modal("###Remove Agent") == nullptr && agents().size() == 2 && Call(e, "team.get", R"J({"id":"kai"})J")["error"]["code"].asString() == "unknown_agent");
#endif

    // An API call that names a team agent is reported under the agent's name; others as "API".
    {
        std::vector<std::string> seen;
        auto posted = e.PostCall("entity.create", Json::parse(R"J({"name":"ByAgent"})J"), "mina");
        e.RunPostedJobs();
        CHECK(posted.get()["ok"].asBool() && editor.LastNotice().find("Mina: entity.create") == 0);
        posted = e.PostCall("entity.create", Json::parse(R"J({"name":"ByApi"})J"));
        e.RunPostedJobs();
        CHECK(posted.get()["ok"].asBool() && editor.LastNotice().find("API: entity.create") == 0 && e.RemoteCallAgent().empty());
        posted = e.PostCall("entity.create", Json::parse(R"J({"name":"ByStranger"})J"), "nobody");  // not in the team: no name to show
        e.RunPostedJobs();
        CHECK(posted.get()["ok"].asBool() && editor.LastNotice().find("API: entity.create") == 0);
        frames(2);
        CHECK(Call(e, "history.undo")["ok"].asBool() && Call(e, "history.undo")["ok"].asBool() && Call(e, "history.undo")["ok"].asBool());
        CHECK(Call(e, "scene.save")["ok"].asBool());
        frames(2);
    }

    // Looked at by a person or an agent: the panel in each language, and the profile dialog.
    for (EditorLanguage language : {EditorLanguage::Korean, EditorLanguage::Japanese, EditorLanguage::English}) {
        SetEditorLanguage(language);
        frames(3);
        panel = ImGui::FindWindowByName("###Team");
        CHECK(panel && panel->Active);
        CHECK(WritePng(JoinPath(OE_SOURCE_DIR, std::string("build/editor-team-") + EditorLanguageCode(language) + ".png"), image.ToImage()));
    }
    editor.EditAgent("mina");
    frames(4);
    CHECK(modal("###Agent") != nullptr);
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/editor-team-dialog.png"), image.ToImage()));
    frames(3, escape);
    CHECK(e.UndoDepth() == 0 && !e.Dirty());  // managing the team is not a scene edit
    CHECK(RemoveAll(dir));
}
TEST(NativeEditorTeamChat) {
    if (!PlatformProcessSupported()) return;
#if defined(_WIN32)
    const std::string project = TempProject("editor_chat");
    const std::string dir = ProcessFixtureDir("editor chat");
    std::string error;
    // Stand-in CLIs as in TeamSessionTurns: one answers at once, one works until it is stopped.
    auto fake = [&](const char* id, const std::string& script) {
        Backend backend;
        backend.id = id;
        backend.displayName = std::string("Fake ") + id;
        backend.executable = JoinPath(dir, std::string(id) + ".cmd");
        backend.installHint = "npm install -g fake";
        backend.models = {{"m", "M", true, 0}};
        backend.buildTurn = [](const TurnRequest& request) {
            ProcessOptions options;
            options.executable = request.executable;
            return options;
        };
        backend.parseLine = &ParseClaudeLine;
        CHECK(WriteTextFile(backend.executable, "@echo off\n" + script));
        return backend;
    };
    const std::string toolLine = R"J({"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Read","input":{"file_path":")J" + project + R"J(/scripts/player.lua"}}]}})J";
    CHECK(WriteTextFile(JoinPath(dir, "tool.jsonl"), toolLine + "\n"));
    CHECK(WriteTextFile(JoinPath(dir, "reply.jsonl"), toolLine + "\n" +
                                                         R"J({"type":"result","subtype":"success","is_error":false,"result":"Done. See scenes/main.scene.json:\n```lua\nlocal x = 1\n```","session_id":"s1"})J" + "\n"));
    Engine e;
    CHECK(e.Open(project, &error));
    std::shared_ptr<TeamHandle> team = RegisterTeamCommands(
        e.Commands(), {fake("echo", "findstr \"^\" >nul\ntype \"%~dp0reply.jsonl\"\n"), fake("slow", "findstr \"^\" >nul\ntype \"%~dp0tool.jsonl\"\nping -n 30 127.0.0.1 >nul\n")});
    CHECK(Call(e, "team.add", R"J({"name":"Mina","backend":"echo"})J")["ok"].asBool());
    CHECK(Call(e, "team.add", R"J({"name":"Sloth","backend":"slow"})J")["ok"].asBool());
    if (!e.EnableGpu(nullptr, &error)) {
        std::printf("  SKIP editor chat (%s)\n", error.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    options.onFrame = team->update;
    NativeEditor editor(e, nullptr, options);
    CHECK(editor.Init(&error));
    RenderTarget image;
    auto frames = [&](int count, std::vector<WindowEvent> events = {}) {
        for (int frame = 0; frame < count; ++frame) {
            editor.Update(frame == 0 ? events : std::vector<WindowEvent>(), 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(editor.DrawToImage(image));
        }
    };
    auto typed = [](const char* text) {
        std::vector<WindowEvent> events;
        for (const char* c = text; *c; ++c) {
            WindowEvent event;
            event.type = WindowEvent::Type::Text;
            event.codepoint = static_cast<uint32_t>(static_cast<unsigned char>(*c));
            events.push_back(event);
        }
        return events;
    };
    auto key = [](WindowKey which, bool down, bool shift = false) {
        WindowEvent event = KeyEvent(which, down);
        event.shift = shift;
        return event;
    };
    const std::vector<WindowEvent> enter = {KeyEvent(WindowKey::Enter, true), KeyEvent(WindowKey::Enter, false)};
    auto messages = [&] { return Call(e, "team.messages", R"J({"limit":50})J")["result"]["messages"]; };
    auto lastMessage = [&] {
        const Json all = messages();
        return all.size() ? all[all.size() - 1] : Json();
    };
    auto until = [&](const std::function<bool()>& done) {
        const double end = PlatformTimeSeconds() + 30.0;
        while (PlatformTimeSeconds() < end && !done()) {
            frames(1);  // the editor's frame is what advances the turn
            PlatformSleep(0.01);
        }
        return done();
    };
    auto stateOf = [&](const char* id) {
        const Json state = Call(e, "team.state")["result"];
        for (const Json& agent : state["agents"].items()) {
            if (agent["id"].asString() == id) return agent["state"].asString();
        }
        return std::string();
    };

    // The chat tab waits beside the Console; focusing it gives the input the keyboard.
    frames(4);
    ImGuiWindow* chat = ImGui::FindWindowByName("###Team Chat");
    ImGuiWindow* console = ImGui::FindWindowByName("###Console");
    CHECK(chat && console && chat->DockId == console->DockId);
    editor.FocusTeamChat();
    frames(3);
    chat = ImGui::FindWindowByName("###Team Chat");
    CHECK(chat && chat->Active && chat->DockTabIsVisible);

    // Tab completes the mention being typed; Enter sends; the reply arrives while frames run.
    frames(2, typed("@mi"));
    CHECK(editor.TeamChatInput() == "@mi");
    frames(2, {KeyEvent(WindowKey::Tab, true), KeyEvent(WindowKey::Tab, false)});
    CHECK(editor.TeamChatInput() == "@mina ");
    frames(2, typed("hello"));
    frames(3, enter);
    CHECK(editor.TeamChatInput().empty());
    CHECK(messages().size() >= 1 && messages()[0]["text"].asString() == "@mina hello" && messages()[0]["to"][0].asString() == "mina");
    CHECK(until([&] { return lastMessage()["from"].asString() == "mina"; }));
    CHECK(lastMessage()["text"].asString().find("Done.") == 0 && lastMessage()["turn"]["steps"].size() == 1);
    frames(3);

    // Shift+Enter is a line break, not a send. The input keeps the keyboard after a send.
    frames(2, typed("one"));
    frames(1, {key(WindowKey::Shift, true, true)});
    frames(2, {key(WindowKey::Enter, true, true), key(WindowKey::Enter, false, true)});
    frames(1, {key(WindowKey::Shift, false)});
    CHECK(editor.TeamChatInput() == "one\n");
    frames(2, typed("two"));
    const size_t before = messages().size();
    frames(3, enter);
    CHECK(until([&] { return messages().size() >= before + 2; }));  // sent (to the lead) and answered
    CHECK(messages()[before]["text"].asString() == "one\ntwo" && messages()[before]["from"].asString() == "user");
    frames(2, enter);  // an empty input sends nothing
    frames(2);
    CHECK(messages().size() == before + 2);

    // A running turn has a live line with Stop; a real click on it cancels the turn.
    frames(2, typed("@sloth go"));
    frames(3, enter);
    CHECK(until([&] { return stateOf("sloth") == "working" && editor.TeamChatStopRect()[2] > 0.0f; }));
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/editor-team-chat-en.png"), image.ToImage()));
    const std::array<float, 4> stop = editor.TeamChatStopRect();
    WindowEvent move;
    move.type = WindowEvent::Type::MouseMove;
    move.x = stop[0] + stop[2] * 0.5f;
    move.y = stop[1] + stop[3] * 0.5f;
    WindowEvent click = move;
    click.type = WindowEvent::Type::MouseButton;
    click.button = 0;
    click.down = true;
    frames(2, {move});
    frames(2, {click});
    click.down = false;
    frames(2, {click});
    CHECK(until([&] { return stateOf("sloth") == "idle"; }));
    CHECK(lastMessage()["kind"].asString() == "notice" && lastMessage()["text"].asString() == "Sloth was stopped.");
    frames(2);
    CHECK(editor.TeamChatStopRect()[2] == 0.0f);

    // Messages that arrive while the tab is hidden are counted in its title.
    editor.FocusHistoryPanel();
    frames(3);
    chat = ImGui::FindWindowByName("###Team Chat");
    CHECK(chat && !chat->DockTabIsVisible);
    CHECK(Call(e, "team.send", R"J({"text":"@mina status?"})J")["ok"].asBool());
    CHECK(until([&] { return lastMessage()["from"].asString() == "mina"; }));
    frames(3);
    chat = ImGui::FindWindowByName("###Team Chat");
    CHECK(chat && std::strstr(chat->Name, "(1)") != nullptr);
    editor.FocusTeamChat();
    frames(3);
    chat = ImGui::FindWindowByName("###Team Chat");
    CHECK(chat && chat->DockTabIsVisible && std::strstr(chat->Name, "(1)") == nullptr);

    // A file dropped on the chat is attached to the next message; one dropped elsewhere is imported as before.
    Image picture;
    picture.width = 8;
    picture.height = 6;
    picture.rgba.assign(8 * 6 * 4, 255);
    for (size_t i = 0; i < picture.rgba.size(); i += 4) picture.rgba[i + 1] = picture.rgba[i + 2] = 40;  // red
    const std::string shot = JoinPath(dir, "shot.png");
    CHECK(WritePng(shot, picture, true));
    const std::array<float, 4> panel = editor.TeamChatRect();
    CHECK(panel[2] > 100.0f && panel[3] > 50.0f);
    WindowEvent drop;
    drop.type = WindowEvent::Type::DropFile;
    drop.path = shot;
    drop.x = panel[0] + panel[2] * 0.5f;
    drop.y = panel[1] + panel[3] * 0.5f;
    frames(3, {drop});
    CHECK(editor.TeamChatAttachments() == std::vector<std::string>{shot});
    frames(2, {drop});  // the same file again: still one
    CHECK(editor.TeamChatAttachments().size() == 1 && !FileExists(JoinPath(project, "assets/textures/shot.png")));
    frames(2, typed("@mina what is this?"));
    const size_t beforeAttach = messages().size();
    frames(3, enter);
    CHECK(editor.TeamChatAttachments().empty() && editor.TeamChatInput().empty());
    CHECK(messages().size() > beforeAttach && messages()[beforeAttach]["attachments"].size() == 1);
    const std::string attachedPath = messages()[beforeAttach]["attachments"][0]["path"].asString();
    CHECK(attachedPath.find(".oe/team/attachments/") == 0 && messages()[beforeAttach]["attachments"][0]["width"].asInt() == 8);
    CHECK(until([&] { return lastMessage()["from"].asString() == "mina"; }));
    frames(3);
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/editor-team-chat-attachment.png"), image.ToImage()));
    drop.x = 600.0f;  // the Scene view
    drop.y = 250.0f;
    frames(3, {drop});
    CHECK(editor.TeamChatAttachments().empty() && editor.LastNotice().find("shot.png") != std::string::npos);

    // # completes a project file (Tab), as @ completes an agent.
    editor.FocusTeamChat();
    frames(3);
    frames(2, typed("open #main.sc"));
    frames(2, {KeyEvent(WindowKey::Tab, true), KeyEvent(WindowKey::Tab, false)});
    CHECK(editor.TeamChatInput() == "open scenes/main.scene.json ");
    // Up / Down move the choice in the list (wrapping), Tab takes it; the caret stays put.
    const std::vector<WindowEvent> down = {KeyEvent(WindowKey::Down, true), KeyEvent(WindowKey::Down, false)};
    const std::vector<WindowEvent> up = {KeyEvent(WindowKey::Up, true), KeyEvent(WindowKey::Up, false)};
    const std::vector<WindowEvent> tab = {KeyEvent(WindowKey::Tab, true), KeyEvent(WindowKey::Tab, false)};
    frames(2, typed("@"));  // Mina, Sloth, all
    frames(2, down);
    frames(2, tab);
    CHECK(editor.TeamChatInput() == "open scenes/main.scene.json @sloth ");
    frames(2, typed("@"));
    frames(2, up);  // from the first entry up: the last one
    frames(2, tab);
    CHECK(editor.TeamChatInput() == "open scenes/main.scene.json @sloth @all ");
    const Json files = Call(e, "asset.list")["result"];
    CHECK(files.size() > 9);  // more than the eight rows that show: the list scrolls
    frames(2, typed("#"));
    for (int i = 0; i < 9; ++i) frames(2, down);
    frames(2);
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/editor-team-chat-files.png"), image.ToImage()));
    frames(2, tab);
    CHECK(files.size() > 9 && editor.TeamChatInput() == "open scenes/main.scene.json @sloth @all " + files[9]["path"].asString() + " ");
    frames(2);
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/editor-team-chat-complete.png"), image.ToImage()));  // references tinted in the input
    frames(3, enter);
    CHECK(until([&] { return lastMessage()["from"].asString() == "mina" && stateOf("mina") == "idle"; }));
    CHECK(Call(e, "team.cancel", R"J({"all":true})J")["ok"].asBool());  // @all also called the slow one
    frames(3);
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/editor-team-chat-attachment.png"), image.ToImage()));  // and in the sent message

    // The picture viewer opens on an attachment and closes with Escape.
    editor.ViewChatImage(attachedPath);
    frames(3);
    ImGuiWindow* viewer = ImGui::FindWindowByName("###Image");
    CHECK(viewer && viewer->Active && editor.ViewedChatImage() == attachedPath);
    CHECK(WritePng(JoinPath(OE_SOURCE_DIR, "build/editor-team-chat-viewer.png"), image.ToImage()));
    frames(3, {KeyEvent(WindowKey::Escape, true), KeyEvent(WindowKey::Escape, false)});
    CHECK(editor.ViewedChatImage().empty());

    // team.clear empties the panel's copy too (checked through what it draws next: no crash, new ids).
    CHECK(Call(e, "team.clear")["ok"].asBool());
    frames(3);
    CHECK(messages().size() == 0);
    for (EditorLanguage language : {EditorLanguage::Korean, EditorLanguage::Japanese, EditorLanguage::English}) {
        SetEditorLanguage(language);
        if (language == EditorLanguage::Korean) {
            CHECK(Call(e, "team.send", R"J({"text":"@mina show me"})J")["ok"].asBool());
            CHECK(until([&] { return lastMessage()["from"].asString() == "mina"; }));
        }
        frames(3);
        if (language != EditorLanguage::English) {
            CHECK(WritePng(JoinPath(OE_SOURCE_DIR, std::string("build/editor-team-chat-") + EditorLanguageCode(language) + ".png"), image.ToImage()));
        }
    }
    CHECK(e.UndoDepth() == 0);
    CHECK(RemoveAll(dir));
#endif
}
#endif

TEST(NativeEditorHeadless) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Hello", &err));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    NativeEditor ed(e, nullptr, options);
    CHECK(ed.Init(&err));
    RenderTarget img;
    // Events are queued: Dear ImGui applies at most one press/release of a key per frame.
    auto frames = [&](int n, std::vector<WindowEvent> events = {}) {
        for (int i = 0; i < n; ++i) {
            ed.Update(i == 0 ? events : std::vector<WindowEvent>(), 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(ed.DrawToImage(img));
        }
    };
    frames(4);
    CHECK(img.width == 1280 && img.height == 720);
    std::set<uint32_t> colors;
    for (size_t i = 0; i < img.color.size(); i += 101) colors.insert(img.color[i]);
    CHECK(colors.size() > 40);  // panels, text and the rendered scene, not a blank frame

    Scene& s = e.GetScene();
    EntityId player = s.FindByName("Player");
    ed.Select(player);
    frames(1);
    CHECK(ed.Selected() == player);
    Vec3 start = s.Get<Transform>(player)->position;

    // Delete goes through entity.delete, Ctrl+Z through history.undo.
    frames(3, {KeyEvent(WindowKey::Delete, true), KeyEvent(WindowKey::Delete, false)});
    CHECK(!s.Exists(player));
    frames(6, CtrlChord(WindowKey::Z));
    CHECK(s.Exists(player));

    // Edits arriving through the API (an agent via HTTP/MCP) are announced.
    auto posted = e.PostCall("component.set", Json::parse(R"J({"id": "Player", "type": "MeshRenderer", "values": {"color": [1, 0, 0]}})J"));
    e.RunPostedJobs();
    CHECK(posted.get()["ok"].asBool());
    frames(1);
    CHECK(ed.LastNotice().rfind("API: component.set Player", 0) == 0);

    // Ctrl+P plays and gives the Game view the keyboard: W walks the player forward.
    frames(6, CtrlChord(WindowKey::P));
    CHECK(e.InPlaySession());
    CHECK(ed.GameViewFocused());
    ed.WindowInput().SetAxis("LeftX", 0.575f);
    ed.WindowInput().down.insert("GamepadA");
    frames(1);
    CHECK(std::fabs(e.Input().Axis("LeftX") - 0.5f) < 1e-6f && e.Input().IsDown("GamepadA"));
    ed.WindowInput().SetAxis("LeftX", 0);
    ed.WindowInput().down.erase("GamepadA");
    frames(1);
    CHECK(e.Input().Axis("LeftX") == 0 && !e.Input().IsDown("GamepadA"));
    frames(30, {KeyEvent(WindowKey::W, true)});
    frames(1, {KeyEvent(WindowKey::W, false)});
    CHECK(s.Exists(player) && s.Get<Transform>(player)->position.z < start.z - 0.5f);

    ed.WindowInput().SetAxis("RT", 1);
    ed.WindowInput().down.insert("GamepadB");
    frames(1);
    CHECK(e.Input().Axis("RT") == 1 && e.Input().IsDown("GamepadB"));
    WindowEvent focus;
    focus.type = WindowEvent::Type::Focus;
    focus.down = false;
    frames(1, {focus});
    CHECK(!ed.GameViewFocused() && e.Input().Axis("RT") == 0 && !e.Input().IsDown("GamepadB"));
    // A stale window snapshot cannot re-press buttons while the Game view is unfocused.
    frames(1);
    CHECK(!e.Input().IsDown("GamepadB"));
    ed.WindowInput().axes.clear();
    ed.WindowInput().down.clear();
    focus.down = true;
    frames(2, {focus});

    // Ctrl+P again stops and restores the edit-time scene.
    frames(6, CtrlChord(WindowKey::P));
    CHECK(!e.InPlaySession());
    player = s.FindByName("Player");
    CHECK(player != kNullEntity && Near(s.Get<Transform>(player)->position, start, 1e-5f));
    CHECK(!ed.GameViewFocused());

    // Closing the window with unsaved edits asks first instead of quitting.
    WindowEvent close;
    close.type = WindowEvent::Type::Close;
    frames(2, {close});
    CHECK(!ed.QuitRequested());
}

TEST(NativeEditorUIEditing) {
    // The stopped Game view edits UI in place: click selects, drag moves, handles resize, arrows nudge.
    Engine e;
    std::string err;
    CHECK(e.Open(TempProject("editor_ui"), &err));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    Call(e, "scene.new", R"J({"empty":true})J");
    Call(e, "entity.create", R"J({"name":"Camera","components":{"Camera":{}}})J");
    Call(e, "entity.create", R"J({"name":"Back","components":{"UIPanel":{"anchor":"stretch","x":0,"y":0,"width":0,"height":0,"opacity":0.2}}})J");
    Call(e, "entity.create", R"J({"name":"Play","parent":"Back","components":{"UIButton":{"text":"Play","anchor":"top-left","x":200,"y":150,"width":240,"height":80}}})J");
    Call(e, "entity.create", R"J({"name":"Corner","parent":"Back","components":{"UIPanel":{"anchor":"bottom-right","x":-40,"y":-40,"width":100,"height":60,"opacity":1}}})J");
    NativeEditor::Options options;
    options.language = "en";
    NativeEditor ed(e, nullptr, options);
    CHECK(ed.Init(&err));
    RenderTarget img;
    auto frames = [&](int n, std::vector<WindowEvent> events = {}) {
        for (int i = 0; i < n; ++i) {
            ed.Update(i == 0 ? events : std::vector<WindowEvent>(), 1600, 900, 1.0f, Engine::kFixedDt);
            CHECK(ed.DrawToImage(img));
        }
    };
    auto mouse = [](WindowEvent::Type type, float x, float y, int button = 0, bool down = false) {
        WindowEvent ev;
        ev.type = type;
        ev.x = x;
        ev.y = y;
        ev.button = button;
        ev.down = down;
        return ev;
    };
    frames(3);
    ed.FocusGameView(true);
    frames(4);
    const std::array<float, 4> view = ed.GameViewRect();
    CHECK(view[2] > 200 && view[3] > 100);
    const float k = view[3] / 720.0f;  // window pixels per reference pixel
    Scene& s = e.GetScene();
    const EntityId play = s.FindByName("Play"), corner = s.FindByName("Corner");
    auto at = [&](float rx, float ry) { return std::pair<float, float>(view[0] + rx * k, view[1] + ry * k); };
    auto drag = [&](std::pair<float, float> from, float dx, float dy) {
        frames(2, {mouse(WindowEvent::Type::MouseMove, from.first, from.second)});
        frames(2, {mouse(WindowEvent::Type::MouseButton, from.first, from.second, 0, true)});
        for (int i = 1; i <= 4; ++i) frames(1, {mouse(WindowEvent::Type::MouseMove, from.first + dx * static_cast<float>(i) / 4, from.second + dy * static_cast<float>(i) / 4)});
        frames(2, {mouse(WindowEvent::Type::MouseButton, from.first + dx, from.second + dy, 0, false)});
    };
    const size_t history = Call(e, "history.list", "{}")["result"]["entries"].size();
    // Click selects the topmost element under the pointer; dragging it moves it in reference pixels.
    drag(at(320, 190), 60 * k, 30 * k);
    CHECK(ed.Selected() == play);
    const UIButton* button = s.Get<UIButton>(play);
    CHECK(std::fabs(button->x - 260) <= 1 && std::fabs(button->y - 180) <= 1);
    CHECK(Call(e, "history.list", "{}")["result"]["entries"].size() == history + 1);  // the whole drag is one undo step
    // The bottom-right handle resizes; the top-left corner stays where it is.
    drag(at(260 + 240, 180 + 80), 40 * k, 20 * k);
    CHECK(std::fabs(button->width - 280) <= 1 && std::fabs(button->height - 100) <= 1);
    CHECK(std::fabs(button->x - 260) <= 1 && std::fabs(button->y - 180) <= 1);
    // A bottom-right anchored element keeps its right edge when its left handle moves.
    const float cx = 1280.0f * (view[2] / (1280.0f * k)) ;  // reference width of this view
    (void)cx;
    const float refW = view[2] / k;
    drag(at(refW - 40 - 50, 720 - 40 - 30), 0, 0);  // select it
    CHECK(ed.Selected() == corner);
    drag(at(refW - 40 - 100, 720 - 40 - 30), -30 * k, 0);  // left edge handle
    const UIPanel* panel = s.Get<UIPanel>(corner);
    CHECK(std::fabs(panel->width - 130) <= 1 && std::fabs(panel->x - (-40)) <= 1);
    // Undo walks back through the drags.
    CHECK(Call(e, "history.undo", "{}")["ok"].asBool());
    CHECK(std::fabs(s.Get<UIPanel>(corner)->width - 100) <= 1);
    // Clicking empty space (the transparent backdrop is an element too) selects what is there.
    drag(at(640, 600), 0, 0);
    CHECK(ed.Selected() == s.FindByName("Back"));
}

TEST(NativeEditorTilePainting) {
    Engine e;
    std::string err;
    CHECK(e.Open(TestSourceDir() + "/samples/Dungeon", &err));
    if (!e.EnableGpu(nullptr, &err)) {
        std::printf("  SKIP no GPU backend here (%s)\n", err.c_str());
        return;
    }
    NativeEditor::Options options;
    options.language = "en";
    NativeEditor ed(e, nullptr, options);
    CHECK(ed.Init(&err));
    RenderTarget img;
    auto frames = [&](int n, std::vector<WindowEvent> events = {}) {
        for (int i = 0; i < n; ++i) {
            ed.Update(i == 0 ? events : std::vector<WindowEvent>(), 1280, 720, 1.0f, Engine::kFixedDt);
            CHECK(ed.DrawToImage(img));
        }
    };
    auto mouse = [](WindowEvent::Type type, float x, float y, int button = 0, bool down = false) {
        WindowEvent ev;
        ev.type = type;
        ev.x = x;
        ev.y = y;
        ev.button = button;
        ev.down = down;
        return ev;
    };
    frames(3);
    Scene& s = e.GetScene();
    EntityId level = s.FindByName("Level");
    ed.Select(level);
    ed.SetTileBrush(true, '~');
    frames(3);
    auto count = [&](char c) {
        int n = 0;
        for (const Json& row : s.Get<Tilemap>(level)->map.items()) n += static_cast<int>(std::count(row.asString().begin(), row.asString().end(), c));
        return n;
    };
    const int water = count('~');
    std::array<float, 4> view = ed.SceneViewRect();
    CHECK(view[2] > 100 && view[3] > 100);
    float cx = view[0] + view[2] * 0.5f, cy = view[1] + view[3] * 0.5f;
    // A left drag paints a line of cells.
    frames(2, {mouse(WindowEvent::Type::MouseMove, cx, cy)});
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx, cy, 0, true)});
    frames(2, {mouse(WindowEvent::Type::MouseMove, cx + 60, cy)});
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx + 60, cy, 0, false)});
    const int painted = count('~') - water;
    CHECK(painted >= 2);
    const size_t undo = e.UndoDepth();
    // A right drag erases (one more undo step).
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx, cy, 1, true)});
    frames(2, {mouse(WindowEvent::Type::MouseButton, cx, cy, 1, false)});
    CHECK(count('~') == water + painted - 1);
    CHECK(e.UndoDepth() == undo + 1);
    // Ctrl+Z undoes the erase, then the whole stroke.
    frames(4, CtrlChord(WindowKey::Z));
    CHECK(count('~') == water + painted);
    frames(4, CtrlChord(WindowKey::Z));
    CHECK(count('~') == water);
    // Brush off: clicks select again.
    ed.SetTileBrush(false, '~');
    frames(2);
    CHECK(ed.Selected() == level);
}
#endif

}  // namespace

// After a short UDP outage the two sides can select different send routes. Each side must keep
// receiving on both paths, otherwise the UDP sender's traffic is silently discarded.
TEST(NetworkFallbackRoutesMayDisagree) {
    int disagreements = 0;
    for (uint64_t start = 40; start < 110; ++start) for (uint64_t length = 31; length <= 45; length += 7) {
        auto udp = std::make_shared<LoopbackNetwork>(), tcp = std::make_shared<LoopbackNetwork>();
        LoopbackTransport ua(udp, 1), ub(udp, 2), ta(tcp, 1), tb(tcp, 2);
        NetworkDropTransport da(ua), db(ub);
        FallbackTransport a(da, ta), b(db, tb);
        CHECK(a.AddPeer(2, 100));
        size_t toA = 0, toB = 0; const uint8_t byte = 7;
        for (uint64_t frame = 0; frame < 400; ++frame) {
            da.drop = db.drop = frame >= start && frame < start + length;
            std::vector<TransportEvent> ea, eb;
            CHECK(a.Poll(frame, ea) && b.Poll(frame, eb));
            if (frame == 15) CHECK(b.AddPeer(1, 200));
            if (frame == 300) CHECK(a.Send(2, &byte, 1) && b.Send(1, &byte, 1));
            if (frame >= 300) { toA += ea.size(); toB += eb.size(); }
        }
        if (a.Selected(2) != b.Selected(1)) ++disagreements;
        CHECK(toA == 1 && toB == 1);
    }
    CHECK(disagreements > 0);  // the scenario under test really occurs in this sweep
}

// Remote entities play back one buffered frame per tick: no double-speed catch-up and stalls.
TEST(NetworkAuthoritativeInterpolationAdvancesOncePerFrame) {
    std::string project = AuthorityProject("authority_interpolation", "loopback"); Engine host, client;
    AuthorityPrepare(host, client, project, "loopback");
    host.Input().down.insert("W");  // the host's P1 moves one unit per frame
    NetworkSteps(host, client, 30);
    EntityId remote = client.GetScene().FindByName("P1"); CHECK(remote != 0);
    float previous = client.GetScene().Get<Transform>(remote)->position.x, first = previous, largest = 0;
    int stalled = 0;
    for (int frame = 0; frame < 60; ++frame) {
        NetworkSteps(host, client, 1);
        float x = client.GetScene().Get<Transform>(remote)->position.x, step = x - previous;
        largest = std::max(largest, step); if (step < 0.5f) ++stalled;
        previous = x;
    }
    CHECK(largest <= 1.001f);
    CHECK(stalled <= 2);
    CHECK(previous - first > 55);
    float server = host.GetScene().Get<Transform>(host.GetScene().FindByName("P1"))->position.x;
    CHECK(server - previous >= 3 && server - previous <= 10);  // about interpolationFrames behind
    host.Stop(); client.Stop(); RemoveAll(project);
}

// A departing participant leaves a running authoritative match; the remaining players continue.
TEST(NetworkAuthoritativePlayerLossKeepsMatch) {
    std::string project = AuthorityProject("authority_leave", "tcp", true), error;
    Engine host; CHECK(host.Open(project, &error)); host.GetScene().Clear();
    CHECK(Call(host, "net.spawn_local_peers", R"({"count":2})")["ok"].asBool()); host.Step(100);
    CHECK(Call(host, "net.state")["result"]["sync"]["state"].asString() == "running");
    CHECK(host.GetScene().Pool<NetPlayer>().size() == 3 && host.LocalPeer(1)->GetScene().Pool<NetPlayer>().size() == 3);
    CHECK(Call(*host.LocalPeer(2), "net.leave")["ok"].asBool()); host.Step(120);
    CHECK(Call(host, "net.state")["result"]["sync"]["state"].asString() == "running");
    CHECK(Call(*host.LocalPeer(1), "net.state")["result"]["sync"]["state"].asString() == "running");
    CHECK(Call(host, "net.players")["result"].size() == 2);
    CHECK(host.GetScene().Pool<NetPlayer>().size() == 2 && host.LocalPeer(1)->GetScene().Pool<NetPlayer>().size() == 2);
    for (const auto& item : host.GetScene().Pool<NetSync>()) CHECK(item.second.owner != 3);
    uint64_t before = host.Frame(); host.Step(60); CHECK(host.Frame() == before + 60);
    CHECK(host.Scripts().Errors().empty() && host.LocalPeer(1)->Scripts().Errors().empty());
    // Without any remote participant the match is over.
    CHECK(Call(*host.LocalPeer(1), "net.leave")["ok"].asBool()); host.Step(120);
    CHECK(Call(host, "net.state")["result"]["sync"]["state"].asString() == "stopped");
    host.Stop(); RemoveAll(project);
}

// Unauthenticated connections do not occupy player slots; maxPlayers is enforced at admission.
TEST(NetworkSessionPendingConnectionsDoNotFillLobby) {
    auto wire = std::make_shared<LoopbackNetwork>();
    SessionConfig config; config.mode = "lockstep"; config.transport = "loopback"; config.gameId = "pending"; config.maxPlayers = 2;
    Session server(config, std::make_unique<LoopbackTransport>(wire, 1), true, 1, "Server", 71, SessionTestRandom(820));
    Session idle(config, std::make_unique<LoopbackTransport>(wire, 2), false, 1, "Idle", 0, SessionTestRandom(821));
    Session real(config, std::make_unique<LoopbackTransport>(wire, 3), false, 1, "Real", 0, SessionTestRandom(822));
    Session late(config, std::make_unique<LoopbackTransport>(wire, 4), false, 1, "Late", 0, SessionTestRandom(823));
    uint64_t tick = 0;
    server.Advance(tick); idle.Advance(tick); ++tick;  // hello only: never answers the challenge
    for (; tick < 100; ++tick) { server.Advance(tick); real.Advance(tick); }
    CHECK(server.Stats()["peers"].size() == 2 && !idle.Connected());
    CHECK(real.Connected() && server.Players().size() == 2);
    for (; tick < 500; ++tick) { server.Advance(tick); real.Advance(tick); late.Advance(tick); }
    CHECK(!late.Connected() && real.Connected() && server.Players().size() == 2);
}

// A flush requested while persistence is deferred (recording/match) is written when it ends.
TEST(SaveDeferredFlushCommitsOnStop) {
    const std::string project = TempProject("save_deferred"), directory = JoinPath(project, "saves");
    Engine e; std::string error; CHECK(e.Open(project, &error));
    e.Saves().Configure(directory);
    CHECK(Call(e, "sim.record_state")["ok"].asBool());
    CHECK(Call(e, "script.eval", R"J({"code":"save.set('score', 9) save.flush()"})J")["ok"].asBool());
    CHECK(!FileExists(JoinPath(directory, "slot-default.json")));
    CHECK(Call(e, "sim.stop")["ok"].asBool());
    std::string text; CHECK(ReadTextFile(JoinPath(directory, "slot-default.json"), text));
    CHECK(Json::parse(text)["score"].asNumber() == 9);
    CHECK(!Call(e, "save.state", "{}")["result"]["dirty"].asBool());
    CHECK(RemoveAll(project));
}

// Binary Json payloads: exact round trip, compact repeated keys, and hostile input is rejected
// without large allocations.
TEST(NetworkJsonCodecRoundTripAndBounds) {
    Json world = Json::MakeObject();
    for (int i = 1; i <= 200; ++i) {
        Json entity = Json::parse(R"({"name":"Player","owner":3,"predict":true,"prefab":"prefabs/player.prefab.json","always":[],"Transform.position":[1.5,-2.25,1000000],"Transform.rotation":[0,90,0],"Tag.tags":"a"})");
        entity["source"] = i; entity["Transform.position"][0] = 0.1 * i; world[std::to_string(i)] = entity;
    }
    world["misc"] = Json::parse(R"([null,true,false,-7,9007199254740991,0.1,1e300,"",{"nested":{"deep":[1,2,3]}}])");
    std::vector<uint8_t> bytes; CHECK(EncodeJson(world, bytes, 56000));
    Json decoded; CHECK(DecodeJson(bytes.data(), bytes.size(), decoded)); CHECK(decoded == world && decoded.dump() == world.dump());
    CHECK(bytes.size() * 3 < world.dump().size());  // interned keys and binary numbers
    std::vector<uint8_t> small; CHECK(!EncodeJson(world, small, 100) && small.empty());
    Json deep = Json::MakeArray(), nan = Json::MakeArray(); nan.push(std::nan(""));
    for (int i = 0; i < 20; ++i) { Json outer = Json::MakeArray(); outer.push(deep); deep = outer; }
    CHECK(!EncodeJson(deep, small, 56000) && !EncodeJson(nan, small, 56000));
    Json out = Json("unchanged");
    // Truncation at every length, trailing bytes, unknown tags and absurd counts.
    for (size_t size = 0; size < bytes.size(); size += 7) CHECK(!DecodeJson(bytes.data(), size, out));
    std::vector<uint8_t> trailing = bytes; trailing.push_back(0); CHECK(!DecodeJson(trailing.data(), trailing.size(), out));
    for (std::vector<uint8_t> bad : {std::vector<uint8_t>{11}, {9, 0xff, 0xff, 0xff, 0xff, 0x0f}, {10, 0xff, 0xff, 0xff, 0x7f},
                                     {8, 0}, {7, 5, 'a'}, {3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
                                     {5, 0, 0, 0x80, 0x7f}, {6, 0, 0, 0, 0, 0, 0, 0xf0, 0x7f}})
        CHECK(!DecodeJson(bad.data(), bad.size(), out));
    std::vector<uint8_t> nested(40, 9); for (size_t i = 0; i < nested.size(); ++i) nested[i] = i % 2 ? 1 : 9;
    CHECK(!DecodeJson(nested.data(), nested.size(), out));
    CHECK(out == Json("unchanged"));
    uint32_t seed = 12345;
    for (int round = 0; round < 2000; ++round) {
        std::vector<uint8_t> fuzz = bytes;
        for (int flips = 0; flips < 4; ++flips) { seed = seed * 1664525u + 1013904223u; fuzz[(seed >> 8) % fuzz.size()] = static_cast<uint8_t>(seed >> 24); }
        Json ignored; DecodeJson(fuzz.data(), fuzz.size(), ignored);
    }
}

// Snapshots and their ACKs travel on the unreliable channel: a lossy link neither stalls the
// match behind retransmissions nor fills the reliable queue, and the client still converges.
TEST(NetworkAuthoritativeUnreliableSnapshotsConverge) {
    LoopbackConfig faults; faults.seed=977; faults.lossPermille=150; faults.duplicatePermille=80; faults.latencyFrames=1; faults.reorderFrames=3;
    auto wire=std::make_shared<LoopbackNetwork>(faults); SessionConfig config; config.mode="authoritative"; config.transport="loopback"; config.sync.keys={"W"};
    Session server(config,std::make_unique<LoopbackTransport>(wire,1),true,1,"Server",71,SessionTestRandom(830));
    Session client(config,std::make_unique<LoopbackTransport>(wire,2),false,1,"Client",0,SessionTestRandom(831));
    uint64_t tick=0; for(;tick<300;++tick) {server.Advance(tick);client.Advance(tick);}
    CHECK(server.Connected() && client.Connected());
    Authority host(config,true,1,99,[&](uint32_t player,const std::vector<uint8_t>& b){return server.SendSync(player,b);});
    Authority guest(config,false,2,99,[&](uint32_t player,const std::vector<uint8_t>& b){return client.SendSync(player,b);});
    uint64_t lossy=0;
    host.SetUnreliableSend([&](uint32_t player,const std::vector<uint8_t>& b){++lossy;return server.SendSync(player,b,false);});
    guest.SetUnreliableSend([&](uint32_t player,const std::vector<uint8_t>& b){return client.SendSync(player,b,false);});
    std::string error; CHECK(host.Start({1,2},&error)); uint64_t total=0,frames=0,received=0; Json last, world;
    auto pump=[&] {
        server.Advance(tick);client.Advance(tick);
        for(const auto& msg:server.DrainSync()) host.Receive(msg.first,msg.second);
        for(const auto& msg:client.DrainSync()) guest.Receive(msg.first,msg.second);
        host.TakeStart();guest.TakeStart();
        for(const auto& update:guest.DrainUpdates()) {last=update.world;++received;}
    };
    for(;tick<100000 && frames<5000;++tick) {
        pump();
        if(guest.Running() && guest.CanPredict()) {FrameInput input;input.down=(tick/7)%2;guest.Submit(input);}
        if(host.Running()) {
            auto inputs=host.Consume(FrameInput{});total+=inputs[2].down; ++frames;
            if(frames%3==0) {world=Json::MakeObject();world["1"]=Json::MakeObject();world["1"]["Transform.position"]=Json::parse("[0,0,0]");world["1"]["Transform.position"][0]=total;host.Snapshot(frames,{{2,world}});}
        }
        if(!host.Running() && frames) break;
    }
    CHECK(frames==5000 && host.Running() && guest.Running() && host.TakeDropped().empty());
    CHECK(lossy>1500 && received>800 && received<lossy);  // some snapshots were lost and never resent
    for(int n=0;n<300;++n,++tick) { if(n%3==0) host.Snapshot(frames,{{2,world}}); pump(); }
    CHECK(last==world);
    CHECK(server.Stats()["peers"][0]["pending"].asNumber()<32);
}

// dropPolicy "empty": after one timeout a stalled peer no longer costs a wait per frame, and a
// peer that resumes catches up through the buffered confirmed frames and plays again.
TEST(NetworkFrameSyncStalledPeerCatchesUp) {
    SyncConfig config; config.keys={"W"}; config.delay=2; config.waitFrames=30; config.emptyOnTimeout=true; config.hashInterval=600;
    std::deque<std::vector<uint8_t>> toClient, toHost;
    FrameSync host(config,true,1,5,[&](uint32_t,const std::vector<uint8_t>& b){toClient.push_back(b);return true;});
    FrameSync client(config,false,2,5,[&](uint32_t,const std::vector<uint8_t>& b){toHost.push_back(b);return true;});
    std::string error; CHECK(host.Start({1,2},&error));
    uint64_t tick=0, hostTotal=0, clientTotal=0; bool frozen=false; uint64_t clientPlayed=0;
    auto step=[&](FrameSync& sync, uint64_t& total, bool held, int frames) {
        sync.Tick(tick); sync.TakeStart();
        for(int n=0;n<frames;++n) {
            if(n && !sync.Lagging()) break;
            if(sync.NeedsInput()) {FrameInput input; input.down=held?1:0; sync.Submit(input);}
            sync.Tick(tick);
            auto inputs=sync.Next(); if(!inputs) break;
            for(const auto& input:*inputs) { total+=input.first*(input.second.down+1)+sync.Frame(); if(&sync==&host && input.first==2) clientPlayed+=input.second.down; }
            sync.Applied(*inputs);
        }
    };
    auto run=[&](uint64_t until) {
        for(;tick<until;++tick) {
            // One tick of latency each way; a frozen client neither reads nor simulates.
            auto in=std::move(toHost); toHost.clear(); for(const auto& b:in) host.Receive(2,b);
            step(host,hostTotal,false,1);
            if(!frozen) { auto out=std::move(toClient); toClient.clear(); for(const auto& b:out) client.Receive(1,b); step(client,clientTotal,true,1+FrameSync::kCatchUpFrames); }
        }
    };
    run(100); CHECK(host.Running() && client.Running() && host.Frame()>80 && clientPlayed>70);
    frozen=true; uint64_t before=host.Frame(); run(400);
    // One 30-tick wait, then full speed (the old behaviour gave about ten frames here).
    CHECK(host.Frame()>before+250 && host.State()["stalled"].size()==1);
    frozen=false; uint64_t played=clientPlayed; run(700);
    CHECK(host.Running() && client.Running() && host.State()["stalled"].size()==0);
    CHECK(client.Frame()+8>=host.Frame() && clientPlayed>played+150);
    // Both sides simulated the same merged inputs for every frame the client has reached.
    uint64_t target=client.Frame(); CHECK(target>600);
    frozen=true; CHECK(host.Frame()>=target);
    CHECK(client.State()["state"].asString()=="running" && client.State()["error"].asString().empty());
}

// The local API server runs on the platform sockets: request/response, a silent client cannot
// block Stop, and an ephemeral port is reported.
TEST(HttpServerOnPlatformSockets) {
#ifndef __EMSCRIPTEN__
    const uint64_t live = PlatformNetSocketsLive();
    {
        HttpServer server; std::string error, response;
        CHECK(server.Start(0, [](const HttpRequest& request) {
            HttpResponse out; out.body = request.method + " " + request.path + " " + request.Query("x") + " " + std::to_string(request.body.size());
            return out;
        }, &error));
        CHECK(server.Port() > 0);
        std::string large(300000, 'a');
        CHECK(HttpPostLocal(server.Port(), "/api/call?x=7", large, response, &error));
        CHECK(response == "POST /api/call 7 300000");
        for (int i = 0; i < 20; ++i) CHECK(HttpPostLocal(server.Port(), "/p", "{}", response, &error) && response == "POST /p  2");
        // A client that connects and never sends must not keep the server from stopping.
        auto silent = CreateNetSocket(SocketKind::Tcp, &error); CHECK(silent && silent->Connect({"127.0.0.1", static_cast<uint16_t>(server.Port())}, &error));
        PlatformSleep(0.05);
        const double start = PlatformTimeSeconds(); server.Stop(); CHECK(PlatformTimeSeconds() - start < 2.0);
        CHECK(!HttpPostLocal(server.Port(), "/p", "{}", response, &error) && !error.empty());
    }
    CHECK(PlatformNetSocketsLive() == live);
#endif
}

int main() {
    Log::SetEcho(false);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    for (const TestCase& t : Tests()) {
        int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }
    std::printf("\n%zu tests, %d failed checks\n", Tests().size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
