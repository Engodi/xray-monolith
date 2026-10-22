////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_bridge.cpp
//	Created 	: 23.10.2026
//	Description : Coop Lua bridge / event adapter (task F0-05)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop/coop_bridge.h"

#include <iterator>

namespace coop {
namespace {
// Export manifest of the C++ half of the coop Lua bridge. Keep it sorted: the
// fingerprint below is computed over the table order. Every entry added here
// must be registered in coop_bridge_script.cpp in the same commit.
const SApiEntry g_api_entries[] = {
    {"alife_enabled", 0, 1},
    {"api_fingerprint", 0, 1},
    {"api_manifest", 0, 1},
    {"api_version", 0, 1},
    {"clear", 0, 1},
    {"debug_enabled", 0, 1},
    {"emit_event", 2, 1},
    {"enable_dispatch", 0, 1},
    {"god_mode", 0, 1},
    {"inbound", 1, 1},
    {"is_client", 0, 1},
    {"is_host", 0, 1},
    {"is_offline", 0, 1},
    {"last_error", 0, 1},
    {"local_player", 0, 1},
    {"peer_count", 0, 1},
    {"pump", 0, 1},
    {"role", 0, 1},
    {"session", 0, 1},
    {"set_alife_enabled", 1, 1},
    {"set_debug_enabled", 2, 1},
    {"set_god_mode", 1, 1},
    {"set_loopback", 1, 1},
    {"set_peer_count", 1, 1},
    {"set_role", 2, 1},
    {"stats", 0, 1},
    {"sync", 2, 1},
};

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void fnv_mix(std::uint64_t& hash, const char* text) {
    for (const char* it = text; it && *it; ++it) {
        hash ^= static_cast<std::uint8_t>(*it);
        hash *= kFnvPrime;
    }
}

void fnv_mix_uint(std::uint64_t& hash, std::uint32_t value) {
    for (std::uint32_t i = 0; i < 4; ++i) {
        hash ^= static_cast<std::uint8_t>((value >> (i * 8)) & 0xff);
        hash *= kFnvPrime;
    }
}

bool is_valid_role(ERole role) {
    return role == ERole::offline || role == ERole::host || role == ERole::client;
}
}  // namespace

const char* role_name(ERole role) {
    switch (role) {
    case ERole::host:
        return "host";
    case ERole::client:
        return "client";
    case ERole::offline:
        break;
    }
    return "offline";
}

CBridge::CBridge()
    : m_role(ERole::offline), m_local_player(0), m_peer_count(0), m_event_seq(0), m_sync_seq(0),
      m_loopback(false), m_debug(false), m_alife(true), m_god(false), m_sink(nullptr), m_stats{} {}

const char* CBridge::role_name() const { return coop::role_name(m_role); }

bool CBridge::set_role(ERole role, std::uint32_t local_player) {
    if (!is_valid_role(role)) {
        set_error("invalid_role");
        return false;
    }

    if (role == ERole::offline)
        local_player = 0;

    m_role = role;
    m_local_player = local_player;
    m_last_error.clear();
    return true;
}

void CBridge::set_peer_count(std::uint32_t count) {
    m_peer_count = count > kMaxSlots ? kMaxSlots : count;
}

std::uint32_t CBridge::emit_event(const std::string& name, std::string payload) {
    if (name.empty()) {
        set_error("empty_event_name");
        return 0;
    }

    SMessage message;
    message.seq = ++m_event_seq;
    message.channel = name;
    message.payload = std::move(payload);

    ++m_stats.events_out;
    m_stats.last_event_seq = message.seq;
    m_last_error.clear();

    if (m_sink)
        m_sink->on_outbound(message);
    if (m_loopback)
        deliver(message);

    return message.seq;
}

std::uint32_t CBridge::send_sync(const std::string& channel, std::string payload) {
    if (channel.empty()) {
        set_error("empty_sync_channel");
        return 0;
    }

    if (m_role == ERole::offline) {
        ++m_stats.sync_rejected;
        set_error("not_connected");
        return 0;
    }

    // Authority rules live in the Lua layer (coop_sync), the bridge only
    // requires an established session.
    SMessage message;
    message.seq = ++m_sync_seq;
    message.channel = channel;
    message.payload = std::move(payload);

    ++m_stats.sync_out;
    m_stats.last_sync_seq = message.seq;
    m_last_error.clear();

    if (m_sink)
        m_sink->on_outbound(message);
    if (m_loopback)
        deliver(message);

    return message.seq;
}

std::size_t CBridge::pump() {
    if (!m_sink)
        return 0;

    std::size_t count = 0;
    SMessage message;
    while (m_sink->pull_inbound(message)) {
        deliver(message);
        ++count;
    }
    return count;
}

bool CBridge::enqueue(const SMessage& message) {
    if (m_inbox.size() >= kInboxCapacity) {
        m_inbox.pop_front();
        ++m_stats.inbound_dropped;
    }
    m_inbox.push_back(message);
    return true;
}

bool CBridge::pop_inbound(SMessage& message) {
    if (m_inbox.empty())
        return false;

    message = std::move(m_inbox.front());
    m_inbox.pop_front();
    return true;
}

void CBridge::set_sink(IBridgeSink* sink) { m_sink = sink; }

void CBridge::set_dispatch(std::function<void(const SMessage&)> dispatch) {
    m_dispatch = std::move(dispatch);
}

void CBridge::clear_dispatch() { m_dispatch = nullptr; }

void CBridge::set_loopback(bool enabled) { m_loopback = enabled; }

const std::vector<SApiEntry>& CBridge::api_entries() {
    static const std::vector<SApiEntry> entries(std::begin(g_api_entries), std::end(g_api_entries));
    return entries;
}

std::uint64_t CBridge::manifest_fingerprint() {
    std::uint64_t hash = kFnvOffset;
    for (const SApiEntry& entry : api_entries()) {
        fnv_mix(hash, entry.name);
        fnv_mix_uint(hash, entry.argc);
        fnv_mix_uint(hash, entry.since);
        fnv_mix(hash, "\n");
    }
    return hash;
}

bool CBridge::set_debug_enabled(bool enabled, bool launcher_allows) {
    if (enabled && !launcher_allows) {
        set_error("debug_not_allowed");
        return false;
    }

    m_debug = enabled;
    if (!enabled) {
        m_alife = true;
        m_god = false;
    }
    m_last_error.clear();
    return true;
}

bool CBridge::set_alife_enabled(bool enabled) {
    if (!m_debug) {
        set_error("debug_disabled");
        return false;
    }
    m_alife = enabled;
    m_last_error.clear();
    return true;
}

bool CBridge::set_god_mode(bool enabled) {
    if (!m_debug) {
        set_error("debug_disabled");
        return false;
    }
    m_god = enabled;
    m_last_error.clear();
    return true;
}

void CBridge::reset() {
    m_role = ERole::offline;
    m_local_player = 0;
    m_peer_count = 0;
    m_event_seq = 0;
    m_sync_seq = 0;
    m_loopback = false;
    m_debug = false;
    m_alife = true;
    m_god = false;
    m_last_error.clear();
    m_inbox.clear();
    m_stats = SStats{};
}

void CBridge::deliver(const SMessage& message) {
    ++m_stats.events_in;
    if (m_dispatch) {
        m_dispatch(message);
        return;
    }
    enqueue(message);
}

CBridge& bridge() {
    static CBridge instance;
    return instance;
}
}  // namespace coop
