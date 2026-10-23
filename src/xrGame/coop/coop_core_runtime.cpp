////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_core_runtime.cpp
//	Created 	: 23.10.2026
//	Description : Engine wiring of the coop core (F0-04/F0-05/F0-07/F0-12)
////////////////////////////////////////////////////////////////////////////

#include "pch_script.h"
#include "coop/coop_core_runtime.h"

#include <algorithm>
#include <chrono>
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
#include "xrn/core/actor.hpp"
#include "xrn/core/bridge_seam.hpp"
#include "xrn/core/connection.hpp"
#include "xrn/core/environment.hpp"
#include "xrn/core/error.hpp"
#include "xrn/core/inventory.hpp"
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
    std::uint32_t steps = 0;
    std::int64_t accumulator_ms = 0;
    std::int64_t last_state_ms = 0;
    std::int64_t last_flush_ms = 0;
    rep::EntityId debug_item = 0;
    std::uint32_t next_request = 0;
    std::uint32_t item_sequence = 0;
    bool lua_started = false;

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
    };
    SPendingDrop pending_drop;
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
        std::int64_t deadline_ms = 0;
        // A debug command forwarded to the host (network profile); its verdict
        // arrives on `kDebugAckChannel`, not as a pickup ack.
        bool debug = false;
    };
    std::vector<SPendingAction> pending;

    xrn::core::BridgeLink* local_bridge() {
        if (host_session != nullptr) {
            return &host_session->bridge();
        }
        if (client_session != nullptr) {
            return &client_session->bridge();
        }
        return nullptr;
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
            // The start of the cycle and its end are the two boundaries every
            // observer sees once (AC-1.6.2); the intermediate phases stay in the
            // F0-07 log only.
            if (event.emission.transition == env::EmissionTransition::Warn ||
                event.emission.transition == env::EmissionTransition::Start) {
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
        const CoreError reason = client_session->join_rejection();
        if (reason == CoreError::BuildMismatch) {
            channel.observe_event_files("join_rejected", client_session->rejection_paths(),
                                        "cfg_diff");
            return;
        }
        const std::string text = xrn::core::to_string(reason);
        channel.observe_event("join_rejected", {{"reason", bot::SJson::make_string(text)}});
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

    // Host: executes the debug intents peers sent and answers them. Everything
    // else stays in the local bridge for the Lua layer (the queue is drained
    // destructively, so foreign messages are handed over explicitly).
    void drain_debug_intents() {
        if (host_session == nullptr) {
            return;
        }
        std::vector<BridgeMessage> foreign;
        BridgeMessage message;
        while (host_session->bridge().pull_inbound(message)) {
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
                channel.observe_event("debug_command", {{"command", bot::SJson::make_string(name)},
                                                        {"detail", bot::SJson::make_string(detail)}});
            } else {
                // Honest answer: the surface exists, the mechanic is not wired.
                channel.observe_event(
                    "debug_unsupported",
                    {{"command", bot::SJson::make_string(name)},
                     {"reason", bot::SJson::make_string(detail)}});
            }
            BridgeMessage ack;
            ack.seq = message.seq;
            ack.channel = kDebugAckChannel;
            ack.payload = std::string(accepted ? "done" : "failed") + kDebugFieldSeparator + detail;
            static_cast<void>(host_session->broadcast_bridge(ack));
            apply_pending_drop();
        }
        for (BridgeMessage& kept : foreign) {
            coop::bridge().enqueue(xrn::core::to_engine<coop::SMessage>(std::move(kept)));
        }
    }

    // The verdict is already on the wire, so the peer can answer its bot action
    // even though the host closes its link right after (F0-02).
    void apply_pending_drop() {
        if (!pending_drop.armed) {
            return;
        }
        const SPendingDrop drop = pending_drop;
        pending_drop = SPendingDrop{};
        if (host_session == nullptr) {
            return;
        }
        const CoreError result = host_session->drop_peer(drop.peer, drop.mode);
        if (failed(result)) {
            note_error(std::string("debug.reconnect: cannot drop the peer: ") +
                       xrn::core::to_string(result));
            channel.observe_event(
                "debug_unsupported",
                {{"command", bot::SJson::make_string("debug.reconnect")},
                 {"reason", bot::SJson::make_string(std::string("drop failed: ") +
                                                    xrn::core::to_string(result))}});
        }
    }

    // --- bot actions (F0-12) -------------------------------------------------
    //
    // The scenario names an item class ("bread"), the wire carries ids. A client
    // has no authoritative inventory: it resolves the class in the world items it
    // mirrors, exactly like a player picking up what it sees. The host resolves
    // it in its own authority.
    rep::EntityId resolve_item(const std::string& item_class) {
        if (inventory != nullptr) {
            for (const inv::ItemView& item : inventory->world_items()) {
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
                pending_action.debug = true;
                pending_action.deadline_ms = now + kAckTimeoutMs;
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
                        {{"item", bot::SJson::make_string(verdict.item_class.empty() ? action.item
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
            pending_action.deadline_ms = now + kAckTimeoutMs;
            pending.push_back(std::move(pending_action));
            return;
        }

        if (action.type == "move_to" || action.type == "teleport") {
            channel.observe_event("move", {{"item", bot::SJson::make_string(action.target)}});
            answer(message, "done", started);
            return;
        }
        if (action.type == "drop" || action.type == "equip" || action.type == "use" ||
            action.type == "pause" || action.type == "resume" || action.type == "respawn") {
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
                for (std::size_t index = 0; index < pending.size(); ++index) {
                    if (!pending[index].debug || pending[index].request != message.seq) {
                        continue;
                    }
                    answer(pending[index].message, status.c_str(), pending[index].deadline_ms,
                           detail);
                    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
                    break;
                }
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
            for (std::size_t index = 0; index < pending.size(); ++index) {
                if (pending[index].debug || pending[index].request != verdict.request) {
                    continue;
                }
                if (verdict.applied()) {
                    channel.observe_event(
                        "pickup",
                        {{"item", bot::SJson::make_string(verdict.item_class)},
                         {"count", bot::SJson::make_number(static_cast<double>(verdict.count))}});
                    answer(pending[index].message, "done", pending[index].deadline_ms);
                } else {
                    channel.observe_event(
                        "pickup_rejected",
                        {{"item",
                          bot::SJson::make_string(verdict.item_class.empty() ? pending[index].item
                                                                             : verdict.item_class)},
                         {"reason", bot::SJson::make_string(inv::to_string(verdict.status))}});
                    answer(pending[index].message, "done", pending[index].deadline_ms);
                }
                pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(index));
                break;
            }
        }
    }

    // --- observed state (contract `kind=state`) ------------------------------
    bot::SObservedState observe_state() {
        bot::SObservedState state;
        state.actor.name = instance;
        state.actor.level = level;

        if (host_session != nullptr) {
            const std::vector<xrn::core::SessionPlayer> players = host_session->players();
            for (const xrn::core::SessionPlayer& player : players) {
                if (player.state != xrn::core::PlayerState::Connected) {
                    continue;
                }
                bot::SActorState row;
                row.name =
                    player.name.empty() ? ("slot-" + std::to_string(player.slot)) : player.name;
                row.level = level;
                if (actors != nullptr) {
                    const act::Vec3 position = actors->spawn_position(player.slot);
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
                row.level = level;
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

        if (wired) {
            xrn::core::HostConfig config;
            config.log = impl.log.get();
            config.actors = impl.actors.get();
            config.inventory = impl.inventory.get();
            config.environment = impl.environment.get();
            config.host_name = impl.instance;
            config.hash_every_publishes = 5;
            config.reconnect.window_ms = kDebugReconnectWindowMs;
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
        xrn::core::ClientConfig config;
        config.host_peer = kClientPeerId;
        config.player_id = env_uint("XRN_HARNESS_SLOT", 1) + 1;
        config.player_name = impl.instance;
        config.log = impl.log.get();
        // F0-02 recovery: a lost link is not the end of the session. The adapter
        // restarts the transport session and rejoins with the same identity while
        // the host's reconnect window still holds the slot (F1-10) — so a peer the
        // host dropped comes back as the same character, or learns that its window
        // expired. Outside the grace window the session ends like a listen host
        // without a host (EC-4).
        config.host_loss_grace_ms = kClientReconnectGraceMs;
        impl.local_player_id = config.player_id;
        if (impl.net != nullptr) {
            impl.link = impl.net->endpoint(kClientPeerId).release();
            impl.owned_link.reset(impl.link);
        }
        impl.client_session.reset(new ClientSession(*impl.link, config));
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
        const std::string detail = std::string(impl.host ? "headless host, " : "headless client, ") +
                                   impl.transport;
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
    // Afterwards it only receives events (NFR-ARCH-10).
    if (!impl.lua_started) {
        impl.lua_started = true;
        ::luabind::functor<void> bootstrap;
        if (ai().script_engine().functor("coop_core.on_game_start", bootstrap)) {
            try {
                bootstrap();
            } catch (const ::luabind::error&) {
                impl.note_error("Lua bootstrap failed (coop_core.on_game_start)");
            }
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
                // F0-02: a client's debug intent is executed before the publish
                // of this step, so its effect leaves in the same delta.
                impl.drain_debug_intents();
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
                impl.host_session->publish();
            }
            impl.observe_item_events();
            impl.observe_session_deltas();
            impl.step_reconnect_cycle(now);
        } else if (impl.client_session != nullptr) {
            impl.client_session->service();
            impl.observe_client_rejection();
            impl.service_client_link(now);
        }
    }

    // Host verdicts -> bot channel (F0-12). Drains the process' own bridge
    // before the Lua pump: a pickup/debug verdict is the bot's, not Lua data.
    impl.drain_peer_verdicts(now);

    // Core -> Lua (F0-05): event-driven dispatch only.
    static_cast<void>(coop::bridge().pump());

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
        // A debug run does not wait out a 15..40 game-minute dwell: skipping a
        // whole dwell guarantees at least one weather step (AC-1.6.1).
        static_cast<void>(impl.environment->advance_game_ms(45 * env::kMsPerMinute));
        detail = "weather cycle advanced";
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
        detail = expired ? "peer drops, window closes, peer returns"
                         : "peer returns inside the window";
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
