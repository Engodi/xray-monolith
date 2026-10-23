////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_core_runtime.h
//	Created 	: 23.10.2026
//	Description : Engine wiring of the coop core (F0-04/F0-05/F0-07/F0-12)
//
//	The coop core (`xrn::core`, built from the `coop` checkout as a sibling of
//	the engine tree) is engine-free: it ticks, replicates and logs on its own.
//	This adapter is the only place where the engine touches it:
//
//	  * CLevel::OnFrame -> tick(): fixed-step session tick (service/publish),
//	    world clock, environment events, bridge pump and artifact flush;
//	  * core event log -> `<run-dir>/<instance>.jsonl` (F0-07 format consumed by
//	    `xrn_core_runlog merge` / `testrun.sh diff`);
//	  * core events -> engine Lua bridge (`coop.sync`/`coop.events` channels,
//	    F0-05) so the gameplay layer sees the same world events as the
//	    observability artifact;
//	  * bot action channel (F0-12): `<run-dir>/<instance>/bot/inbox` intents are
//	    resolved by the host session and answered in `outbox` + observations.
//
//	Everything here is armed only by the harness environment
//	(`XRN_HARNESS_RUN_DIR`/`_INSTANCE`/`_ROLE`, set by F0-11) inside a headless
//	profile. A normal game process has no such variables and pays one test.
//
//	NFR-ARCH-9: the engine keeps a single adapter file pair; the core stays a
//	separate library. NFR-ARCH-10: only events cross into Lua, never per-frame
//	state.
////////////////////////////////////////////////////////////////////////////

#ifndef COOP_CORE_RUNTIME_H_INCLUDED
#define COOP_CORE_RUNTIME_H_INCLUDED

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace coop {

// Status of the wiring, for the Lua/debug surface (`coop_runtime.status()`).
struct SCoreStatus {
    bool active = false;           // the harness profile is armed and the runtime runs
    bool host = false;             // authoritative side
    bool degraded = false;         // started, but the core refused part of the wiring
    std::string role;              // "host" | "client" | ""
    std::string instance;          // harness instance name ("host", "client-1", ...)
    std::string run_dir;           // <run-dir> of the run
    std::string log_path;          // <run-dir>/<instance>.jsonl
    std::string transport;         // F0-02: "xnt <ver> (abi N) udp host 127.0.0.1:port"
    std::string error;             // last start/wiring error
    std::uint32_t steps = 0;       // core ticks executed
    std::uint32_t world_tick = 0;  // authoritative tick (host) / host tick (client)
    std::uint32_t peers = 0;       // players in the session
    bool joined = false;           // client: the host accepted the join
    std::size_t events = 0;        // events in the F0-07 log ring
};

// One per process. `start()` is idempotent; `tick()` is called from the level
// frame hook and does nothing until the profile is armed.
class CCoreRuntime {
public:
    static CCoreRuntime& instance();

    CCoreRuntime();
    ~CCoreRuntime();

    CCoreRuntime(const CCoreRuntime&) = delete;
    CCoreRuntime& operator=(const CCoreRuntime&) = delete;

    // Reads the harness environment and wires the core. False when the process
    // is not a harness instance (nothing to do) or the wiring failed.
    bool start();

    // Stops the runtime: flushes the F0-07 log and the metrics artifact.
    void shutdown();

    // Frame hook (CLevel::OnFrame). Fixed-step: the core never sees a variable
    // frame delta.
    void tick();

    [[nodiscard]] bool active() const;

    // Debug surface (PLAN §8.11). `name` is the command the bot sends
    // (`debug.weather`, `debug.emission`, `debug.clock`, `debug.reconnect`,
    // `debug.cfg_probe`, `debug.level_change`); `argument` keeps the raw tail
    // (`type=fog`, `window=ok`, ...). False = unknown/unsupported command (the
    // caller reports it instead of pretending it worked).
    bool command(const std::string& name, const std::string& argument, std::string& detail);

    [[nodiscard]] SCoreStatus status() const;

private:
    struct SImpl;
    std::unique_ptr<SImpl> m_impl;
};

// Convenience accessor used by the frame hook and the Lua exports.
CCoreRuntime& core_runtime();

}  // namespace coop

#endif  // COOP_CORE_RUNTIME_H_INCLUDED
