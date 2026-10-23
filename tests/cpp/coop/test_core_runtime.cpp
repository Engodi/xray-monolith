////////////////////////////////////////////////////////////////////////////
//	Host-side test of the coop core wiring (F0-04/F0-05/F0-07/F0-12).
//
//	Runs the real adapter (`src/xrGame/coop/coop_core_runtime.cpp`) against the
//	real coop core, with the engine reduced to a stub (`stub/pch_script.h`):
//
//	  * the harness profile is armed through the F0-11 environment;
//	  * the core ticks at a fixed step, hosts a loopback peer and publishes
//	    environment events;
//	  * debug commands drive the host authority (weather / emission / clock);
//	  * the bot file channel is served end-to-end: inbox action -> host verdict
//	    -> outbox ack + observations;
//	  * `<run-dir>/<instance>.jsonl` is a valid F0-07 artifact (`obs::read_jsonl`).
//
//	This is the local stand-in for the CI end-to-end run: it catches API drift
//	between the engine adapter and the core long before the Windows build.
//
//	Build: tests/cpp/coop/run_core.sh
////////////////////////////////////////////////////////////////////////////

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "coop/coop_core_runtime.h"
#include "xrn/core/observability.hpp"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cout << "FAIL " << what << "\n";
        ++g_failures;
    }
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::vector<std::string> lines_of(const fs::path& path) {
    std::vector<std::string> lines;
    std::istringstream input(read_text(path));
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

// Gives the runtime real time to reach a deadline: the adapter paces itself on
// the process clock (a debug reconnect window is 600 ms in the debug profile).
void pump(coop::CCoreRuntime& runtime, int frames, int sleep_ms = 0) {
    for (int index = 0; index < frames; ++index) {
        runtime.tick();
        if (sleep_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
}

void push_action(const fs::path& inbox, unsigned seq, const std::string& body, const char* token) {
    std::ostringstream text;
    text << "{\"schema_version\":1,\"token\":\"" << token << "\",\"seq\":" << seq
         << ",\"sent_at_ms\":0,\"action\":{" << body << "}}";
    std::ostringstream name;
    name << std::setw(4) << std::setfill('0') << seq << ".json";
    write_text(inbox / name.str(), text.str());
}

bool has_observation(const fs::path& observations, const std::string& needle) {
    for (const std::string& line : lines_of(observations)) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "coop_core_runtime_test";
    fs::remove_all(root);
    fs::create_directories(root / "host" / "bot");

    setenv("XRN_HARNESS_RUN_DIR", root.string().c_str(), 1);
    setenv("XRN_HARNESS_INSTANCE", "host", 1);
    setenv("XRN_HARNESS_ROLE", "host", 1);
    setenv("XRN_HARNESS_SLOT", "0", 1);
    // This check is about the adapter logic, not sockets: the in-process profile
    // keeps it deterministic. The cross-process run is test_cross_process.cpp.
    setenv("XRN_HARNESS_NET", "inprocess", 1);

    coop::CCoreRuntime& runtime = coop::core_runtime();
    // The engine only calls tick() (CLevel::OnFrame): the first frame arms the
    // runtime from the harness environment.
    runtime.tick();
    check(runtime.active(), "runtime arms itself on the first frame");
    check(runtime.start(), "start() is idempotent after the first frame");

    const coop::SCoreStatus status = runtime.status();
    check(status.host, "profile is a host");
    check(status.instance == "host", "instance name comes from the environment");
    check(status.role == "host", "role comes from the environment");

    // The loopback peer joins, the environment publishes its three entities.
    pump(runtime, 20);
    check(runtime.status().steps > 0, "core ticks");
    check(runtime.status().world_tick > 0, "authoritative tick advances");

    const fs::path observations = root / "host" / "bot" / "observations.jsonl";
    check(fs::exists(observations), "observations stream is created");

    // --- debug commands through the bot channel -----------------------------
    const fs::path inbox = root / "host" / "bot" / "inbox";
    push_action(inbox, 1, "\"type\":\"send\",\"command\":\"debug.weather type=fog\"", "run-1");
    pump(runtime, 5);
    check(fs::exists(root / "host" / "bot" / "outbox" / "0001.json"), "weather action acked");
    check(has_observation(observations, "\"event\":\"weather\""), "weather observed");

    push_action(inbox, 2, "\"type\":\"send\",\"command\":\"debug.emission start\"", "run-1");
    pump(runtime, 5);
    check(has_observation(observations, "\"event\":\"emission_start\""), "emission start observed");

    push_action(inbox, 3, "\"type\":\"send\",\"command\":\"debug.clock sync\"", "run-1");
    pump(runtime, 5);
    check(has_observation(observations, "\"event\":\"time_sync\""), "time sync observed");

    // --- bot intent reaches HostSession and gets a verdict -------------------
    push_action(inbox, 4, "\"type\":\"pickup\",\"item\":\"bread\"", "run-1");
    pump(runtime, 10);
    check(fs::exists(root / "host" / "bot" / "outbox" / "0004.json"), "pickup acked by the host");
    check(has_observation(observations, "\"event\":\"pickup\""), "pickup verdict observed");

    // --- reconnect scenario (AC-1.12) ---------------------------------------
    push_action(inbox, 5, "\"type\":\"send\",\"command\":\"debug.reconnect window=ok\"", "run-1");
    pump(runtime, 60, 10);
    check(has_observation(observations, "\"event\":\"player_rejoin\""),
          "in-window rejoin observed");

    push_action(inbox, 6, "\"type\":\"send\",\"command\":\"debug.reconnect window=expired\"",
                "run-1");
    pump(runtime, 120, 10);
    check(has_observation(observations, "\"event\":\"window_expired\""), "expired window observed");

    // --- observability artifact (F0-07) -------------------------------------
    runtime.shutdown();

    const fs::path log_path = root / "host.jsonl";
    check(fs::exists(log_path), "<instance>.jsonl is written");
    std::istringstream input(read_text(log_path));
    std::vector<xrn::core::obs::Event> events;
    const xrn::core::obs::ReadResult read = xrn::core::obs::read_jsonl(input, events);
    check(read.ok(), "event log is valid F0-07 JSONL");
    check(!events.empty(), "event log is not empty");

    bool session_start = false;
    bool weather = false;
    bool emission = false;
    bool state_hash = false;
    for (const xrn::core::obs::Event& event : events) {
        session_start = session_start || event.kind == xrn::core::obs::EventKind::SessionStart;
        weather = weather || event.kind == xrn::core::obs::EventKind::Weather;
        emission = emission || event.kind == xrn::core::obs::EventKind::Emission;
        state_hash = state_hash || event.kind == xrn::core::obs::EventKind::StateHash;
    }
    check(session_start, "log has a session.start event");
    check(weather, "log has a weather event");
    check(emission, "log has an emission event");
    check(state_hash, "log has a state.hash sample");

    check(fs::exists(root / "host.metrics.json"), "metrics artifact is written");

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "core runtime: all checks passed\n";
    return 0;
}
