////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_bridge.h
//	Created 	: 23.10.2026
//	Description : Coop Lua bridge / event adapter (task F0-05)
//
//	Stable C++ interface between the engine, the Lua gameplay layer and the
//	coop runtime (transport F0-02, replication core F0-06).
//
//	NFR-ARCH-9 : the engine stays behind this header; the coop core only
//	             depends on this interface, never on engine internals.
//	NFR-ARCH-10: only events and decisions cross the C++/Lua boundary.
//	             Nothing in here is called every frame by the bridge itself.
////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace coop {
enum class ERole : std::uint8_t {
    offline = 0,
    host = 1,
    client = 2,
};

// One message on the wire (event or sync call). `channel` is the event name
// for events and the sync channel name for sync calls.
struct SMessage {
    std::uint32_t seq = 0;
    std::string channel;
    std::string payload;
};

// Transport seam. Installed by the networking layer (F0-02/F0-06). When no
// sink is installed the bridge stays a pure in-process bookkeeper.
class IBridgeSink {
public:
    virtual ~IBridgeSink() = default;

    virtual void on_outbound(const SMessage& message) = 0;
    // Returns false when the inbound queue of the sink is empty.
    virtual bool pull_inbound(SMessage& message) = 0;
};

// One entry of the exported coop API. Used for host<->client export parity
// verification on connect (AC of F0-05).
struct SApiEntry {
    const char* name;
    std::uint32_t argc;
    std::uint32_t since;
};

struct SStats {
    std::uint64_t events_out;
    std::uint64_t events_in;
    std::uint64_t sync_out;
    std::uint64_t sync_in;
    std::uint64_t sync_rejected;
    std::uint64_t inbound_dropped;
    std::uint32_t last_event_seq;
    std::uint32_t last_sync_seq;
};

class CBridge {
public:
    static constexpr std::uint32_t kApiVersion = 1;
    static constexpr std::uint32_t kMaxSlots = 4;
    static constexpr std::size_t kInboxCapacity = 1024;

    CBridge();
    ~CBridge() = default;

    CBridge(const CBridge&) = delete;
    CBridge& operator=(const CBridge&) = delete;

    // --- session -----------------------------------------------------------
    bool set_role(ERole role, std::uint32_t local_player);
    ERole role() const { return m_role; }
    const char* role_name() const;
    bool is_host() const { return m_role == ERole::host; }
    bool is_client() const { return m_role == ERole::client; }
    bool is_offline() const { return m_role == ERole::offline; }

    std::uint32_t local_player() const { return m_local_player; }
    std::uint32_t peer_count() const { return m_peer_count; }
    void set_peer_count(std::uint32_t count);

    // --- outbound ----------------------------------------------------------
    // Returns the assigned sequence number or 0 on rejection (see last_error).
    std::uint32_t emit_event(const std::string& name, std::string payload);
    std::uint32_t send_sync(const std::string& channel, std::string payload);

    // --- inbound -----------------------------------------------------------
    // Drains the installed sink into the dispatch hook (or the local queue).
    std::size_t pump();
    bool pop_inbound(SMessage& message);
    bool enqueue(const SMessage& message);
    std::size_t inbound_size() const { return m_inbox.size(); }

    // --- wiring ------------------------------------------------------------
    void set_sink(IBridgeSink* sink);
    IBridgeSink* sink() const { return m_sink; }
    void set_dispatch(std::function<void(const SMessage&)> dispatch);
    void clear_dispatch();
    // Debug helper (and in-process end-to-end checks): re-deliver own
    // outbound traffic to the local inbound path.
    void set_loopback(bool enabled);
    bool loopback() const { return m_loopback; }

    // --- exports parity ----------------------------------------------------
    static const std::vector<SApiEntry>& api_entries();
    static std::uint64_t manifest_fingerprint();
    std::uint32_t api_version() const { return kApiVersion; }

    // --- debug surface (PLAN 8.11) ----------------------------------------
    // Everything below is off/frozen-only by default and can be enabled only
    // when the launcher passed the debug switch (checked caller-side).
    bool set_debug_enabled(bool enabled, bool launcher_allows);
    bool debug_enabled() const { return m_debug; }
    bool set_alife_enabled(bool enabled);
    bool alife_enabled() const { return m_alife; }
    bool set_god_mode(bool enabled);
    bool god_mode() const { return m_god; }

    // --- diagnostics -------------------------------------------------------
    const std::string& last_error() const { return m_last_error; }
    const SStats& stats() const { return m_stats; }
    void reset();

private:
    void deliver(const SMessage& message);
    void set_error(const char* error) { m_last_error = error; }

    ERole m_role;
    std::uint32_t m_local_player;
    std::uint32_t m_peer_count;
    std::uint32_t m_event_seq;
    std::uint32_t m_sync_seq;
    bool m_loopback;
    bool m_debug;
    bool m_alife;
    bool m_god;
    std::string m_last_error;
    std::deque<SMessage> m_inbox;
    IBridgeSink* m_sink;
    std::function<void(const SMessage&)> m_dispatch;
    SStats m_stats;
};

const char* role_name(ERole role);

CBridge& bridge();
}  // namespace coop
