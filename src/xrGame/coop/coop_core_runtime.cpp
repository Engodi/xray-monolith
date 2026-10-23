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
#include "xrn/core/actor.hpp"
#include "xrn/core/bridge_seam.hpp"
#include "xrn/core/environment.hpp"
#include "xrn/core/error.hpp"
#include "xrn/core/inventory.hpp"
#include "xrn/core/link.hpp"
#include "xrn/core/observability.hpp"
#include "xrn/core/session.hpp"
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
    std::string role;
    std::string instance;
    std::string run_dir;
    std::string level = kDefaultLevel;
    std::string error;

    // --- core ----------------------------------------------------------------
    std::unique_ptr<obs::EventLog> log;
    std::unique_ptr<WorldState> world;
    std::unique_ptr<env::EnvironmentAuthority> environment;
    std::unique_ptr<act::ActorReplicator> actors;
    std::unique_ptr<inv::InventoryAuthority> inventory;
    std::unique_ptr<InProcessNetwork> net;
    std::unique_ptr<IMessageLink> link;
    std::unique_ptr<HostSession> host_session;
    std::unique_ptr<ClientSession> client_session;
    // Debug loopback peer of the host profile: a real ClientSession on the
    // in-process link, so a bot intent travels the same path as a remote one
    // (peer -> HostSession -> host authority -> verdict) until the F0-02
    // transport is wired into the engine.
    std::unique_ptr<IMessageLink> peer_link;
    std::unique_ptr<ClientSession> peer_session;

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
    std::uint32_t debug_item = 0;
    std::uint32_t next_request = 0;
    bool lua_started = false;

    // Reconnect scenario (`debug.reconnect`): 0 idle, 1 dropped, 2 returning.
    int reconnect_phase = 0;
    bool reconnect_expired = false;
    std::int64_t reconnect_at_ms = 0;

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
    void observe_environment_event(const env::EnvironmentEvent& event) {
        std::string payload;
        if (failed(env::encode_environment_event(event, payload))) {
            return;
        }

        obs::Event log_event;
        log_event.tick = world != nullptr ? world->tick() : 0;
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
            // The start of the cycle is what the scenarios observe once.
            if (event.emission.transition == env::EmissionTransition::Warn ||
                event.emission.transition == env::EmissionTransition::Start) {
                channel.observe_event(
                    "emission_start",
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

    // --- connection lifecycle -> observations (F0-08/F1-10/F0-09) -----------
    void observe_session_deltas() {
        if (host_session == nullptr) {
            return;
        }
        const xrn::core::SessionStats& stats = host_session->stats();

        if (stats.reconnects_reclaimed > seen_reclaims) {
            seen_reclaims = stats.reconnects_reclaimed;
            channel.observe_event("player_rejoin", {});
        }
        if (stats.resumes_expired > seen_expired) {
            seen_expired = stats.resumes_expired;
            channel.observe_event("window_expired", {});
        }
        if (stats.builds_rejected > seen_build_rejects) {
            seen_build_rejects = stats.builds_rejected;
            std::vector<std::string> paths;
            if (peer_session != nullptr) {
                paths = peer_session->rejection_paths();
            }
            // AC-1.10.1: the refusal names the divergent config paths.
            channel.observe_event_files("join_rejected", paths, "cfg_diff");
        }
    }

    // --- debug reconnect scenario (AC-1.12) ---------------------------------
    //
    // The in-process link keeps events for a peer id until somebody polls them:
    // a peer that vanished must be drained, otherwise its successor would read
    // the *previous* Disconnected event and end before joining.
    void drain_peer_inbox() {
        std::unique_ptr<IMessageLink> scratch = net->endpoint(kLoopbackPeerId);
        std::vector<xrn::core::LinkEvent> events;
        while (scratch->poll(events)) {
            events.clear();
        }
    }

    void step_reconnect_cycle(std::int64_t now) {
        if (reconnect_phase == 1) {
            // Drop the loopback peer exactly like a lost client process.
            net->disconnect(kHostPeerId, kLoopbackPeerId);
            peer_session.reset();
            peer_link.reset();
            drain_peer_inbox();
            reconnect_at_ms =
                now +
                (reconnect_expired ? static_cast<std::int64_t>(kDebugReconnectWindowMs) + 150 : 40);
            reconnect_phase = 2;
            return;
        }
        if (reconnect_phase == 2 && now >= reconnect_at_ms) {
            drain_peer_inbox();
            xrn::core::ClientConfig config;
            config.host_peer = kHostPeerId;
            config.player_id = 1;
            config.player_name = instance + "-bot";
            peer_link = net->endpoint(kLoopbackPeerId);
            peer_session.reset(new ClientSession(*peer_link, config));
            net->connect(kHostPeerId, kLoopbackPeerId);
            reconnect_phase = 0;
        }
    }

    // --- bot actions (F0-12) -------------------------------------------------
    void answer(const bot::SInboxMessage& message, const char* status, std::int64_t started_ms,
                const std::string& failure = std::string()) {
        channel.ack(message, status, std::max<std::int64_t>(0, epoch_ms() - started_ms), failure);
    }

    void handle_action(const bot::SInboxMessage& message, std::int64_t now) {
        const bot::SAction& action = message.action;
        const std::int64_t started = message.sent_at_ms > 0 ? message.sent_at_ms : now;

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
            if (debug_item == 0) {
                channel.observe_event("pickup_rejected",
                                      {{"item", bot::SJson::make_string(action.item)},
                                       {"reason", bot::SJson::make_string("no_item")}});
                answer(message, "done", started);
                return;
            }
            inv::PickupIntent intent;
            intent.request = ++next_request;
            intent.item = debug_item;
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

    // --- host verdicts -> bot channel ---------------------------------------
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

        if (peer_session == nullptr) {
            return;
        }
        BridgeMessage message;
        while (peer_session->bridge().pull_inbound(message)) {
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
                if (pending[index].request != verdict.request) {
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
    bot::SObservedState observe_state() const {
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
                // The bot's own inventory is the one of its loopback peer.
                for (const inv::ItemView& item : inventory->inventory_of(1)) {
                    bot::SItemState row;
                    row.id = item.item;
                    row.count = item.count;
                    state.inventory.push_back(row);
                }
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
    impl.net.reset(new InProcessNetwork());

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

        xrn::core::HostConfig config;
        config.log = impl.log.get();
        config.actors = impl.actors.get();
        config.inventory = impl.inventory.get();
        config.environment = impl.environment.get();
        config.host_name = impl.instance;
        config.hash_every_publishes = 5;
        config.reconnect.window_ms = kDebugReconnectWindowMs;
        impl.link = impl.net->endpoint(kHostPeerId);
        impl.host_session.reset(new HostSession(*impl.world, *impl.link, config));

        xrn::core::ClientConfig peer_config;
        peer_config.host_peer = kHostPeerId;
        peer_config.player_id = 1;
        peer_config.player_name = impl.instance + "-bot";
        impl.peer_link = impl.net->endpoint(kLoopbackPeerId);
        impl.peer_session.reset(new ClientSession(*impl.peer_link, peer_config));
        impl.net->connect(kHostPeerId, kLoopbackPeerId);
    } else {
        xrn::core::ClientConfig config;
        config.host_peer = kClientPeerId;
        config.player_id = env_uint("XRN_HARNESS_SLOT", 1) + 1;
        config.player_name = impl.instance;
        config.log = impl.log.get();
        // Listen-host semantics: a vanished host ends the session (EC-4).
        config.host_loss_grace_ms = 0;
        impl.link = impl.net->endpoint(kClientPeerId);
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
        impl.log->session_start(0, impl.host
                                       ? "headless host, in-process link (transport seam ready)"
                                       : "headless client, transport seam ready");
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
    impl.link.reset();
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
            }
            if (impl.environment != nullptr && impl.host_session != nullptr) {
                static_cast<void>(impl.environment->advance_real_ms(kStepMs));
                const std::vector<env::EnvironmentEvent> events = impl.environment->take_events();
                for (const env::EnvironmentEvent& event : events) {
                    impl.observe_environment_event(event);
                }
                if (!events.empty()) {
                    static_cast<void>(env::publish_environment_events(impl.host_session->bridge(),
                                                                      events, impl.env_sequence));
                }
                impl.host_session->publish();
            }
            impl.observe_session_deltas();
            impl.step_reconnect_cycle(now);
        } else if (impl.client_session != nullptr) {
            impl.client_session->service();
        }
    }

    // Core -> Lua (F0-05): event-driven dispatch only.
    static_cast<void>(coop::bridge().pump());

    // Host verdicts -> bot channel (F0-12).
    impl.drain_peer_verdicts(now);

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
        if (!impl.host || impl.host_session == nullptr || impl.peer_session == nullptr) {
            detail = "reconnect is a host-side debug scenario";
            return false;
        }
        impl.reconnect_expired = argument_value(argument, "window") == "expired";
        impl.reconnect_phase = 1;
        impl.reconnect_at_ms = 0;
        detail = impl.reconnect_expired ? "peer drops, window closes, peer returns"
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
    if (impl.active) {
        status.log_path = (fs::path(impl.run_dir) / (impl.instance + ".jsonl")).string();
    }
    status.error = impl.error;
    status.steps = impl.steps;
    status.world_tick = impl.world != nullptr ? impl.world->tick() : 0;
    status.events = impl.log != nullptr ? impl.log->size() : 0;
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
