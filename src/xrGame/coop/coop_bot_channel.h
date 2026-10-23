////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_bot_channel.h
//	Created 	: 23.10.2026
//	Description : File channel of a headless bot (F0-12 wiring, engine side)
//
//	The bot contract (coop repository, `tools/bots/README.md`) is a
//	platform-neutral file protocol, so the engine can be a bot without new
//	ports or privileges:
//
//	  <run-dir>/<instance>/bot/inbox/NNNN.json        runner -> bot: action
//	  <run-dir>/<instance>/bot/outbox/NNNN.json       bot -> runner: ack
//	  <run-dir>/<instance>/bot/observations.jsonl     bot -> runner: state/event/ack
//
//	This module owns the mechanics of that protocol only: ordering, tokens,
//	atomic writes, JSON reading/writing. It knows nothing about the coop core
//	or the engine, so it is unit-tested on a host compiler
//	(`tests/cpp/coop/test_bot_channel.cpp`).
//
//	The scaling key is the instance directory from the harness environment
//	(`$XRN_HARNESS_RUN_DIR/$XRN_HARNESS_INSTANCE`); the coop runtime resolves it
//	once at start and refuses to open a channel outside that directory.
////////////////////////////////////////////////////////////////////////////

#ifndef COOP_BOT_CHANNEL_H_INCLUDED
#define COOP_BOT_CHANNEL_H_INCLUDED

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coop/coop_bot_json.h"

namespace coop {
namespace bot {

// One action as it arrives from the scenario runner.
struct SAction {
    std::string type;
    std::string item;
    std::string command;
    std::string target;
    bool has_pos = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct SInboxMessage {
    std::string token;
    std::uint32_t seq = 0;
    std::int64_t sent_at_ms = 0;
    SAction action;
};

struct SActorState {
    std::string name;
    std::string level;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    std::int64_t hp = 100;
    bool alive = true;
    std::vector<std::string> equipped;
};

struct SItemState {
    std::string id;
    std::int64_t count = 1;
};

// What the bot observes right now — exactly the surface of the contract, no
// engine internals.
struct SObservedState {
    SActorState actor;
    std::vector<SItemState> inventory;
    std::int64_t money_uah = 0;
    std::vector<SActorState> players;
    std::vector<SItemState> world_items;
    std::vector<std::string> session_players;
    bool paused = false;
};

class CBotChannel {
public:
    CBotChannel() = default;
    ~CBotChannel();

    CBotChannel(const CBotChannel&) = delete;
    CBotChannel& operator=(const CBotChannel&) = delete;

    // Creates `<run_dir>/<instance>/bot/{inbox,outbox}` and opens the
    // observations log. False on a filesystem error (see `error()`).
    bool open(const std::string& run_dir, const std::string& instance);

    [[nodiscard]] bool ready() const { return m_ready; }
    [[nodiscard]] const std::string& error() const { return m_error; }
    [[nodiscard]] const std::string& instance() const { return m_instance; }

    // Next action in `seq` order. The token rules of the contract hold: a new
    // token restarts the sequence, `seq <= last_seq` is ignored and a gap waits
    // for the missing action (the runner sends strictly in order).
    bool poll(SInboxMessage& out);

    // Verdicts back to the runner.
    void ack(const SInboxMessage& message, const char* status, std::int64_t latency_ms,
             const std::string& failure = std::string());
    void observe_event(const std::string& name,
                       const std::vector<std::pair<std::string, SJson>>& fields);
    void observe_state(const SObservedState& state);
    // Appends an event that carries the divergent build paths (AC-1.10.1) —
    // the shape `join_rejected` has in the e2e assertions.
    void observe_event_files(const std::string& name, const std::vector<std::string>& files,
                             const std::string& reason);

    // Flushes the observations log to disk (called once per runtime tick).
    void flush();

private:
    bool write_atomic(const std::string& path, const std::string& payload);
    void append_observation(const SJson& payload);
    std::string inbox_dir() const;
    std::string outbox_dir() const;
    std::string observations_path() const;

    std::string m_run_dir;
    std::string m_instance;
    std::string m_error;
    std::string m_token;
    std::uint32_t m_last_seq = 0;
    bool m_ready = false;
    void* m_log = nullptr;  // std::FILE*, kept opaque in the header
};

// Compact JSON text of `value` (declared here so every caller of the channel
// shares one serialisation path).
[[nodiscard]] std::string to_json(const SJson& value);

}  // namespace bot
}  // namespace coop

#endif  // COOP_BOT_CHANNEL_H_INCLUDED
