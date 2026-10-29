////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_core_runtime.cpp
//	Created 	: 23.10.2026
//	Description : Engine wiring of the coop core (F0-04/F0-05/F0-07/F0-12)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop/coop_core_runtime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "coop/coop_bot_channel.h"
#include "coop/coop_bridge.h"
#include "coop/coop_transport.h"
#include "xrn/cfgdiff/diff.hpp"
#include "xrn/cfgdiff/handshake.hpp"
#include "xrn/cfgdiff/manifest.hpp"
#include "xrn/core/actor.hpp"
#include "xrn/core/bridge_seam.hpp"
#include "xrn/core/connection.hpp"
#include "xrn/core/environment.hpp"
#include "xrn/core/error.hpp"
#include "xrn/core/inventory.hpp"
#include "xrn/core/level.hpp"
#include "xrn/core/link.hpp"
#include "xrn/core/mirror.hpp"
#include "xrn/core/observability.hpp"
#include "xrn/core/session.hpp"
#include "xrn/core/transport_link.hpp"
#include "xrn/core/world.hpp"

#include "ai_space.h"
#include "script_engine.h"

namespace coop {
namespace {

namespace fs = std::filesystem;
namespace obs = xrn::core::obs;
namespace env = xrn::core::env;
namespace act = xrn::core::actor;
namespace inv = xrn::core::inventory;
namespace lvl = xrn::core::level;
namespace cfg = xrn::cfgdiff;
namespace rep = xrn::replication;
using xrn::core::BridgeMessage;
using xrn::core::ClientSession;
using xrn::core::CoreError;
using xrn::core::failed;
using xrn::core::HostSession;
using xrn::core::IMessageLink;
using xrn::core::InProcessNetwork;
using xrn::core::PeerId;
using xrn::core::WorldState;

// Fixed step of the authoritative core. The engine frame rate varies (headless
// loop, level load stalls); the core must not.
constexpr std::uint32_t kStepMs = 20;
// A stalled frame (level load, debugger) must not replay hundreds of steps.
constexpr std::uint32_t kMaxCatchUpMs = 200;
constexpr std::uint32_t kStateEveryMs = 100;
constexpr std::uint32_t kLogFlushEveryMs = 500;
constexpr std::uint32_t kAckTimeoutMs = 2000;
// Debug profile only: a short reconnect window, so the `expired` path of the
// reconnect scenario is observable inside a test run instead of after 120 s.
constexpr std::uint32_t kDebugReconnectWindowMs = 600;
constexpr PeerId kHostPeerId = 1;
constexpr PeerId kLoopbackPeerId = 2;
// Matches `ClientTransportLink::kHostPeerId`: whatever the transport, the
// client sees exactly one logical peer.
constexpr PeerId kClientPeerId = 1;
// The host's own player row (`xrn::core::kHostPlayerId`): in the network
// profile the host bot acts as this player, not through a loopback client.
constexpr xrn::core::PlayerId kHostLocalPlayerId = xrn::core::kHostPlayerId;
// Debug surface over the network (F0-02): a bot command is a host-side mechanic,
// so a client process sends it as an intent and answers its bot only when the
// host's verdict comes back. Intent payload: `name\nargument`; ack: `status\ndetail`.
constexpr const char* kDebugIntentChannel = "coop.debug.intent";
constexpr const char* kDebugAckChannel = "coop.debug.ack";
constexpr char kDebugFieldSeparator = '\n';
// F1-01 (US-1.2): how a client asks the host to move its character. The client
// sends what the player wants (a waypoint), never a slot or a position of
// somebody else: the session attributes the intent to the sender (NFR-ARCH-1).
constexpr const char* kMoveIntentChannel = "coop.move.intent";
constexpr const char* kMoveAckChannel = "coop.move.ack";
// F1-02 (US-1.3): a drop is the same kind of intent — the host puts the item of
// the sender's own inventory back into the world.
constexpr const char* kDropIntentChannel = "coop.drop.intent";
constexpr const char* kDropAckChannel = "coop.drop.ack";
// Fields of an intent payload (`key=value` pairs joined by ';') and of a verdict
// payload (`value` lines separated by kDebugFieldSeparator, so a free-form detail
// never has to be escaped).
constexpr char kIntentFieldSeparator = ';';
// The run animation is held for one beat after a debug move, so the animation
// field of the record changes for a remote observer (US-1.2) and then settles to
// idle: the debug move itself is instantaneous.
constexpr std::int64_t kMoveAnimationMs = 200;
// Host -> clients: the session events the host observes for its own bot channel
// (F0-12) travel to every client on this channel, so a client bot sees the same
// events as the host (`observer = peer` over the network). Payload: `event=<name>`.
constexpr const char* kObsEventChannel = "coop.obs.event";
constexpr const char* kObsEventField = "event=";
// F0-02 client recovery: a lost link is re-established on the same identity, and
// the host's reconnect window (F1-10) decides what happens. The client keeps the
// session open for one window longer than the host reserves the slot, so a
// "window expired" answer still arrives instead of the client giving up first.
constexpr std::uint32_t kClientReconnectGraceMs = kDebugReconnectWindowMs * 2;
// A fresh transport session takes a handshake; never restart it faster.
constexpr std::uint32_t kClientReconnectRetryMs = 50;
constexpr const char* kDefaultLevel = "l01_escape";
constexpr const char* kDebugItem = "bread";
// F1-13 debug transition: a debug run does not walk into a level changer and does
// not load a real map. The host commits as soon as the movers acknowledged the
// token, or after this budget — a straggler then resyncs into the committed state
// (AC-3.3.4), so a broken load can never freeze the group.
constexpr std::int64_t kDebugLoadBudgetMs = 300;
// F0-09 debug probe: the dropped peer must still receive the verdict of the
// command that caused the drop, so the drop follows it after this delay.
constexpr std::int64_t kDebugCfgDropDelayMs = 100;
// A refusal list is a player-facing label: bounded like the F1-12 join notice.
constexpr std::size_t kMaxProbePaths = 64;
constexpr std::size_t kMaxProbePathBytes = 256;

// --- F0-09: the harness build ------------------------------------------------
//
// A harness process has no gamedata tree to scan, so the adapter serves a
// deterministic synthetic build: the same fixture list in every process, hashed
// per path. A join therefore carries a real cfg-diff manifest (`cfgdiff::Probe`),
// the host compares it against its own (`cfgdiff::Gate`), and the debug build
// probe can diverge a file on purpose without touching a real installation.
constexpr const char* kHarnessBuildId = "xray-monolith/harness";
constexpr const char* kHarnessModId = "coop-harness";
constexpr const char* kHarnessModVersion = "1";
constexpr const char* kHarnessConfigFiles[] = {
    "creatures.ltx", "items.ltx", "mod_load_order.ltx", "weapon_ammo.ltx", "zones.ltx",
};

// --- F1-13: the harness map profile ------------------------------------------
//
// Every harness session starts on the map the profile names (`XRN_HARNESS_LEVEL`,
// `l01_escape` by default); the debug surface can move the whole group to the
// second known map. Anything else is refused instead of guessed.
constexpr lvl::InstanceId kStartInstance = 1;
constexpr lvl::InstanceId kTargetInstance = 2;
constexpr const char* kTargetLevel = "l02_darkvalley";

bool is_probe_path(const std::string& path) {
    if (path.empty() || path.size() > kMaxProbePathBytes) {
        return false;
    }
    // A config path of the build manifest: relative, no traversal, printable.
    if (path.front() == '/' || path.front() == '\\') {
        return false;
    }
    return path.find("..") == std::string::npos && path.find('\\') == std::string::npos &&
           path.find_first_of("\r\n\t") == std::string::npos;
}

// `a,b,c` -> ordered unique list of build paths (the debug surface treats its own
// argument as untrusted: it reaches a manifest and an observation field).
std::vector<std::string> split_probe_paths(const std::string& csv) {
    std::vector<std::string> paths;
    std::size_t start = 0;
    while (start <= csv.size() && paths.size() < kMaxProbePaths) {
        const std::size_t comma = csv.find(',', start);
        std::string item =
            csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const std::size_t first = item.find_first_not_of(" \t");
        const std::size_t last = item.find_last_not_of(" \t");
        item = first == std::string::npos ? std::string() : item.substr(first, last - first + 1);
        if (is_probe_path(item) && std::find(paths.begin(), paths.end(), item) == paths.end()) {
            paths.push_back(std::move(item));
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return paths;
}

// The divergent list keeps the order the probe asked for (the player reads the
// list that it triggered), but only paths the core really reported survive.
std::vector<std::string> order_like(const std::vector<std::string>& requested,
                                    const std::vector<std::string>& reported) {
    std::vector<std::string> paths;
    for (const std::string& path : requested) {
        if (std::find(reported.begin(), reported.end(), path) != reported.end() &&
            std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }
    for (const std::string& path : reported) {
        if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
            paths.push_back(path);
        }
    }
    return paths;
}

cfg::CfgManifest harness_build() {
    cfg::CfgManifest manifest;
    manifest.build_id = kHarnessBuildId;
    manifest.mods.push_back(cfg::CfgMod{kHarnessModId, kHarnessModVersion});
    for (const char* path : kHarnessConfigFiles) {
        cfg::CfgFile file;
        file.path = path;
        file.size = static_cast<std::uint32_t>(file.path.size());
        file.digest = cfg::sha256(std::string("harness:") + file.path);
        manifest.files.push_back(std::move(file));
    }
    // Canonical order + the build digest: exactly what `Gate::verify` compares.
    cfg::finalize(manifest);
    return manifest;
}

// A build whose listed files carry other bytes (a changed .ltx); a path the host
// does not have at all is an extra file of the client. Both are divergences the
// core reports by path.
cfg::CfgManifest diverge_build(const cfg::CfgManifest& base,
                               const std::vector<std::string>& paths) {
    cfg::CfgManifest next = base;
    for (const std::string& path : paths) {
        bool known = false;
        for (cfg::CfgFile& file : next.files) {
            if (file.path != path) {
                continue;
            }
            known = true;
            file.size += 1;
            file.digest = cfg::sha256(std::string("divergent:") + path);
        }
        if (!known) {
            cfg::CfgFile extra;
            extra.path = path;
            extra.size = 1;
            extra.digest = cfg::sha256(std::string("divergent:") + path);
            next.files.push_back(std::move(extra));
        }
    }
    cfg::finalize(next);
    return next;
}

std::string level_for_instance(lvl::InstanceId instance, const std::string& start_level) {
    return instance == kTargetInstance ? std::string(kTargetLevel) : start_level;
}

bool instance_for_level(const std::string& level, const std::string& start_level,
                        lvl::InstanceId& out) {
    if (level.empty()) {
        return false;
    }
    if (level == kTargetLevel) {
        out = kTargetInstance;
        return true;
    }
    if (level == start_level) {
        out = kStartInstance;
        return true;
    }
    return false;
}

std::int64_t epoch_ms() {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
}

std::string env_value(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::uint32_t env_uint(const char* name, std::uint32_t fallback) {
    const std::string text = env_value(name);
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0') {
        return fallback;
    }
    return static_cast<std::uint32_t>(parsed);
}

// `key=value` tail of a debug command ("window=expired").
std::string argument_value(const std::string& argument, const char* key) {
    const std::string prefix = std::string(key) + "=";
    const std::size_t position = argument.find(prefix);
    if (position == std::string::npos) {
        return std::string();
    }
    const std::size_t start = position + prefix.size();
    const std::size_t end = argument.find_first_of(" \t", start);
    return argument.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// --- intent/verdict payloads (F1-01/F1-02) ---------------------------------
//
// Intent (Lua/bot side -> host): `key=value` pairs joined by ';'. Verdict (host
// -> sender): values on their own lines, so a free-form detail never has to be
// escaped. Both stay private to this adapter: the core owns the domain types and
// the session owns the attribution, this is only their carrier over the bridge.
bool parse_double(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

bool intent_field(const std::string& payload, const char* key, std::string& out) {
    const std::string prefix = std::string(key) + "=";
    std::size_t start = 0;
    while (start <= payload.size()) {
        const std::size_t end = payload.find(kIntentFieldSeparator, start);
        const std::string item =
            payload.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (item.rfind(prefix, 0) == 0) {
            out = item.substr(prefix.size());
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

bool intent_number(const std::string& payload, const char* key, double& out) {
    std::string text;
    return intent_field(payload, key, text) && parse_double(text, out);
}

// F1-01 intent payload: the point the player wants to stand on. `%.9g` is the
// shortest text that round-trips a float, so the host applies exactly the point
// the client named.
std::string encode_move_intent(const act::Vec3& position) {
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "x=%.9g;y=%.9g;z=%.9g",
                  static_cast<double>(position.x), static_cast<double>(position.y),
                  static_cast<double>(position.z));
    return buffer;
}

// Joins the values into the verdict payload (`status` first: it is what every
// consumer reads before anything else).
std::string verdict_payload(const std::vector<std::string>& values) {
    std::string payload;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index > 0) {
            payload.push_back(kDebugFieldSeparator);
        }
        payload += values[index];
    }
    return payload;
}

std::vector<std::string> split_verdict(const std::string& payload) {
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start <= payload.size()) {
        const std::size_t end = payload.find(kDebugFieldSeparator, start);
        values.push_back(
            payload.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return values;
}

}  // namespace

// ---------------------------------------------------------------------------
// Wiring state. The engine-free core keeps its own state; this struct owns it
// and the adapter bookkeeping.
// ---------------------------------------------------------------------------
struct CCoreRuntime::SImpl {
    // --- profile (read once from the harness environment, F0-11) ------------
    bool active = false;
    bool host = false;
    bool started = false;
    // True when the profile asked for the real transport (default in a harness
    // run) instead of the in-process queues (`XRN_HARNESS_NET=inprocess`).
    bool udp = true;
    std::string role;
    std::string instance;
    std::string run_dir;
    std::string level = kDefaultLevel;
    // The map this profile starts on (`XRN_HARNESS_LEVEL`); every instance other
    // than the debug target map is this one (F1-13).
    std::string start_level = kDefaultLevel;
    std::string error;
    // "xnt 0.1.0 (abi N) udp host 127.0.0.1:47000 - backends: ..." (F0-02).
    std::string transport;

    // --- core ----------------------------------------------------------------
    std::unique_ptr<obs::EventLog> log;
    std::unique_ptr<WorldState> world;
    std::unique_ptr<env::EnvironmentAuthority> environment;
    std::unique_ptr<act::ActorReplicator> actors;
    std::unique_ptr<inv::InventoryAuthority> inventory;
    // In-process profile only: deterministic queues for local checks.
    std::unique_ptr<InProcessNetwork> net;
    // Network profile: the F0-02 endpoint (owns the xnt host/client object).
    std::unique_ptr<CTransport> endpoint;
    // In-process profile: the host endpoint the session borrows through `link`.
    std::unique_ptr<IMessageLink> owned_link;
    // Borrowed from `net`/`endpoint`; never deleted through this pointer.
    IMessageLink* link = nullptr;
    std::unique_ptr<HostSession> host_session;
    std::unique_ptr<ClientSession> client_session;
    // In-process profile: the host's own bot acts through a real ClientSession
    // on the in-process link (peer -> HostSession -> host authority -> verdict).
    // The network profile has no such peer: the slot budget (3 peer slots) is
    // reserved for the real client processes.
    std::unique_ptr<IMessageLink> peer_link;
    std::unique_ptr<ClientSession> peer_session;
    // Client read model: the mirrored snapshots are what a client process can
    // observe (inventory, world items, actors).
    act::ActorDirectory actor_view;
    inv::ItemDirectory item_view;
    std::uint32_t view_sequence = 0;
    bool view_ready = false;
    // Player id of this process' own character (host row / client slot).
    xrn::core::PlayerId local_player_id = kHostLocalPlayerId;

    // --- F0-09 build identity ------------------------------------------------
    // The manifest this process serves (host gate) or presents (client probe).
    // Both are the same deterministic harness build, so a normal join passes and
    // only an explicit debug divergence is refused.
    cfg::CfgManifest build;
    std::unique_ptr<cfg::Gate> cfg_gate;
    std::unique_ptr<cfg::Probe> cfg_probe;
    // The divergent paths the current debug probe asked for (event order + the
    // manifest the client presents on its next join).
    std::vector<std::string> probe_paths;
    // True while a probe-triggered (re)join is in flight: the host's verdict is
    // the answer of the check (`cfg_check` on success).
    bool probe_pending = false;

    // --- F1-13 group level change --------------------------------------------
    // Host: the authoritative transition (wired into `HostConfig::level`, so the
    // session ticks it and attributes peer intents).
    std::unique_ptr<lvl::LevelChangeAuthority> level_authority;
    // Client: the loading screen fed by the replicated journal.
    lvl::LoadScreen level_screen;
    bool level_screen_ready = false;
    std::uint32_t level_ack_sequence = 0;
    std::uint32_t level_event_sequence = 0;

    // --- engine bridge (F0-05) ----------------------------------------------
    // `coop::bridge()` (Lua) <-> core `BridgeLink` of the local session.
    struct CSink final : coop::IBridgeSink {
        SImpl* owner = nullptr;

        void on_outbound(const coop::SMessage& message) override {
            if (owner == nullptr) {
                return;
            }
            xrn::core::BridgeLink* const target = owner->local_bridge();
            if (target != nullptr) {
                target->on_outbound(xrn::core::from_engine(message));
            }
        }

        bool pull_inbound(coop::SMessage& message) override {
            if (owner == nullptr) {
                return false;
            }
            xrn::core::BridgeLink* const source = owner->local_bridge();
            if (source == nullptr) {
                return false;
            }
            BridgeMessage converted;
            if (!source->pull_inbound(converted)) {
                return false;
            }
            message = xrn::core::to_engine<coop::SMessage>(std::move(converted));
            return true;
        }
    };
    CSink sink;

    // --- bot channel (F0-12) -------------------------------------------------
    bot::CBotChannel channel;

    // --- run state -----------------------------------------------------------
    std::uint32_t env_sequence = 0;
    // Sequence of the F1-01 actor deltas on the Lua seam (`actor` channel).
    std::uint32_t actor_sequence = 0;
    std::uint32_t steps = 0;
    std::int64_t accumulator_ms = 0;
    std::int64_t last_state_ms = 0;
    std::int64_t last_flush_ms = 0;
    rep::EntityId debug_item = 0;
    std::uint32_t next_request = 0;
    std::uint32_t item_sequence = 0;
    bool lua_started = false;

    // F1-01: characters whose debug move is still "running" (the animation settles
    // to idle after `idle_at_ms`). One entry per slot, never one per frame.
    struct SActiveMove {
        rep::WriterSlot slot = rep::kHostWriter;
        std::int64_t idle_at_ms = 0;
    };
    std::vector<SActiveMove> moving;

    // The runner prepares the bot channel *after* the stand is up (a scenario
    // names its actors only when it runs), so the adapter re-checks the directory
    // at a modest interval instead of opening it once and never again.
    std::int64_t channel_probe_at_ms = 0;

    // Reconnect scenario (`debug.reconnect`): 0 idle, 1 dropped, 2 returning.
    int reconnect_phase = 0;
    bool reconnect_expired = false;
    std::int64_t reconnect_at_ms = 0;

    // F0-02: the debug command arrived from this transport peer (0 = local, the
    // host's own bot/Lua). A network drop is applied to exactly this peer.
    PeerId debug_requester = 0;
    // Host: the drop the current debug command asked for. It is applied *after*
    // the verdict left, so the client still answers its bot action.
    struct SPendingDrop {
        bool armed = false;
        PeerId peer = 0;
        xrn::core::PeerDrop mode = xrn::core::PeerDrop::KeepSlot;
        // Wall clock of the drop; 0 = right after the verdict (see the callers).
        std::int64_t due_ms = 0;
        // The debug command that armed it (named in a failure observation).
        const char* command = "debug.reconnect";
    };
    SPendingDrop pending_drop;
    // Host: peers whose join the build gate refused. Their transport session still
    // holds their address (the transport rekeys a peer only after its session
    // closed), so the host closes it once the refusal verdict left (F0-09).
    struct SRefusedPeer {
        PeerId peer = 0;
        std::int64_t due_ms = 0;
    };
    std::vector<SRefusedPeer> refused_peers;
    // Client: when to try starting a fresh transport session (F0-02 recovery).
    std::int64_t client_reconnect_at_ms = 0;
    // Client: the join verdict was already observed as an event (F0-09).
    bool rejection_seen = false;
    // Host -> clients observation stream sequence (F0-12).
    std::uint32_t obs_sequence = 0;

    // Authoritative counters: the derived observations of AC-1.12/AC-1.10 are
    // read from them, never from the adapter's own guesses.
    std::uint64_t seen_reclaims = 0;
    std::uint64_t seen_expired = 0;
    std::uint64_t seen_build_rejects = 0;

    struct SPendingAction {
        bot::SInboxMessage message;
        std::uint32_t request = 0;
        std::string item;
        std::string target;
        std::int64_t started_ms = 0;
        std::int64_t deadline_ms = 0;
        // The channel the verdict of this action arrives on: empty = the
        // inventory ack of a pickup (the core's own channel), otherwise one of
        // the adapter channels (debug / move / drop).
        std::string verdict_channel;
    };
    std::vector<SPendingAction> pending;

    // The pending action waiting for a verdict on `channel` with this request id.
    std::optional<std::size_t> pending_index(const std::string& channel,
                                             std::uint32_t request) const {
        for (std::size_t index = 0; index < pending.size(); ++index) {
            if (pending[index].verdict_channel == channel && pending[index].request == request) {
                return index;
            }
        }
        return std::nullopt;
    }

    xrn::core::BridgeLink* local_bridge() {
        if (host_session != nullptr) {
            return &host_session->bridge();
        }
        if (client_session != nullptr) {
            return &client_session->bridge();
        }
        return nullptr;
    }

    // The config of this process' client session. Built on every (re)connect, so
    // a probe that swapped the build manifest is presented on the next join.
    xrn::core::ClientConfig make_client_config() const {
        xrn::core::ClientConfig config;
        config.host_peer = kClientPeerId;
        config.player_id = local_player_id;
        config.player_name = instance;
        config.log = log.get();
        // F0-02 recovery: a lost link is not the end of the session (F1-10 decides).
        config.host_loss_grace_ms = kClientReconnectGraceMs;
        if (cfg_probe != nullptr) {
            // F0-09: the manifest of this build travels with the join.
            config.cfg_probe = cfg_probe.get();
        }
        return config;
    }

    void note_error(const std::string& text) {
        if (error.empty()) {
            error = text;
        }
    }

    // --- environment events -> observations + F0-07 log ---------------------
    //
    // F0-05 + F0-02: this is the host's single world-event stream. It reaches the
    // host's Lua layer (core -> Lua) and every client (bridge broadcast); a client
    // decodes the same canonical payload instead of reconstructing the event from
    // a snapshot diff, so counts and order match the host's stream exactly.
    void publish_environment_events(const std::vector<env::EnvironmentEvent>& events) {
        if (host_session == nullptr) {
            return;
        }
        for (const env::EnvironmentEvent& event : events) {
            std::string payload;
            if (failed(env::encode_environment_event(event, payload))) {
                continue;
            }
            BridgeMessage message;
            message.seq = ++env_sequence;
            message.channel = env::kBridgeChannel;
            message.payload = std::move(payload);
            static_cast<void>(host_session->bridge().push_inbound(message));
            static_cast<void>(host_session->broadcast_bridge(message));
        }
    }

    // Host: a session event its own bot observes is a world event for the
    // clients too (F1-10/F0-12) — the rejoin of a peer, the end of a window.
    void observe_session_event(const std::string& name) {
        channel.observe_event(name, {});
        if (host_session == nullptr) {
            return;
        }
        BridgeMessage message;
        message.seq = ++obs_sequence;
        message.channel = kObsEventChannel;
        message.payload = std::string(kObsEventField) + name;
        static_cast<void>(host_session->broadcast_bridge(message));
    }

    // Client: parse the host's observation event (`event=<name>`).
    static std::string obs_event_name(const std::string& payload) {
        const std::string prefix = kObsEventField;
        if (payload.rfind(prefix, 0) != 0) {
            return std::string();
        }
        return payload.substr(prefix.size());
    }

    // --- F1-13: level transitions -> observations + F0-07 log + Lua seam -----
    //
    // The host owns the transition: the authority queues its events (loading,
    // spawned, refused) and they cross the seam like every other world event —
    // once into the F0-07 log, once into the Lua channel and once into the bot
    // channel, so a bot observes the same group move a script does.
    void observe_level_events() {
        if (level_authority == nullptr) {
            return;
        }
        const std::vector<lvl::LevelEvent> events = level_authority->take_events();
        if (events.empty()) {
            return;
        }
        for (const lvl::LevelEvent& event : events) {
            std::string payload;
            if (failed(lvl::encode_level_event(event, payload))) {
                continue;
            }
            if (log != nullptr) {
                obs::Event log_event;
                log_event.tick = world != nullptr ? world->tick() : 0;
                log_event.kind = obs::EventKind::WorldMutation;
                log_event.detail = "level " + payload;
                log->push(std::move(log_event));
            }
            if (event.kind == lvl::LevelEventKind::Spawned) {
                // The group stands in the target instance: the map of every actor
                // (and of the host's own row) follows the world, never a counter.
                level = level_for_instance(event.target, start_level);
                channel.observe_event(
                    "level_change",
                    {{"level", bot::SJson::make_string(level)},
                     {"instance", bot::SJson::make_number(static_cast<double>(event.target))},
                     {"movers", bot::SJson::make_number(static_cast<double>(event.movers))}});
            } else if (event.kind == lvl::LevelEventKind::Refused) {
                channel.observe_event(
                    "level_refused",
                    {{"reason", bot::SJson::make_string(lvl::refusal_token(event.refusal))}});
            }
        }
        if (host_session != nullptr) {
            static_cast<void>(
                lvl::publish_level_events(host_session->bridge(), events, level_event_sequence));
        }
    }

    // What `command("debug.level_change")` acts for: the peer that sent the
    // command (F0-02), the loopback player of an in-process check, or the host's
    // own player when the host's bot asked.
    rep::WriterSlot level_requester() const {
        if (debug_requester != 0 && host_session != nullptr) {
            const std::optional<rep::WriterSlot> slot = host_session->slot_of(debug_requester);
            if (slot.has_value()) {
                return slot.value();
            }
        }
        if (peer_session != nullptr) {
            return peer_session->writer_slot();
        }
        return rep::kHostWriter;
    }

    // The crash of the transition window (`crash=<instance>`): the peer that asked
    // for it vanishes while the group loads, comes back and must exist exactly
    // once, in the target map, with its inventory (AC-3.3.2). A player can only
    // crash itself, never somebody else.
    bool arm_peer_crash(const std::string& who, std::string& detail) {
        if (who.empty()) {
            return true;
        }
        if (peer_session != nullptr) {
            // In-process profile: the loopback peer is this process' own client.
            if (who != instance + "-bot") {
                detail = "crash names another player: " + who;
                return false;
            }
            reconnect_expired = false;
            reconnect_phase = 1;
            reconnect_at_ms = 0;
            return true;
        }
        if (debug_requester == 0 || host_session == nullptr) {
            detail = "a crash needs a connected peer";
            return false;
        }
        for (const xrn::core::PeerInfo& peer : host_session->peers()) {
            if (peer.peer != debug_requester) {
                continue;
            }
            if (peer.player_name != who) {
                detail = "crash names another player: " + who;
                return false;
            }
            // The drop follows the verdict of the command (the peer still answers
            // its bot action), and the slot stays reserved, so the return resumes
            // the same character inside the window (F1-10).
            pending_drop.armed = true;
            pending_drop.peer = peer.peer;
            pending_drop.mode = xrn::core::PeerDrop::KeepSlot;
            pending_drop.due_ms = epoch_ms() + kDebugCfgDropDelayMs;
            return true;
        }
        detail = "crash names a peer that is not in the session: " + who;
        return false;
    }

    void observe_environment_event(const env::EnvironmentEvent& event) {
        std::string payload;
        if (failed(env::encode_environment_event(event, payload))) {
            return;
        }

        obs::Event log_event;
        // The tick of the event: authoritative on the host, the last applied
        // authoritative tick on a client. Both processes then merge onto one
        // timeline (F0-14) instead of one side logging under tick 0.
        log_event.tick = host_session != nullptr
                             ? (world != nullptr ? world->tick() : 0)
                             : (client_session != nullptr ? client_session->mirror().tick() : 0);
        log_event.detail = payload;

        switch (event.kind) {
        case env::EnvironmentEventKind::WeatherChange:
            log_event.kind = obs::EventKind::Weather;
            channel.observe_event(
                "weather", {{"world_ms", bot::SJson::make_number(
                                             static_cast<double>(event.weather.changed_at_ms))}});
            break;
        case env::EnvironmentEventKind::Emission:
            log_event.kind = obs::EventKind::Emission;
            // AC-1.6.2 (QA-F5): one `emission_start` and one `emission_end` per
            // cycle. The warning is the siren phase *of the same cycle*, so it is
            // not a second start: it stays in the F0-07 log (and the Lua layer,
            // which drives the siren, sees the raw transition).
            if (event.emission.transition == env::EmissionTransition::Start) {
                channel.observe_event(
                    "emission_start",
                    {{"cycle", bot::SJson::make_number(static_cast<double>(event.emission.cycle))},
                     {"world_ms",
                      bot::SJson::make_number(static_cast<double>(event.emission.at_ms))}});
            } else if (event.emission.transition == env::EmissionTransition::End) {
                channel.observe_event(
                    "emission_end",
                    {{"cycle", bot::SJson::make_number(static_cast<double>(event.emission.cycle))},
                     {"world_ms",
                      bot::SJson::make_number(static_cast<double>(event.emission.at_ms))}});
            }
            break;
        case env::EnvironmentEventKind::ClockJump:
            // There is no separate F0-07 kind for a host clock jump: the event
            // is a world mutation of the global clock record.
            log_event.kind = obs::EventKind::WorldMutation;
            log_event.detail = "clock jump " + payload;
            channel.observe_event(
                "time_sync",
                {{"world_ms", bot::SJson::make_number(static_cast<double>(event.clock.world_ms))}});
            break;
        }

        if (log != nullptr) {
            log->push(std::move(log_event));
        }
    }

    // --- inventory events -> observations + Lua seam (F1-02) ----------------
    //
    // The authority is the single source of truth for "the item moved". The host
    // bot channel observes the pickups here (in the in-process profile the
    // loopback peer's ack did it), and the same events go to the Lua seam.
    void observe_item_events() {
        if (inventory == nullptr) {
            return;
        }
        const std::vector<inv::ItemEvent> events = inventory->take_events();
        if (events.empty()) {
            return;
        }
        for (const inv::ItemEvent& event : events) {
            if (event.kind == inv::ItemEventKind::PickedUp) {
                channel.observe_event(
                    "pickup",
                    {{"item", bot::SJson::make_string(event.view.item)},
                     {"count", bot::SJson::make_number(static_cast<double>(event.view.count))}});
            } else if (event.kind == inv::ItemEventKind::Dropped) {
                channel.observe_event(
                    "drop",
                    {{"item", bot::SJson::make_string(event.view.item)},
                     {"count", bot::SJson::make_number(static_cast<double>(event.view.count))}});
            }
        }
        if (host_session != nullptr) {
            static_cast<void>(
                inv::publish_item_events(host_session->bridge(), events, item_sequence));
        }
    }

    // --- F1-01/F1-02: peer intents (movement, item drop) ----------------------
    //
    // A character and the items a player holds are host-authoritative. A client
    // sends what the player *wants* on its own bridge channel; the host resolves
    // the sender's slot here (never the payload) and applies the core authority.
    // The verdict answers the sender only, so a bot action is `done` exactly when
    // the host accepted it (NFR-ARCH-1/5).

    struct SRequester {
        rep::WriterSlot slot = rep::kHostWriter;
        xrn::core::PlayerId player = xrn::core::kAnonymousPlayerId;
    };

    // `peer` is attributed by the session: the intent acts for the sender's own
    // character, never for a slot or a player id the payload names.
    bool requester_of(PeerId peer, SRequester& out) const {
        if (host_session == nullptr) {
            return false;
        }
        const std::optional<rep::WriterSlot> slot = host_session->slot_of(peer);
        if (!slot.has_value()) {
            return false;
        }
        for (const xrn::core::SessionPlayer& player : host_session->players()) {
            if (player.slot != slot.value()) {
                continue;
            }
            out.slot = player.slot;
            out.player = player.player_id;
            return player.state == xrn::core::PlayerState::Connected &&
                   player.player_id != xrn::core::kAnonymousPlayerId;
        }
        return false;
    }

    // F1-01: the host moves the character of `slot` to the requested point. The
    // debug move is instantaneous (the scenario asserts an eventual position), so
    // the run animation is held for one beat and then settles to idle: a remote
    // observer sees both the position and the animation change.
    bool apply_move(rep::WriterSlot slot, const act::Vec3& position, std::string& detail) {
        if (actors == nullptr) {
            detail = "no actor authority";
            return false;
        }
        const std::optional<act::ActorView> current = actors->view(slot);
        const float yaw = current.has_value() ? current->yaw : 0.0F;
        const CoreError moved = actors->move(slot, position, yaw);
        if (failed(moved)) {
            detail = std::string("move refused: ") + xrn::core::to_string(moved);
            return false;
        }
        static_cast<void>(actors->set_animation(slot, act::Animation::Run, 0.0F));
        const std::int64_t idle_at = epoch_ms() + kMoveAnimationMs;
        for (SActiveMove& entry : moving) {
            if (entry.slot == slot) {
                entry.idle_at_ms = idle_at;
                return true;
            }
        }
        moving.push_back(SActiveMove{slot, idle_at});
        return true;
    }

    // The animation of a moved character settles back to idle once the beat is
    // over (F1-01: only changes cross the seam, never per-frame state).
    void service_moves(std::int64_t now) {
        if (actors == nullptr) {
            moving.clear();
            return;
        }
        for (std::size_t index = 0; index < moving.size();) {
            if (now < moving[index].idle_at_ms) {
                ++index;
                continue;
            }
            static_cast<void>(
                actors->set_animation(moving[index].slot, act::Animation::Idle, 0.0F));
            moving.erase(moving.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    // F1-01 Lua seam: the actor deltas of the authority reach the gameplay layer
    // on `actor::kBridgeChannel` (the same event-driven path as the items and the
    // environment, NFR-ARCH-10). Draining the queue here also keeps it bounded.
    void publish_actor_changes() {
        if (actors == nullptr) {
            return;
        }
        const std::vector<act::ActorEvent> events = actors->take_events();
        if (events.empty() || host_session == nullptr) {
            return;
        }
        static_cast<void>(
            act::publish_actor_events(host_session->bridge(), events, actor_sequence));
    }

    // F1-02: the host puts an item held by `requester` back into the world at the
    // character's own position. The item entity is the same one for its whole
    // life, so a drop can never duplicate it (AC-1.3.1).
    inv::PickupVerdict apply_drop(const SRequester& requester, rep::EntityId item,
                                  std::string& detail) {
        inv::PickupVerdict verdict;
        verdict.item = item;
        if (inventory == nullptr) {
            verdict.status = inv::PickupStatus::InvalidRequest;
            detail = "no inventory authority";
            return verdict;
        }
        inv::Vec3 position{};
        bool placed = false;
        if (actors != nullptr) {
            const std::optional<act::ActorView> view = actors->view(requester.slot);
            if (view.has_value()) {
                position = view->position;
                placed = true;
            }
        }
        if (!placed) {
            const std::optional<inv::ItemView> known = inventory->item(item);
            if (!known.has_value()) {
                verdict.status = inv::PickupStatus::UnknownItem;
                detail = "no such item in the world";
                return verdict;
            }
            position = known->position;
        }
        verdict = inventory->drop(requester.slot, requester.player, item, position);
        detail = inv::to_string(verdict.status);
        return verdict;
    }

    // --- connection lifecycle -> observations (F0-08/F1-10/F0-09) -----------
    void observe_session_deltas() {
        if (host_session == nullptr) {
            return;
        }
        const xrn::core::SessionStats& stats = host_session->stats();

        if (stats.reconnects_reclaimed > seen_reclaims) {
            seen_reclaims = stats.reconnects_reclaimed;
            observe_session_event("player_rejoin");
        }
        if (stats.resumes_expired > seen_expired) {
            seen_expired = stats.resumes_expired;
            observe_session_event("window_expired");
        }
        if (stats.builds_rejected > seen_build_rejects) {
            seen_build_rejects = stats.builds_rejected;
            // Only the in-process profile has the loopback peer whose refusal
            // paths this process owns. Over the network the refused client is not
            // in the session (a broadcast cannot reach it), so it observes its own
            // refusal from the verdict it received (see `observe_client_rejection`).
            if (peer_session != nullptr) {
                channel.observe_event_files("join_rejected", peer_session->rejection_paths(),
                                            "cfg_diff");
            }
        }
    }

    // --- debug reconnect scenario (AC-1.12) ---------------------------------
    //
    // The in-process link keeps events for a peer id until somebody polls them:
    // a peer that vanished must be drained, otherwise its successor would read
    // the *previous* Disconnected event and end before joining. A network
    // endpoint is destroyed instead, so there is nothing to drain.
    void drain_peer_inbox() {
        if (net == nullptr) {
            return;
        }
        std::unique_ptr<IMessageLink> scratch = net->endpoint(kLoopbackPeerId);
        std::vector<xrn::core::LinkEvent> events;
        while (scratch->poll(events)) {
            events.clear();
        }
    }

    // Creates the host's own bot as a real client of the host session. Only the
    // in-process profile has it: over the network the peer slots belong to the
    // real client processes (host + 3 clients = the 4-slot session).
    bool start_local_peer() {
        if (net == nullptr) {
            return false;
        }
        xrn::core::ClientConfig config;
        config.host_peer = kHostPeerId;
        config.player_id = kLoopbackPeerId;
        config.player_name = instance + "-bot";
        peer_link = net->endpoint(kLoopbackPeerId);
        peer_session.reset(new ClientSession(*peer_link, config));
        net->connect(kHostPeerId, kLoopbackPeerId);
        return true;
    }

    void drop_local_peer() {
        if (net != nullptr) {
            // Drop the loopback peer exactly like a lost client process.
            net->disconnect(kHostPeerId, kLoopbackPeerId);
        }
        peer_session.reset();
        peer_link.reset();
        drain_peer_inbox();
    }

    void step_reconnect_cycle(std::int64_t now) {
        if (reconnect_phase == 1) {
            drop_local_peer();
            reconnect_at_ms =
                now +
                (reconnect_expired ? static_cast<std::int64_t>(kDebugReconnectWindowMs) + 150 : 40);
            reconnect_phase = 2;
            return;
        }
        if (reconnect_phase == 2 && now >= reconnect_at_ms) {
            drain_peer_inbox();
            static_cast<void>(start_local_peer());
            reconnect_phase = 0;
        }
    }

    // --- client read model (F0-02: what a snapshot looks like on a client) ---
    //
    // The core applies snapshots/deltas into `ClientMirror`; the directories turn
    // those records into the observable state the bot channel and the Lua layer
    // read. Refreshed only when the authoritative sequence moved (the host
    // publishes every 100 ms), never per frame.
    void refresh_client_view() {
        if (client_session == nullptr) {
            return;
        }
        const std::uint32_t sequence = client_session->mirror().last_sequence();
        if (view_ready && sequence == view_sequence) {
            return;
        }
        view_ready = true;
        view_sequence = sequence;
        const std::vector<rep::EntityRecord> records = client_session->mirror().records();
        actor_view.refresh(records, local_player_id);
        item_view.refresh(records, local_player_id);
        // F1-13: the journal travels in the same records, so the loading screen is
        // refreshed from the same snapshot the view was built from.
        level_screen.refresh(records, local_player_id);
        level_screen_ready = true;
    }

    // --- F1-13: level of a character -----------------------------------------
    //
    // The level of a player is the map of the instance its character lives in
    // (the transition rekeys the records, so a snapshot is enough — the level is
    // never a replicated string that could disagree with the world).
    std::string level_of_slot(rep::WriterSlot slot) const {
        if (actors != nullptr) {
            const std::optional<act::ActorView> view = actors->view(slot);
            if (view.has_value()) {
                return level_for_instance(view->key.instance, start_level);
            }
        }
        return level;
    }

    // The character of this process' own player, once it is in the world.
    std::optional<act::ActorView> local_actor_view() const {
        if (host_session != nullptr && actors != nullptr) {
            for (const xrn::core::SessionPlayer& player : host_session->players()) {
                if (player.player_id != local_player_id ||
                    player.state != xrn::core::PlayerState::Connected) {
                    continue;
                }
                return actors->view(player.slot);
            }
            return std::nullopt;
        }
        if (client_session != nullptr) {
            for (const act::ActorView& view : actor_view.all()) {
                if (view.player_id == local_player_id) {
                    return view;
                }
            }
        }
        return std::nullopt;
    }

    // Client: the level this process' own character is in. Before the character
    // is in the mirror (still joining) the last known level stands.
    std::string client_level() const {
        const std::vector<act::ActorView>& views = actor_view.all();
        for (const act::ActorView& view : views) {
            if (view.player_id == local_player_id) {
                return level_for_instance(view.key.instance, start_level);
            }
        }
        if (!views.empty()) {
            return level_for_instance(views.front().key.instance, start_level);
        }
        return level;
    }

    // --- F1-13: client loading screen + acknowledgement ----------------------
    //
    // The host commits once every mover acknowledged the token or the load budget
    // expired (AC-3.3.3/AC-3.3.4). The ack is the real one of the protocol: a
    // client says "the target map is up" through the same seam a script would.
    void service_level_screen() {
        if (client_session == nullptr) {
            return;
        }
        // The journal and the characters arrive in the same records; the view
        // refresh is sequence-gated, so this is a no-op while nothing changed.
        refresh_client_view();
        if (!level_screen_ready) {
            return;
        }
        const std::uint32_t token = level_screen.pending_token();
        if (token == lvl::kNoToken) {
            return;
        }
        std::string payload;
        if (failed(lvl::encode_level_ack(token, payload))) {
            return;
        }
        BridgeMessage message;
        message.seq = ++level_ack_sequence;
        message.channel = lvl::kAckChannel;
        message.payload = std::move(payload);
        client_session->bridge().on_outbound(message);
        level_screen.ack(token);
    }

    // --- F0-02 client recovery ----------------------------------------------
    //
    // The transport never reconnects behind the owner's back and a listen host
    // owns the session (EC-4), so the client adapter is the place that recovers a
    // lost link: a fresh transport session on the same identity, then the host's
    // reconnect policy (F1-10) decides — the same character inside the window,
    // `window_expired` outside it. Only a closed transport session is restarted:
    // a running handshake must not be reset every tick.
    void service_client_link(std::int64_t now) {
        if (client_session == nullptr || endpoint == nullptr) {
            return;
        }
        if (!client_session->reconnecting() || !endpoint->client_closed()) {
            return;
        }
        if (now < client_reconnect_at_ms) {
            return;
        }
        client_reconnect_at_ms = now + kClientReconnectRetryMs;
        if (!endpoint->reconnect_client()) {
            note_error("transport reconnect: " + endpoint->error());
        }
    }

    // F0-09: a refused client is not in the session, so the host cannot send it
    // anything. The client observes its own refusal — with the divergent paths,
    // which only it knows (AC-1.10.1).
    void observe_client_rejection() {
        if (client_session == nullptr || rejection_seen || !client_session->join_rejected()) {
            return;
        }
        rejection_seen = true;
        probe_pending = false;
        const CoreError reason = client_session->join_rejection();
        if (reason == CoreError::BuildMismatch) {
            // The paths come from the host's verdict (decoded by the core probe);
            // only their order is the one the player asked for.
            channel.observe_event_files(
                "join_rejected", order_like(probe_paths, client_session->rejection_paths()),
                "cfg_diff");
            return;
        }
        const std::string text = xrn::core::to_string(reason);
        channel.observe_event("join_rejected", {{"reason", bot::SJson::make_string(text)}});
    }

    // F0-09: the verdict of a probe-triggered join is the answer of the build
    // check: an accepted join is exactly "the builds match" (AC-1.10.2).
    void observe_client_join() {
        if (!probe_pending || client_session == nullptr) {
            return;
        }
        if (!client_session->join_accepted()) {
            return;
        }
        probe_pending = false;
        channel.observe_event("cfg_check", {{"result", bot::SJson::make_string("match")}});
    }

    // --- F0-09 debug build probe (client side) -------------------------------
    //
    // The build belongs to the client, so the probe is applied here: the manifest
    // this process presents is swapped and the host decides at the join step
    // (a divergent build never reaches the session, AC-1.10.1). Nothing is faked:
    // the real `cfgdiff::Probe`/`Gate` pair does the comparison.
    //
    //   * the build matches and the session runs: the join already verified this
    //     exact build, so the probe answers with `cfg_check` and moves nothing;
    //   * the build diverges: the probe goes to the host as a normal debug intent,
    //     which drops this peer so it re-joins with the build it now presents; the
    //     host's verdict is the refusal, with the divergent paths;
    //   * no session runs (a refused build): the player fixed it and connects
    //     again — the probe is that reconnection.
    enum class SProbeOutcome { Local, Forward, Failed };

    SProbeOutcome probe_build(const std::string& argument, std::string& detail) {
        detail.clear();
        const std::vector<std::string> paths =
            split_probe_paths(argument_value(argument, "mismatch"));
        probe_paths = paths;
        const cfg::CfgManifest next = paths.empty() ? build : diverge_build(build, paths);
        const bool diverges = !cfg::compare(build, next).accepted();
        if (cfg_probe != nullptr) {
            cfg_probe->update_manifest(next);
        }
        const bool live = client_session != nullptr && !client_session->session_ended() &&
                          client_session->join_accepted();
        if (!diverges) {
            if (live) {
                // The host accepted this very build at the join (F0-09), so the
                // check is a confirmation and nothing is refused.
                channel.observe_event("cfg_check",
                                      {{"result", bot::SJson::make_string("match")}});
                detail = "builds match";
                return SProbeOutcome::Local;
            }
            if (!restart_client_session()) {
                detail = "no transport to re-check the build";
                return SProbeOutcome::Failed;
            }
            probe_pending = true;
            detail = "re-checking the current build at the join";
            return SProbeOutcome::Local;
        }
        if (cfg_probe == nullptr) {
            detail = "this profile does not verify builds";
            return SProbeOutcome::Failed;
        }
        if (live) {
            // Divergent build and a running session: the host refuses it and drops
            // this peer for the real re-check (see `command("debug.cfg_probe")`).
            detail = "divergent build sent to the host";
            return SProbeOutcome::Forward;
        }
        if (!restart_client_session()) {
            detail = "no transport to re-check the build";
            return SProbeOutcome::Failed;
        }
        probe_pending = true;
        detail = "divergent build re-checked at the join";
        return SProbeOutcome::Local;
    }

    // F0-08: a refused join ends the client session (the player is out). The debug
    // surface is the player fixing the build and connecting again: a fresh session
    // on the same identity, so the host decides afresh (F1-10 governs the slot).
    bool restart_client_session() {
        if (endpoint == nullptr || link == nullptr) {
            return false;
        }
        client_session.reset();
        rejection_seen = false;
        view_ready = false;
        view_sequence = 0;
        level_screen.clear();
        level_screen_ready = false;
        client_session.reset(new ClientSession(*link, make_client_config()));
        return endpoint->reconnect_client();
    }

    // --- debug surface over the network (F0-02) ------------------------------
    //
    // A debug command is a host-side mechanic: the client sends it as an intent
    // (`kDebugIntentChannel`), the host executes it and broadcasts the verdict
    // (`kDebugAckChannel`). Only armed in the harness profile, like the surface
    // itself; the payload never crosses into Lua.
    static std::string encode_debug(const std::string& name, const std::string& argument) {
        return name + kDebugFieldSeparator + argument;
    }

    static void split_debug(const std::string& payload, std::string& name, std::string& argument) {
        const std::size_t separator = payload.find(kDebugFieldSeparator);
        if (separator == std::string::npos) {
            name = payload;
            argument.clear();
            return;
        }
        name = payload.substr(0, separator);
        argument = payload.substr(separator + 1);
    }

    // Host: executes the intents peers sent and answers them. Everything else
    // stays in the local bridge for the Lua layer (the queue is drained
    // destructively, so foreign messages are handed over explicitly).
    //
    // Every intent is a *request*, never a write: the payload names what the
    // player wants, the host resolves the sender's slot (see `requester_of`) and
    // answers the sender only. A peer that is not in the session is refused
    // instead of guessed.
    void drain_host_intents() {
        if (host_session == nullptr) {
            return;
        }
        std::vector<BridgeMessage> foreign;
        BridgeMessage message;
        while (host_session->bridge().pull_inbound(message)) {
            if (message.channel == kMoveIntentChannel) {
                handle_move_intent(message);
                continue;
            }
            if (message.channel == kDropIntentChannel) {
                handle_drop_intent(message);
                continue;
            }
            if (message.channel != kDebugIntentChannel) {
                foreign.push_back(std::move(message));
                continue;
            }
            // The intent is executed for the peer that sent it (F0-02): a drop
            // must hit exactly that player, never "whoever is around".
            debug_requester = message.from;
            std::string name;
            std::string argument;
            split_debug(message.payload, name, argument);
            std::string detail;
            const bool accepted = coop::core_runtime().command(name, argument, detail);
            debug_requester = 0;
            if (accepted) {
                channel.observe_event("debug_command",
                                      {{"command", bot::SJson::make_string(name)},
                                       {"detail", bot::SJson::make_string(detail)}});
            } else {
                // Honest answer: the surface exists, the mechanic is not wired.
                channel.observe_event("debug_unsupported",
                                      {{"command", bot::SJson::make_string(name)},
                                       {"reason", bot::SJson::make_string(detail)}});
            }
            BridgeMessage ack;
            ack.seq = message.seq;
            ack.channel = kDebugAckChannel;
            ack.payload = std::string(accepted ? "done" : "failed") + kDebugFieldSeparator + detail;
            static_cast<void>(host_session->broadcast_bridge(ack));
            // A drop the command asked for is applied after its verdict left, but
            // never earlier than the command's own due time (see SPendingDrop).
            apply_pending_drop(epoch_ms());
        }
        for (BridgeMessage& kept : foreign) {
            coop::bridge().enqueue(xrn::core::to_engine<coop::SMessage>(std::move(kept)));
        }
    }

    // The answer of one intent: the sender only, on the intent's own ack channel.
    // The host's own bot never sends an intent — its actions are applied directly
    // in `handle_action` — so every verdict here has a peer to answer.
    void answer_intent(PeerId peer, const BridgeMessage& request, const char* channel,
                       const std::vector<std::string>& values) {
        if (host_session == nullptr) {
            return;
        }
        BridgeMessage ack;
        ack.seq = request.seq;
        ack.channel = channel;
        ack.payload = verdict_payload(values);
        static_cast<void>(host_session->send_bridge_to(peer, ack));
    }

    // F1-01 (US-1.2): `x=..;y=..;z=..` -> the character of the sender moves
    // there. The slot comes from the session; a position outside the world is a
    // protocol error and changes nothing.
    void handle_move_intent(const BridgeMessage& message) {
        const PeerId peer = message.from;
        SRequester requester;
        std::string detail;
        std::string status = "applied";
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        if (!requester_of(peer, requester)) {
            status = "refused";
            detail = "the sender has no character in this session";
        } else if (!intent_number(message.payload, "x", x) ||
                   !intent_number(message.payload, "y", y) ||
                   !intent_number(message.payload, "z", z)) {
            status = "refused";
            detail = "the intent carries no finite position";
        } else {
            const float limit = 100000.0F;
            if (std::fabs(x) > limit || std::fabs(y) > limit || std::fabs(z) > limit) {
                status = "refused";
                detail = "the requested position is outside the world";
            } else {
                const act::Vec3 position{static_cast<float>(x), static_cast<float>(y),
                                         static_cast<float>(z)};
                if (apply_move(requester.slot, position, detail)) {
                    channel.observe_event(
                        "move", {{"x", bot::SJson::make_number(x)},
                                 {"y", bot::SJson::make_number(y)},
                                 {"z", bot::SJson::make_number(z)}});
                    detail = "character moved";
                } else {
                    status = "refused";
                }
            }
        }
        answer_intent(peer, message, kMoveAckChannel, {status, detail});
    }

    // F1-02 (US-1.3): `item=<entity>` -> the sender's own item goes back into the
    // world. The entity is resolved by the client from its mirror (the class it
    // sees), the authority decides whether this player really holds it.
    void handle_drop_intent(const BridgeMessage& message) {
        const PeerId peer = message.from;
        SRequester requester;
        std::string detail;
        double raw_item = 0.0;
        inv::PickupVerdict verdict;
        if (!requester_of(peer, requester)) {
            verdict.status = inv::PickupStatus::InvalidRequest;
            detail = "the sender has no character in this session";
        } else if (!intent_number(message.payload, "item", raw_item) || raw_item < 1.0) {
            verdict.status = inv::PickupStatus::InvalidRequest;
            detail = "the intent carries no item";
        } else {
            const rep::EntityId item = static_cast<rep::EntityId>(raw_item);
            verdict = apply_drop(requester, item, detail);
        }
        if (verdict.applied()) {
            channel.observe_event(
                "drop", {{"item", bot::SJson::make_string(verdict.item_class)},
                         {"count", bot::SJson::make_number(static_cast<double>(verdict.count))}});
        } else {
            channel.observe_event(
                "drop_rejected", {{"item", bot::SJson::make_string(verdict.item_class)},
                                  {"reason", bot::SJson::make_string(
                                                 std::string(inv::to_string(verdict.status)))}});
        }
        answer_intent(peer, message, kDropAckChannel,
                      {verdict.applied() ? "applied" : "refused", verdict.item_class,
                       std::to_string(verdict.count), inv::to_string(verdict.status)});
    }

    // The verdict is already on the wire, so the peer can answer its bot action
    // even though the host closes its link right after (F0-02).
    void apply_pending_drop(std::int64_t now) {
        if (!pending_drop.armed) {
            return;
        }
        if (now < pending_drop.due_ms) {
            return;  // still on the wire: the peer answers its action first
        }
        const SPendingDrop drop = pending_drop;
        pending_drop = SPendingDrop{};
        if (host_session == nullptr) {
            return;
        }
        const CoreError result = host_session->drop_peer(drop.peer, drop.mode);
        if (failed(result)) {
            note_error(std::string("debug drop: cannot drop the peer: ") +
                       xrn::core::to_string(result));
            channel.observe_event(
                "debug_unsupported",
                {{"command", bot::SJson::make_string(drop.command)},
                 {"reason", bot::SJson::make_string(std::string("drop failed: ") +
                                                    xrn::core::to_string(result))}});
        }
    }

    // F0-09: a peer whose build the gate refused is not in the session, but its
    // transport session still holds its address — and the transport rekeys a peer
    // only after that session closed. The host closes it once the refusal verdict
    // left, so the player can fix the build and connect again right away instead
    // of waiting out the transport timeout (AC-1.10.3).
    void close_refused_peers(std::int64_t now) {
        if (host_session == nullptr || link == nullptr) {
            refused_peers.clear();
            return;
        }
        const std::vector<xrn::core::PeerInfo> peers = host_session->peers();
        for (const xrn::core::PeerInfo& peer : peers) {
            if (peer.handshake_ok || peer.ended ||
                peer.join_rejection != CoreError::BuildMismatch) {
                continue;
            }
            bool known = false;
            for (const SRefusedPeer& entry : refused_peers) {
                known = known || entry.peer == peer.peer;
            }
            if (!known) {
                refused_peers.push_back(
                    SRefusedPeer{peer.peer, now + kDebugCfgDropDelayMs * 2});
            }
        }
        for (std::size_t index = 0; index < refused_peers.size();) {
            const SRefusedPeer entry = refused_peers[index];
            bool present = false;
            for (const xrn::core::PeerInfo& peer : peers) {
                present = present || (peer.peer == entry.peer && !peer.ended);
            }
            if (!present) {
                refused_peers.erase(refused_peers.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (now < entry.due_ms) {
                ++index;
                continue;
            }
            static_cast<void>(link->disconnect(entry.peer));
            refused_peers.erase(refused_peers.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    // --- bot actions (F0-12) -------------------------------------------------
    //
    // The scenario names an item class ("bread"), the wire carries ids. A client
    // has no authoritative inventory: it resolves the class in what it mirrors —
    // the items on the ground for a pickup, its own inventory for a drop. The host
    // resolves it in its own authority.
    rep::EntityId resolve_item(const std::string& item_class, bool held = false) {
        if (inventory != nullptr) {
            if (held) {
                for (const inv::ItemView& item : inventory->inventory_of(local_player_id)) {
                    if (item.item == item_class) {
                        return item.key.entity;
                    }
                }
            }
            for (const inv::ItemView& item : inventory->world_items()) {
                if (item.item == item_class) {
                    return item.key.entity;
                }
            }
        }
        if (held) {
            for (const inv::ItemView& item : item_view.inventory()) {
                if (item.item == item_class) {
                    return item.key.entity;
                }
            }
        }
        for (const inv::ItemView& item : item_view.world_items()) {
            if (item.item == item_class) {
                return item.key.entity;
            }
        }
        return 0;
    }

    void answer(const bot::SInboxMessage& message, const char* status, std::int64_t started_ms,
                const std::string& failure = std::string()) {
        channel.ack(message, status, std::max<std::int64_t>(0, epoch_ms() - started_ms), failure);
    }

    void handle_action(const bot::SInboxMessage& message, std::int64_t now) {
        const bot::SAction& action = message.action;
        const std::int64_t started = message.sent_at_ms > 0 ? message.sent_at_ms : now;
        // A bot acts on what it can see: the mirrored state must be current when
        // the action names an item (a client resolves classes through it).
        refresh_client_view();

        if (action.type == "send" || action.type == "net_action") {
            std::string detail;
            const std::size_t space = action.command.find(' ');
            const std::string name = action.command.substr(0, space);
            const std::string argument =
                space == std::string::npos ? std::string() : action.command.substr(space + 1);
            if (name.empty()) {
                answer(message, "failed", started, "empty command");
                return;
            }
            if (!host && name == "debug.cfg_probe") {
                // F0-09: the build probe belongs to the side that owns the build,
                // so the client applies it and the host decides at the join step.
                // Only a divergent build of a running session travels as an intent
                // (below): the host refuses it and drops this peer for the real
                // re-check, which is what the client then observes.
                const SProbeOutcome outcome = probe_build(argument, detail);
                if (outcome == SProbeOutcome::Failed) {
                    channel.observe_event(
                        "debug_unsupported",
                        {{"command", bot::SJson::make_string(name)},
                         {"reason", bot::SJson::make_string(detail)}});
                    answer(message, "done", started);
                    return;
                }
                if (outcome == SProbeOutcome::Local) {
                    channel.observe_event(
                        "debug_command", {{"command", bot::SJson::make_string(name)},
                                          {"detail", bot::SJson::make_string(detail)}});
                    answer(message, "done", started);
                    return;
                }
                // Forward: the shared intent path answers when the host decided.
            }
            if (!host) {
                // F0-02: a debug command is a host-side mechanic. A client sends
                // it as an intent over the session bridge and answers its bot only
                // when the host's verdict arrives over the network.
                if (client_session == nullptr || !client_session->join_accepted()) {
                    answer(message, "failed", started, "no session for the debug intent");
                    return;
                }
                BridgeMessage intent;
                intent.seq = ++next_request;
                intent.channel = kDebugIntentChannel;
                intent.payload = encode_debug(name, argument);
                client_session->bridge().on_outbound(intent);

                SPendingAction pending_action;
                pending_action.message = message;
                pending_action.request = intent.seq;
                pending_action.started_ms = started;
                pending_action.deadline_ms = now + kAckTimeoutMs;
                pending_action.verdict_channel = kDebugAckChannel;
                pending.push_back(std::move(pending_action));
                return;
            }
            if (!coop::core_runtime().command(name, argument, detail)) {
                // Honest answer: the surface exists, the mechanic is not wired
                // yet. The scenario's assertion on the world event fails, which
                // is exactly what a missing mechanic must look like.
                channel.observe_event("debug_unsupported",
                                      {{"command", bot::SJson::make_string(action.command)},
                                       {"reason", bot::SJson::make_string(detail)}});
                answer(message, "done", started);
                return;
            }
            channel.observe_event("debug_command", {{"command", bot::SJson::make_string(name)},
                                                    {"detail", bot::SJson::make_string(detail)}});
            answer(message, "done", started);
            return;
        }

        if (action.type == "pickup") {
            const rep::EntityId item = action.item.empty() ? debug_item : resolve_item(action.item);
            if (item == 0) {
                channel.observe_event("pickup_rejected",
                                      {{"item", bot::SJson::make_string(action.item)},
                                       {"reason", bot::SJson::make_string("no_item")}});
                answer(message, "done", started);
                return;
            }
            if (peer_session == nullptr && client_session == nullptr) {
                // Network profile: the host bot acts as the host's own player
                // (slot 0). No loopback peer exists there - the peer slots belong
                // to the real client processes - so the intent goes straight to
                // the authority that owns the item.
                if (inventory == nullptr) {
                    answer(message, "failed", started, "no inventory authority");
                    return;
                }
                inv::PickupRequest request;
                request.slot = rep::kHostWriter;
                request.player_id = kHostLocalPlayerId;
                request.request = ++next_request;
                request.item = item;
                if (actors != nullptr) {
                    request.picker_position = actors->spawn_position(rep::kHostWriter);
                }
                const inv::PickupVerdict verdict = inventory->pickup(request);
                if (verdict.applied()) {
                    channel.observe_event(
                        "pickup",
                        {{"item", bot::SJson::make_string(verdict.item_class)},
                         {"count", bot::SJson::make_number(static_cast<double>(verdict.count))}});
                } else {
                    channel.observe_event(
                        "pickup_rejected",
                        {{"item",
                          bot::SJson::make_string(verdict.item_class.empty() ? action.item
                                                                             : verdict.item_class)},
                         {"reason", bot::SJson::make_string(inv::to_string(verdict.status))}});
                }
                answer(message, "done", started);
                return;
            }
            inv::PickupIntent intent;
            intent.request = ++next_request;
            intent.item = item;
            std::string payload;
            if (failed(inv::encode_pickup_intent(intent, payload))) {
                answer(message, "failed", started, "cannot encode the intent");
                return;
            }
            BridgeMessage intent_message;
            intent_message.seq = next_request;
            intent_message.channel = inv::kPickupChannel;
            intent_message.payload = payload;

            xrn::core::BridgeLink* const target =
                peer_session != nullptr ? &peer_session->bridge() : local_bridge();
            if (target == nullptr) {
                answer(message, "failed", started, "no session for the intent");
                return;
            }
            target->on_outbound(intent_message);

            SPendingAction pending_action;
            pending_action.message = message;
            pending_action.request = intent.request;
            pending_action.item = action.item;
            pending_action.started_ms = started;
            pending_action.deadline_ms = now + kAckTimeoutMs;
            pending.push_back(std::move(pending_action));
            return;
        }

        if (action.type == "move_to" || action.type == "teleport") {
            // F1-01 (US-1.2): a character is host-authoritative. The bot sends
            // where the player wants to go; the host moves the actor record of the
            // sender's slot, and every client sees the new position from the
            // replicated state (never from a local guess).
            if (!action.has_pos) {
                answer(message, "failed", started, "move_to needs a target position");
                return;
            }
            const act::Vec3 position{static_cast<float>(action.x), static_cast<float>(action.y),
                                     static_cast<float>(action.z)};
            if (peer_session == nullptr && client_session == nullptr) {
                // The host's own bot (network profile: no loopback peer) acts as
                // the host row, so its intent is applied right here.
                std::string detail;
                if (!apply_move(rep::kHostWriter, position, detail)) {
                    channel.observe_event("move_rejected",
                                          {{"reason", bot::SJson::make_string(detail)}});
                    answer(message, "failed", started, detail);
                    return;
                }
                channel.observe_event("move", {{"target", bot::SJson::make_string(action.target)},
                                               {"x", bot::SJson::make_number(action.x)},
                                               {"y", bot::SJson::make_number(action.y)},
                                               {"z", bot::SJson::make_number(action.z)}});
                answer(message, "done", started);
                return;
            }
            BridgeMessage intent;
            intent.seq = ++next_request;
            intent.channel = kMoveIntentChannel;
            intent.payload = encode_move_intent(position);
            xrn::core::BridgeLink* const target_link =
                peer_session != nullptr ? &peer_session->bridge() : local_bridge();
            if (target_link == nullptr) {
                answer(message, "failed", started, "no session for the intent");
                return;
            }
            target_link->on_outbound(intent);

            SPendingAction pending_action;
            pending_action.message = message;
            pending_action.request = intent.seq;
            pending_action.target = action.target;
            pending_action.started_ms = started;
            pending_action.deadline_ms = now + kAckTimeoutMs;
            pending_action.verdict_channel = kMoveAckChannel;
            pending.push_back(std::move(pending_action));
            return;
        }
        if (action.type == "drop") {
            // F1-02 (US-1.3): the same item entity goes back into the world. The
            // client resolves the class in its own mirrored inventory, the host
            // decides whether this player really holds it.
            const rep::EntityId item = resolve_item(action.item, true);
            if (item == 0) {
                channel.observe_event("drop_rejected",
                                      {{"item", bot::SJson::make_string(action.item)},
                                       {"reason", bot::SJson::make_string("no_item")}});
                answer(message, "done", started);
                return;
            }
            if (peer_session == nullptr && client_session == nullptr) {
                SRequester own;
                own.slot = rep::kHostWriter;
                own.player = kHostLocalPlayerId;
                std::string detail;
                const inv::PickupVerdict verdict = apply_drop(own, item, detail);
                if (verdict.applied()) {
                    channel.observe_event(
                        "drop",
                        {{"item", bot::SJson::make_string(verdict.item_class)},
                         {"count", bot::SJson::make_number(static_cast<double>(verdict.count))}});
                } else {
                    channel.observe_event(
                        "drop_rejected",
                        {{"item", bot::SJson::make_string(verdict.item_class.empty()
                                                              ? action.item
                                                              : verdict.item_class)},
                         {"reason", bot::SJson::make_string(inv::to_string(verdict.status))}});
                }
                answer(message, "done", started);
                return;
            }
            BridgeMessage intent;
            intent.seq = ++next_request;
            intent.channel = kDropIntentChannel;
            intent.payload = "item=" + std::to_string(static_cast<unsigned long long>(item));
            xrn::core::BridgeLink* const target_link =
                peer_session != nullptr ? &peer_session->bridge() : local_bridge();
            if (target_link == nullptr) {
                answer(message, "failed", started, "no session for the intent");
                return;
            }
            target_link->on_outbound(intent);

            SPendingAction pending_action;
            pending_action.message = message;
            pending_action.request = intent.seq;
            pending_action.item = action.item;
            pending_action.started_ms = started;
            pending_action.deadline_ms = now + kAckTimeoutMs;
            pending_action.verdict_channel = kDropAckChannel;
            pending.push_back(std::move(pending_action));
            return;
        }
        if (action.type == "equip" || action.type == "use" || action.type == "pause" ||
            action.type == "resume" || action.type == "respawn") {
            channel.observe_event(action.type, {{"item", bot::SJson::make_string(action.item)}});
            answer(message, "done", started);
            return;
        }

        answer(message, "failed", started, "unknown action type: " + action.type);
    }

    // --- inbound bridge of this process' own session -------------------------
    //
    // The verdicts of this process' own actions arrive on its own bridge: the
    // in-process peer link in a local check, the client session over the network
    // in a harness run. A debug command travels the same way and comes back on
    // `kDebugAckChannel`.
    //
    // Host verdicts -> bot channel, world events -> observations (F0-12), the rest
    // -> the Lua gameplay layer (F0-05). One consumer only: the queue is drained
    // destructively, so every channel is routed here.
    void drain_peer_verdicts(std::int64_t now) {
        for (std::size_t index = 0; index < pending.size();) {
            if (now >= pending[index].deadline_ms) {
                answer(pending[index].message, "failed", pending[index].deadline_ms,
                       "no verdict from the host");
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            ++index;
        }

        // A network client is the only reader that learns the world events from
        // the host's stream. In the in-process profile the loopback peer lives in
        // the same process: the events its host broadcasts come back to it as an
        // echo of what this process already observed, so they are dropped here.
        const bool remote_client = client_session != nullptr;
        xrn::core::BridgeLink* const source =
            peer_session != nullptr
                ? &peer_session->bridge()
                : (client_session != nullptr ? &client_session->bridge() : nullptr);
        if (source == nullptr) {
            return;
        }
        BridgeMessage message;
        while (source->pull_inbound(message)) {
            if (message.channel == env::kBridgeChannel) {
                if (!remote_client) {
                    continue;
                }
                env::EnvironmentEvent event;
                if (succeeded(env::decode_environment_event(message.payload, event))) {
                    observe_environment_event(event);
                }
                // Same payload, same channel: the Lua layer of a client sees the
                // events of the host (F0-05 parity).
                coop::bridge().enqueue(xrn::core::to_engine<coop::SMessage>(std::move(message)));
                continue;
            }
            if (message.channel == kObsEventChannel) {
                if (!remote_client) {
                    continue;
                }
                const std::string name = obs_event_name(message.payload);
                if (!name.empty()) {
                    channel.observe_event(name, {});
                }
                continue;
            }
            if (message.channel == kDebugAckChannel) {
                std::string status;
                std::string detail;
                split_debug(message.payload, status, detail);
                const std::optional<std::size_t> found =
                    pending_index(kDebugAckChannel, message.seq);
                if (found.has_value()) {
                    answer(pending[found.value()].message, status.c_str(),
                           pending[found.value()].started_ms, detail);
                    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(found.value()));
                }
                continue;
            }
            if (message.channel == kMoveAckChannel) {
                // F1-01: the host applied (or refused) the movement of this
                // character. The bot action is done only when the host accepted it.
                const std::optional<std::size_t> found =
                    pending_index(kMoveAckChannel, message.seq);
                if (!found.has_value()) {
                    continue;
                }
                const std::vector<std::string> values = split_verdict(message.payload);
                const std::string status = values.empty() ? "failed" : values.front();
                const std::string detail = values.size() > 1 ? values[1] : std::string();
                const SPendingAction waiting = pending[found.value()];
                if (status == "applied") {
                    channel.observe_event(
                        "move", {{"target", bot::SJson::make_string(waiting.target)},
                                 {"x", bot::SJson::make_number(waiting.message.action.x)},
                                 {"y", bot::SJson::make_number(waiting.message.action.y)},
                                 {"z", bot::SJson::make_number(waiting.message.action.z)}});
                    answer(waiting.message, "done", waiting.started_ms);
                } else {
                    channel.observe_event(
                        "move_rejected", {{"reason", bot::SJson::make_string(detail)}});
                    answer(waiting.message, "failed", waiting.started_ms, detail);
                }
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(found.value()));
                continue;
            }
            if (message.channel == kDropAckChannel) {
                // F1-02: `applied` = the item is back in the world; anything else
                // is a refusal of the authority (the player does not hold it, ...).
                const std::optional<std::size_t> found =
                    pending_index(kDropAckChannel, message.seq);
                if (!found.has_value()) {
                    continue;
                }
                const std::vector<std::string> values = split_verdict(message.payload);
                const std::string status = values.empty() ? "failed" : values.front();
                const SPendingAction waiting = pending[found.value()];
                const std::string item =
                    values.size() > 1 && !values[1].empty() ? values[1] : waiting.item;
                double raw_count = 0.0;
                const std::int64_t count = values.size() > 2 && parse_double(values[2], raw_count)
                                               ? static_cast<std::int64_t>(raw_count)
                                               : 0;
                const std::string reason = values.size() > 3 ? values[3] : std::string();
                if (status == "applied") {
                    channel.observe_event(
                        "drop", {{"item", bot::SJson::make_string(item)},
                                 {"count", bot::SJson::make_number(static_cast<double>(count))}});
                    answer(waiting.message, "done", waiting.started_ms);
                } else {
                    channel.observe_event(
                        "drop_rejected", {{"item", bot::SJson::make_string(item)},
                                          {"reason", bot::SJson::make_string(reason)}});
                    answer(waiting.message, "failed", waiting.started_ms, reason);
                }
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(found.value()));
                continue;
            }
            if (message.channel != inv::kAckChannel) {
                // Not our business: hand it to the Lua gameplay layer.
                coop::bridge().enqueue(xrn::core::to_engine<coop::SMessage>(message));
                continue;
            }
            inv::PickupVerdict verdict;
            if (failed(inv::decode_pickup_ack(message.payload, verdict))) {
                continue;
            }
            const std::optional<std::size_t> found =
                pending_index(std::string(), verdict.request);
            if (!found.has_value()) {
                continue;
            }
            if (verdict.applied()) {
                channel.observe_event(
                    "pickup",
                    {{"item", bot::SJson::make_string(verdict.item_class)},
                     {"count", bot::SJson::make_number(static_cast<double>(verdict.count))}});
                answer(pending[found.value()].message, "done", pending[found.value()].started_ms);
            } else {
                channel.observe_event(
                    "pickup_rejected",
                    {{"item", bot::SJson::make_string(verdict.item_class.empty()
                                                          ? pending[found.value()].item
                                                          : verdict.item_class)},
                     {"reason", bot::SJson::make_string(inv::to_string(verdict.status))}});
                answer(pending[found.value()].message, "done", pending[found.value()].started_ms);
            }
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(found.value()));
        }
    }

    // --- observed state (contract `kind=state`) ------------------------------
    bot::SObservedState observe_state() {
        bot::SObservedState state;
        state.actor.name = instance;

        // F1-13: a client's map follows the instance of its own character (the
        // group move rekeys the replicated actor records).
        if (client_session != nullptr) {
            refresh_client_view();
            const std::string current = client_level();
            if (current != level) {
                level = current;
                channel.observe_event("level_change",
                                      {{"level", bot::SJson::make_string(level)}});
            }
        }
        state.actor.level = level;
        // This process' own character: its real position, never a default. A client
        // that sees itself somewhere else than the host sees it is a desync, and
        // the F0-14 state comparison would (rightly) report it.
        const std::optional<act::ActorView> own = local_actor_view();
        if (own.has_value()) {
            state.actor.x = own->position.x;
            state.actor.y = own->position.y;
            state.actor.z = own->position.z;
        }

        if (host_session != nullptr) {
            const std::vector<xrn::core::SessionPlayer> players = host_session->players();
            for (const xrn::core::SessionPlayer& player : players) {
                if (player.state != xrn::core::PlayerState::Connected) {
                    continue;
                }
                bot::SActorState row;
                row.name =
                    player.name.empty() ? ("slot-" + std::to_string(player.slot)) : player.name;
                // F1-13: the map of a player is the instance its character is in.
                row.level = level_of_slot(player.slot);
                if (actors != nullptr) {
                    // The replicated position of the character (the entry fan-out is
                    // only its first value: a resume keeps the confirmed one).
                    const std::optional<act::ActorView> view = actors->view(player.slot);
                    const act::Vec3 position =
                        view.has_value() ? view->position : actors->spawn_position(player.slot);
                    row.x = position.x;
                    row.y = position.y;
                    row.z = position.z;
                }
                state.players.push_back(row);
                state.session_players.push_back(row.name);
            }
            if (inventory != nullptr) {
                for (const inv::ItemView& item : inventory->world_items()) {
                    bot::SItemState row;
                    row.id = item.item;
                    row.count = item.count;
                    state.world_items.push_back(row);
                }
                // The bot's own inventory: the host row over the network, the
                // in-process peer's player in a local check.
                for (const inv::ItemView& item : inventory->inventory_of(local_player_id)) {
                    bot::SItemState row;
                    row.id = item.item;
                    row.count = item.count;
                    state.inventory.push_back(row);
                }
            }
        } else if (client_session != nullptr) {
            // A client sees only what the host replicated: the mirror-derived
            // read model, refreshed from the snapshots/deltas of this tick.
            refresh_client_view();
            for (const act::ActorView& view : actor_view.all()) {
                bot::SActorState row;
                row.name = view.name.empty() ? ("slot-" + std::to_string(view.slot)) : view.name;
                row.level = level_for_instance(view.key.instance, start_level);
                row.x = view.position.x;
                row.y = view.position.y;
                row.z = view.position.z;
                state.players.push_back(row);
                state.session_players.push_back(row.name);
            }
            for (const inv::ItemView& item : item_view.world_items()) {
                bot::SItemState row;
                row.id = item.item;
                row.count = item.count;
                state.world_items.push_back(row);
            }
            for (const inv::ItemView& item : item_view.inventory()) {
                bot::SItemState row;
                row.id = item.item;
                row.count = item.count;
                state.inventory.push_back(row);
            }
        }
        if (state.session_players.empty()) {
            state.session_players.push_back(instance);
        }
        return state;
    }

    // --- F0-07 artifact flush ------------------------------------------------
    void flush_log() {
        if (log == nullptr || run_dir.empty() || instance.empty()) {
            return;
        }
        std::error_code code;
        const fs::path directory(run_dir);
        fs::create_directories(directory, code);
        const fs::path path = directory / (instance + ".jsonl");
        std::ostringstream buffer;
        log->write_jsonl(buffer);
        const std::string payload = buffer.str();
        const std::string temporary = path.string() + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                note_error("cannot write " + temporary);
                return;
            }
            output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
            output.flush();
            if (!output) {
                note_error("cannot flush " + temporary);
                return;
            }
        }
        fs::rename(temporary, path, code);
        if (code) {
            note_error("cannot rename " + temporary + ": " + code.message());
        }
    }

    void write_metrics_artifact() {
        if (run_dir.empty() || instance.empty()) {
            return;
        }
        obs::Metrics::Summary summary;
        if (host_session != nullptr) {
            summary = host_session->metrics().summary();
        } else if (client_session != nullptr) {
            summary = client_session->metrics().summary();
        }
        std::ostringstream buffer;
        obs::write_metrics_json(buffer, summary, instance);
        std::error_code code;
        const fs::path directory(run_dir);
        fs::create_directories(directory, code);
        const fs::path path = directory / (instance + ".metrics.json");
        std::ofstream output(path.string(), std::ios::binary | std::ios::trunc);
        if (!output) {
            note_error("cannot write the metrics artifact");
            return;
        }
        const std::string text = buffer.str();
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
};

CCoreRuntime::CCoreRuntime() : m_impl(new SImpl()) {}

CCoreRuntime::~CCoreRuntime() = default;

CCoreRuntime& CCoreRuntime::instance() {
    static CCoreRuntime runtime;
    return runtime;
}

CCoreRuntime& core_runtime() { return CCoreRuntime::instance(); }

bool CCoreRuntime::start() {
    SImpl& impl = *m_impl;
    if (impl.started) {
        return impl.active;
    }
    impl.started = true;

    impl.run_dir = env_value("XRN_HARNESS_RUN_DIR");
    impl.instance = env_value("XRN_HARNESS_INSTANCE");
    impl.role = env_value("XRN_HARNESS_ROLE");
    const std::string level = env_value("XRN_HARNESS_LEVEL");
    if (!level.empty()) {
        impl.level = level;
    }
    impl.start_level = impl.level;
    if (impl.run_dir.empty() || impl.instance.empty() ||
        (impl.role != "host" && impl.role != "client")) {
        // A normal game process: nothing to wire, nothing to log about.
        impl.active = false;
        return false;
    }
    impl.host = impl.role == "host";
    impl.active = true;

    obs::EventLogConfig log_config;
    log_config.source = impl.instance;
    log_config.role = impl.host ? obs::Role::Host : obs::Role::Client;
    impl.log.reset(new obs::EventLog(log_config));

    impl.world.reset(new WorldState());

    // F0-02: one transport endpoint per process. The harness profile goes over
    // the network by default (loopback in the debug profile, so a local e2e
    // needs no network setup); the in-process queues stay available for local
    // checks that must not depend on sockets (`XRN_HARNESS_NET=inprocess`).
    const STransportConfig transport_config = transport_config_from_env();
    impl.udp = transport_config.udp;
    bool wired = true;
    if (impl.udp) {
        impl.endpoint.reset(new CTransport());
        if (impl.endpoint->start(transport_config)) {
            impl.link = impl.endpoint->link();
            impl.transport = impl.endpoint->description();
        } else {
            // No silent fallback: a run that lost its transport must be visible
            // in the artifacts, not fake an in-process session.
            impl.transport = "udp unavailable: " + impl.endpoint->error();
            impl.note_error("transport: " + impl.endpoint->error());
            wired = false;
        }
    } else {
        impl.net.reset(new InProcessNetwork());
        impl.transport = "in-process link (XRN_HARNESS_NET=inprocess)";
    }

    if (impl.host) {
        impl.environment.reset(new env::EnvironmentAuthority(*impl.world));
        impl.actors.reset(new act::ActorReplicator(*impl.world));
        impl.inventory.reset(new inv::InventoryAuthority(*impl.world));

        static_cast<void>(impl.environment->publish());
        static_cast<void>(impl.actors->publish_entry_point());
        const std::optional<act::SpawnPoint> entry = impl.actors->entry_point();
        if (entry.has_value()) {
            impl.debug_item =
                impl.inventory->spawn_item(kDebugItem, 1, entry->position).value_or(0);
        }

        // F1-13: the host owns the group transition (F1-13). The catalog holds the
        // map the profile starts on and the debug target map; one doorway stands on
        // the start map at the point every player spawns on, so the whole group is
        // at the changer (EC-7). The load budget is short: a debug run does not
        // load a real map, a straggler resyncs into the committed state.
        lvl::LevelConfig level_config;
        level_config.load_timeout_ms = kDebugLoadBudgetMs;
        // The debug surface is not a fight: EC-6 stays off for the scenario (a real
        // session feeds the combat state of its own model, F1-08).
        level_config.block_in_combat = false;
        impl.level_authority.reset(
            new lvl::LevelChangeAuthority(*impl.world, *impl.actors, level_config));
        lvl::LevelInfo start;
        start.instance = kStartInstance;
        start.level = impl.start_level;
        start.entry = entry.has_value() ? entry.value() : act::SpawnPoint{};
        start.entry.instance = kStartInstance;
        static_cast<void>(impl.level_authority->catalog().set(start));
        lvl::LevelInfo target;
        target.instance = kTargetInstance;
        target.level = kTargetLevel;
        target.entry = start.entry;
        target.entry.instance = kTargetInstance;
        static_cast<void>(impl.level_authority->catalog().set(target));
        static_cast<void>(impl.level_authority->publish_idle());
        lvl::LevelChanger changer;
        changer.id = lvl::kChangerEntityBase;
        changer.instance = kStartInstance;
        changer.target = kTargetInstance;
        changer.position = start.entry.position;
        static_cast<void>(lvl::publish_changer(*impl.world, changer));

        if (wired) {
            xrn::core::HostConfig config;
            config.log = impl.log.get();
            config.actors = impl.actors.get();
            config.inventory = impl.inventory.get();
            config.environment = impl.environment.get();
            // F1-13: the session ticks the transition and attributes peer intents.
            config.level = impl.level_authority.get();
            config.host_name = impl.instance;
            config.hash_every_publishes = 5;
            config.reconnect.window_ms = kDebugReconnectWindowMs;
            if (impl.udp) {
                // F0-09: every join of a harness client carries the build it runs
                // and this host compares it against its own (AC-1.10.1). The
                // in-process profile keeps the pre-F0-09 behaviour: its loopback
                // peer is a local check, not a joining player.
                impl.build = harness_build();
                impl.cfg_gate.reset(new cfg::Gate(impl.build));
                config.cfg_gate = impl.cfg_gate.get();
            }
            if (impl.net != nullptr) {
                // In-process profile: the endpoint is owned by the network object.
                impl.link = impl.net->endpoint(kHostPeerId).release();
                impl.owned_link.reset(impl.link);
            }
            impl.host_session.reset(new HostSession(*impl.world, *impl.link, config));
            impl.local_player_id = kHostLocalPlayerId;
            // Only the in-process profile gives the host a bot peer; over the
            // network the peer slots hold the real client processes.
            if (!impl.udp) {
                if (impl.start_local_peer()) {
                    impl.local_player_id = kLoopbackPeerId;
                }
            }
        }
    } else if (wired) {
        // F0-09: the build this process presents at the join, verified by the host.
        impl.local_player_id = env_uint("XRN_HARNESS_SLOT", 1) + 1;
        if (impl.udp) {
            impl.build = harness_build();
            impl.cfg_probe.reset(new cfg::Probe(impl.build));
        }
        // F0-02 recovery: a lost link is not the end of the session. The adapter
        // restarts the transport session and rejoins with the same identity while
        // the host's reconnect window still holds the slot (F1-10) — so a peer the
        // host dropped comes back as the same character, or learns that its window
        // expired. Outside the grace window the session ends like a listen host
        // without a host (EC-4).
        if (impl.net != nullptr) {
            impl.link = impl.net->endpoint(kClientPeerId).release();
            impl.owned_link.reset(impl.link);
        }
        impl.client_session.reset(new ClientSession(*impl.link, impl.make_client_config()));
    }

    // F0-05: the core becomes the sink of the engine Lua bridge.
    impl.sink.owner = &impl;
    coop::bridge().set_sink(&impl.sink);

    // F0-12: only when the runner prepared a channel for this instance.
    std::error_code code;
    if (fs::exists(fs::path(impl.run_dir) / impl.instance / "bot", code)) {
        if (!impl.channel.open(impl.run_dir, impl.instance)) {
            impl.note_error("bot channel: " + impl.channel.error());
        }
    }

    if (impl.log != nullptr) {
        const std::string detail =
            std::string(impl.host ? "headless host, " : "headless client, ") + impl.transport;
        impl.log->session_start(0, detail);
    }

    impl.last_flush_ms = epoch_ms();
    impl.last_state_ms = impl.last_flush_ms;
    return true;
}

void CCoreRuntime::shutdown() {
    SImpl& impl = *m_impl;
    if (!impl.active) {
        return;
    }
    impl.active = false;

    if (impl.host_session != nullptr) {
        impl.host_session->end_session();
    }
    if (impl.log != nullptr) {
        impl.log->session_end(impl.world != nullptr ? impl.world->tick() : 0, "runtime stopped");
    }
    coop::bridge().set_sink(nullptr);

    impl.flush_log();
    impl.write_metrics_artifact();
    impl.channel.flush();

    impl.host_session.reset();
    impl.client_session.reset();
    impl.peer_session.reset();
    impl.peer_link.reset();
    // The sessions are gone: the link and its endpoint may be released now.
    impl.link = nullptr;
    impl.owned_link.reset();
    impl.endpoint.reset();
    impl.net.reset();
    impl.level_authority.reset();
    impl.cfg_gate.reset();
    impl.cfg_probe.reset();
    impl.inventory.reset();
    impl.actors.reset();
    impl.environment.reset();
    impl.world.reset();
    impl.log.reset();
}

void CCoreRuntime::tick() {
    // The frame hook is the only always-present entry point: arm the runtime on
    // the first frame (the harness environment is read exactly once). A normal
    // game process reports "not a harness instance" and never pays more than
    // this single test.
    if (!m_impl->started && !start()) {
        return;
    }
    SImpl& impl = *m_impl;
    if (!impl.active) {
        return;
    }

    // F0-05: the gameplay Lua layer is started once, when the level is up.
    // Afterwards it only receives events (NFR-ARCH-10). A dedicated server has
    // no script engine at all; a headless process always has one, so the bridge
    // must go through get_script_engine() instead of dereferencing the AI space.
    if (!impl.lua_started) {
        impl.lua_started = true;
        CScriptEngine* engine = ai().get_script_engine();
        ::luabind::functor<void> bootstrap;
        if (engine && engine->functor("coop_core.on_game_start", bootstrap)) {
            // release builds are compiled without exceptions (`xrCore.h`:
            // `LUABIND_NO_EXCEPTIONS`), so `luabind::error` does not exist and
            // the pcall callback reports a failing call instead of throwing.
#ifndef LUABIND_NO_EXCEPTIONS
            try {
                bootstrap();
            } catch (const ::luabind::error&) {
                impl.note_error("Lua bootstrap failed (coop_core.on_game_start)");
            }
#else   // LUABIND_NO_EXCEPTIONS
            bootstrap();
#endif  // LUABIND_NO_EXCEPTIONS
        }
    }

    const std::int64_t now = epoch_ms();

    impl.accumulator_ms += kStepMs;
    if (impl.accumulator_ms > static_cast<std::int64_t>(kMaxCatchUpMs)) {
        impl.accumulator_ms = kMaxCatchUpMs;
    }
    while (impl.accumulator_ms >= static_cast<std::int64_t>(kStepMs)) {
        impl.accumulator_ms -= static_cast<std::int64_t>(kStepMs);
        ++impl.steps;

        if (impl.host) {
            if (impl.peer_session != nullptr) {
                impl.peer_session->service();
            }
            if (impl.host_session != nullptr) {
                impl.host_session->service();
                // F0-02: a peer's intent is executed before the publish of this
                // step, so its effect leaves in the same delta.
                impl.drain_host_intents();
            }
            if (impl.environment != nullptr && impl.host_session != nullptr) {
                static_cast<void>(impl.environment->advance_real_ms(kStepMs));
                const std::vector<env::EnvironmentEvent> events = impl.environment->take_events();
                for (const env::EnvironmentEvent& event : events) {
                    impl.observe_environment_event(event);
                }
                if (!events.empty()) {
                    impl.publish_environment_events(events);
                }
                // F1-01: the world holds exactly one character per connected player.
                // The host's own character is part of it, so a client observes the
                // host and a group transition moves it with everybody (AC-3.3.1).
                if (impl.actors != nullptr) {
                    static_cast<void>(impl.actors->sync(impl.host_session->players()));
                }
                impl.host_session->publish();
                // F1-13: `publish()` ticks the level authority, so a committed
                // group move is observed in the same step it happened.
                impl.observe_level_events();
            }
            // F1-01: the run animation of a debug move settles to idle after its
            // beat, and the actor deltas reach the Lua layer.
            impl.service_moves(now);
            impl.publish_actor_changes();
            impl.observe_item_events();
            impl.observe_session_deltas();
            impl.step_reconnect_cycle(now);
            // A debug drop that had to let a verdict out first (F0-02/F0-09).
            impl.apply_pending_drop(now);
            // A refused build keeps the address of its transport session: close it
            // once the verdict left, so the player can connect again at once.
            impl.close_refused_peers(now);
        } else if (impl.client_session != nullptr) {
            impl.client_session->service();
            impl.observe_client_rejection();
            impl.observe_client_join();
            impl.service_client_link(now);
            impl.service_level_screen();
        }
    }

    // Host verdicts -> bot channel (F0-12). Drains the process' own bridge
    // before the Lua pump: a pickup/debug verdict is the bot's, not Lua data.
    impl.drain_peer_verdicts(now);

    // Core -> Lua (F0-05): event-driven dispatch only.
    static_cast<void>(coop::bridge().pump());

    // F0-12: the runner prepares `<run-dir>/<instance>/bot` when it starts a
    // scenario, which is after the stand was healthchecked; open the channel as
    // soon as it appears (bounded probe, never a stat per frame).
    if (!impl.channel.ready() && now - impl.channel_probe_at_ms >= 500) {
        impl.channel_probe_at_ms = now;
        std::error_code code;
        if (fs::exists(fs::path(impl.run_dir) / impl.instance / "bot", code)) {
            if (!impl.channel.open(impl.run_dir, impl.instance)) {
                impl.note_error("bot channel: " + impl.channel.error());
            }
        }
    }

    bot::SInboxMessage message;
    while (impl.channel.ready() && impl.channel.poll(message)) {
        impl.handle_action(message, now);
    }

    if (now - impl.last_state_ms >= static_cast<std::int64_t>(kStateEveryMs)) {
        impl.last_state_ms = now;
        impl.channel.observe_state(impl.observe_state());
    }

    if (now - impl.last_flush_ms >= static_cast<std::int64_t>(kLogFlushEveryMs)) {
        impl.last_flush_ms = now;
        impl.flush_log();
    }
}

bool CCoreRuntime::active() const { return m_impl->active; }

bool CCoreRuntime::command(const std::string& name, const std::string& argument,
                           std::string& detail) {
    SImpl& impl = *m_impl;
    detail.clear();

    if (name == "debug.weather") {
        if (!impl.host || impl.environment == nullptr) {
            detail = "weather is host-authoritative";
            return false;
        }
        // A debug run does not wait out a 15..40 game-minute dwell: the next
        // weather step is forced now, without moving the world clock (a skipped
        // clock interval would be a `time_sync` of its own, AC-1.6.3).
        const CoreError result = impl.environment->force_weather_step();
        if (failed(result)) {
            detail = "the weather step was refused";
            return false;
        }
        detail = "weather step forced";
        return true;
    }
    if (name == "debug.emission") {
        if (!impl.host || impl.environment == nullptr) {
            detail = "the emission is host-authoritative";
            return false;
        }
        const CoreError result = impl.environment->trigger_emission();
        if (failed(result)) {
            detail = "an emission is already under way";
            return false;
        }
        detail = "emission triggered";
        return true;
    }
    if (name == "debug.clock") {
        if (!impl.host || impl.environment == nullptr) {
            detail = "the world clock is host-authoritative";
            return false;
        }
        const std::optional<env::ClockView> clock = impl.environment->clock();
        if (!clock.has_value()) {
            detail = "no clock in the world";
            return false;
        }
        const CoreError result =
            impl.environment->set_world_time(clock->world_ms + 60 * env::kMsPerSecond);
        if (failed(result)) {
            detail = "the world clock refused the jump";
            return false;
        }
        detail = "clock advanced by a game minute";
        return true;
    }
    if (name == "debug.reconnect") {
        if (!impl.host || impl.host_session == nullptr) {
            detail = "reconnect is a host-side debug scenario";
            return false;
        }
        const bool expired = argument_value(argument, "window") == "expired";
        if (impl.peer_session != nullptr) {
            // In-process profile: the dropped peer is the loopback client of this
            // process, so the scenario runs locally.
            impl.reconnect_expired = expired;
            impl.reconnect_phase = 1;
            impl.reconnect_at_ms = 0;
            detail = expired ? "peer drops, window closes, peer returns"
                             : "peer returns inside the window";
            return true;
        }
        if (impl.debug_requester == 0) {
            // Network profile: there is no loopback peer to drop and the host's own
            // bot has no link of its own. The drop is a network operation and needs
            // a connected peer that asked for it.
            detail = "reconnect needs a connected peer";
            return false;
        }
        // The link is closed after the verdict left, so the client can answer its
        // bot action and then recover the session (F0-02).
        impl.pending_drop.armed = true;
        impl.pending_drop.peer = impl.debug_requester;
        impl.pending_drop.mode =
            expired ? xrn::core::PeerDrop::CloseWindow : xrn::core::PeerDrop::KeepSlot;
        impl.pending_drop.due_ms = epoch_ms() + kDebugCfgDropDelayMs;
        impl.pending_drop.command = "debug.reconnect";
        detail =
            expired ? "peer drops, window closes, peer returns" : "peer returns inside the window";
        return true;
    }
    if (name == "debug.cfg_probe") {
        // F0-09 (AC-1.10): the build comparison of a debug run. The host serves
        // its manifest, the client presents its own at the join; the probe either
        // confirms the current build (`cfg_check`) or makes the host refuse the
        // divergent one — the peer is dropped and re-joins with the build it now
        // presents, so the refusal the client sees is a real join verdict with the
        // real divergent paths (never a simulated one).
        if (!impl.host) {
            detail = "the build check is host-authoritative";
            return false;
        }
        const std::vector<std::string> paths =
            split_probe_paths(argument_value(argument, "mismatch"));
        if (paths.empty()) {
            // Identical builds pass without a refusal and without a move (AC-1.10.2).
            impl.channel.observe_event("cfg_check",
                                       {{"result", bot::SJson::make_string("match")}});
            detail = "builds match";
            return true;
        }
        if (impl.cfg_gate == nullptr || impl.host_session == nullptr ||
            impl.debug_requester == 0) {
            detail = "a divergent build needs a connected peer to re-check";
            return false;
        }
        const cfg::CfgManifest divergent = diverge_build(impl.cfg_gate->manifest(), paths);
        const cfg::CfgDiff diff = cfg::compare(impl.cfg_gate->manifest(), divergent);
        if (diff.accepted()) {
            detail = "the requested files do not diverge from this build";
            return false;
        }
        const std::vector<std::string> files = order_like(paths, diff.paths());
        impl.channel.observe_event_files("join_rejected", files, "cfg_diff");
        // The peer is dropped *after* its verdict, keeps its slot (the reconnect
        // window of F1-10) and comes back with the divergent build it now
        // presents: the gate answers with the paths and the client records the
        // refusal (AC-1.10.1, the list only the refused player needs).
        impl.pending_drop.armed = true;
        impl.pending_drop.peer = impl.debug_requester;
        impl.pending_drop.mode = xrn::core::PeerDrop::KeepSlot;
        impl.pending_drop.due_ms = epoch_ms() + kDebugCfgDropDelayMs;
        impl.pending_drop.command = "debug.cfg_probe";
        detail = std::to_string(files.size()) + " divergent file(s): the build is refused";
        return true;
    }
    if (name == "debug.level_change") {
        // F1-13 (AC-3.3): a group move through a level changer. The authority
        // validates the request in the sender's slot and the session commits it in
        // `publish()` once the movers acknowledged the token or the load budget
        // expired; the observations of what happened come from its events.
        if (!impl.host || impl.level_authority == nullptr || impl.host_session == nullptr) {
            detail = "a level change is host-authoritative";
            return false;
        }
        const std::string requested = argument_value(argument, "level");
        lvl::InstanceId target = 0;
        if (!instance_for_level(requested, impl.start_level, target)) {
            detail = requested.empty() ? "debug.level_change needs level=<map>"
                                       : "unknown level: " + requested;
            return false;
        }
        // A crash of the transition window is the peer's own: the named player must
        // be the sender, and the drop keeps its slot so the return resumes the same
        // character (AC-3.3.2 — one location, same inventory).
        std::string failure;
        if (!impl.arm_peer_crash(argument_value(argument, "crash"), failure)) {
            detail = failure;
            return false;
        }
        const lvl::TransitionView current = impl.level_authority->transition();
        if (current.state == lvl::State::Completed && current.target == target) {
            // A repeated debug step: the group already stands in the target map.
            detail = "group already in " + requested;
            return true;
        }
        lvl::LevelChangeIntent intent;
        intent.request = ++impl.next_request;
        intent.changer = lvl::kChangerEntityBase;
        const lvl::LevelChangeVerdict verdict =
            impl.level_authority->request(impl.level_requester(), intent, epoch_ms());
        if (!verdict.accepted()) {
            if (verdict.refusal != lvl::Refusal::AlreadyThere) {
                detail = std::string("level change refused: ") + lvl::refusal_token(verdict.refusal);
                return false;
            }
            detail = "group already in " + requested;
            return true;
        }
        detail = "group moves to " + requested + " (" + std::to_string(verdict.movers) +
                 " mover(s))";
        return true;
    }
    detail = "unknown debug command";
    return false;
}

SCoreStatus CCoreRuntime::status() const {
    const SImpl& impl = *m_impl;
    SCoreStatus status;
    status.active = impl.active;
    status.host = impl.host;
    status.degraded = impl.active && !impl.error.empty();
    status.role = impl.role;
    status.instance = impl.instance;
    status.run_dir = impl.run_dir;
    status.transport = impl.transport;
    if (impl.active) {
        status.log_path = (fs::path(impl.run_dir) / (impl.instance + ".jsonl")).string();
    }
    status.error = impl.error;
    status.steps = impl.steps;
    status.world_tick = impl.world != nullptr ? impl.world->tick() : 0;
    status.events = impl.log != nullptr ? impl.log->size() : 0;
    status.joined = impl.host_session != nullptr ||
                    (impl.client_session != nullptr && impl.client_session->join_accepted());
    if (impl.host_session != nullptr) {
        for (const xrn::core::SessionPlayer& player : impl.host_session->players()) {
            if (player.state == xrn::core::PlayerState::Connected) {
                ++status.peers;
            }
        }
    }
    return status;
}

}  // namespace coop
