#pragma once
#include "medium/core.hpp"
#include <memory>
namespace med {
// The largest message this transport will send or accept, in bytes.
//
// It was 1024, chosen when the only snapshot on the wire was a hand-rolled 92-byte record of
// thirteen actor fields. A versioned `encode_snapshot` of a two-fighter duel is **682 bytes** and
// of the shipped courtyard **852**, so the old bound left almost no headroom and a room with a few
// more actors would have been disconnected by its own server rather than told it was too large.
//
// Stated as a named constant so the send side, the receive side and anything sizing a buffer read
// the same number - they were three separate literals, which is how a sender comes to be allowed
// to produce what a receiver will hang up on.
//
// Raised again to 64 KiB: an actor encodes to about 290 bytes now that it carries
// stats, abilities, statuses and a hit ledger, so the representative room is about 6.4 KB and was
// over the 4 KB bound - the host would have thrown from its frame loop. 64 KiB holds a room of 64
// actors with room to spare; the `snapshot` suite checks both. GNS fragments an unreliable
// message this size and drops it whole if any fragment is lost, which a snapshot stream tolerates:
// the next one supersedes it.
//
// **This is not a bandwidth budget.** Sending whole-simulation snapshots is the expensive shape:
// 682 bytes at 20 Hz to two clients is roughly 27 KB/s out of the server, and it grows with every
// actor in the room whether or not that actor changed. Delta encoding against the last
// acknowledged snapshot is the real answer and is not implemented; this bound only stops a
// correct snapshot being refused for being correct.
inline constexpr size_t max_message_bytes = 64 * 1024;
struct Packet {
    uint32_t peer{};
    std::vector<uint8_t> bytes;
};
class Endpoint {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    Endpoint(bool server, uint16_t port, int lag_ms = 0, float loss = 0, bool chaos = false);
    ~Endpoint();
    Endpoint(const Endpoint &) = delete;
    Endpoint &operator=(const Endpoint &) = delete;
    std::vector<Packet> poll();
    std::vector<uint32_t> peers() const;
    bool send(uint32_t peer, std::span<const uint8_t> data, bool reliable = false);
    void disconnect(uint32_t peer);
    int ping(uint32_t peer) const;
};
} // namespace med
