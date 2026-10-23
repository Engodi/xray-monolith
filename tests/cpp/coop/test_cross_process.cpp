////////////////////////////////////////////////////////////////////////////
//	Cross-process check of the F0-02 transport wiring (coop-net)
//
//	Runs the real engine adapter (`src/xrGame/coop/coop_core_runtime.cpp`) in two
//	processes that talk over real UDP sockets on loopback:
//
//	  driver   — prepares the run directory, starts the host, waits for it to
//	             publish its port, starts the client, then verifies the artifacts;
//	  host     — arms the runtime as the authoritative host (UDP, ephemeral port),
//	             accepts the join, applies the client's pickup intent;
//	  client-1 — arms the runtime as a client, joins over UDP, sends a pickup
//	             intent and a debug intent, and answers its bot only when the
//	             host's verdict comes back over the network.
//
//	What it proves that the in-process check cannot:
//	  * `transport_link.cpp` (the F0-02 C ABI adapter) is composed into the wiring;
//	  * the core sessions exchange snapshots, intents and acks over the socket
//	    (no `InProcessNetwork` on this path);
//	  * the client's observable state comes from the mirrored snapshots;
//	  * a debug command from a client process is executed by the host and acked.
//
//	Build and run: tests/cpp/coop/run_core.sh
////////////////////////////////////////////////////////////////////////////

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "coop/coop_core_runtime.h"

namespace fs = std::filesystem;

namespace {

constexpr int kTickSleepMs = 10;
// Generous: the host has to boot a level in a real run; here it is only the
// session handshake, but a slow CI box must not turn into a flaky failure.
constexpr int kHostBudgetMs = 20000;
constexpr int kClientBudgetMs = 20000;
// The host outlives its last verdict by this much, so the client sees the ack.
constexpr int kHostGraceMs = 1500;
constexpr const char* kLoopback = "127.0.0.1";

int g_failures = 0;

void check(bool condition, const std::string& what) {
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

bool has_observation(const fs::path& observations, const std::string& needle) {
    for (const std::string& line : lines_of(observations)) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// The bot channel is only opened for an instance the runner prepared.
fs::path prepare_instance(const fs::path& run_dir, const std::string& instance) {
    const fs::path bot = run_dir / instance / "bot";
    fs::create_directories(bot / "inbox");
    fs::create_directories(bot / "outbox");
    return bot;
}

void push_action(const fs::path& bot, unsigned seq, const std::string& body) {
    std::ostringstream text;
    text << "{\"schema_version\":1,\"token\":\"cross\",\"seq\":" << seq
         << ",\"sent_at_ms\":0,\"action\":{" << body << "}}";
    std::ostringstream name;
    name << std::setw(4) << std::setfill('0') << seq << ".json";
    write_text(bot / "inbox" / name.str(), text.str());
}

void arm_environment(const std::string& run_dir, const std::string& instance,
                     const std::string& role, unsigned slot, const char* host_port) {
    setenv("XRN_HARNESS_RUN_DIR", run_dir.c_str(), 1);
    setenv("XRN_HARNESS_INSTANCE", instance.c_str(), 1);
    setenv("XRN_HARNESS_ROLE", role.c_str(), 1);
    setenv("XRN_HARNESS_SLOT", std::to_string(slot).c_str(), 1);
    setenv("XRN_HARNESS_HOST", kLoopback, 1);
    setenv("XRN_HARNESS_HOST_PORT", host_port, 1);
    // Host: ephemeral port, published in the run directory for the client.
    setenv("XRN_HARNESS_PORT", "0", 1);
    setenv("XRN_HARNESS_NET", "udp", 1);
}

void write_result(const fs::path& run_dir, const std::string& instance, bool pickup,
                  bool debug_ack, bool inventory, const std::string& transport,
                  unsigned peers, bool joined) {
    std::ostringstream text;
    text << "pickup=" << (pickup ? 1 : 0) << '\n'
         << "debug_ack=" << (debug_ack ? 1 : 0) << '\n'
         << "inventory=" << (inventory ? 1 : 0) << '\n'
         << "peers=" << peers << '\n'
         << "joined=" << (joined ? 1 : 0) << '\n'
         << "transport=" << transport << '\n';
    write_text(run_dir / instance / "result", text.str());
}

bool result_flag(const fs::path& run_dir, const std::string& instance, const char* key) {
    for (const std::string& line : lines_of(run_dir / instance / "result")) {
        if (line.rfind(std::string(key) + "=", 0) == 0) {
            return line.substr(std::strlen(key) + 1) == "1";
        }
    }
    return false;
}

std::string result_value(const fs::path& run_dir, const std::string& instance, const char* key) {
    for (const std::string& line : lines_of(run_dir / instance / "result")) {
        if (line.rfind(std::string(key) + "=", 0) == 0) {
            return line.substr(std::strlen(key) + 1);
        }
    }
    return std::string();
}

// --- host process ---------------------------------------------------------
int run_host(const fs::path& run_dir) {
    arm_environment(run_dir.string(), "host", "host", 0, "0");
    const fs::path bot = prepare_instance(run_dir, "host");

    coop::CCoreRuntime& runtime = coop::core_runtime();
    runtime.tick();
    if (!runtime.active()) {
        std::cout << "host: runtime did not arm: " << runtime.status().error << "\n";
        return 2;
    }

    const fs::path observations = bot / "observations.jsonl";
    bool pickup = false;
    unsigned max_peers = 0;
    int picked_at = -1;
    for (int elapsed = 0; elapsed < kHostBudgetMs; elapsed += kTickSleepMs) {
        runtime.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickSleepMs));
        max_peers = std::max(max_peers, runtime.status().peers);
        if (elapsed % (kTickSleepMs * 5) == 0) {
            // The client's pickup intent must have been applied by the authority.
            pickup = has_observation(observations, "\"event\":\"pickup\"");
            if (pickup && picked_at < 0) {
                picked_at = elapsed;
            }
        }
        // Stay a moment after the verdict left: the client must still be able to
        // receive its ack (it observes state only from the host's stream).
        if (pickup && elapsed >= picked_at + kHostGraceMs) {
            break;
        }
    }
    const coop::SCoreStatus status = runtime.status();
    runtime.shutdown();
    write_result(run_dir, "host", pickup, false, false, status.transport, max_peers,
                 status.joined);
    if (!pickup) {
        std::cout << "host: pickup=" << pickup << " peers=" << max_peers
                  << " transport=" << status.transport << "\n";
        return 1;
    }
    return 0;
}

// --- client process -------------------------------------------------------
int run_client(const fs::path& run_dir) {
    arm_environment(run_dir.string(), "client-1", "client", 1, "0");
    const fs::path bot = prepare_instance(run_dir, "client-1");

    coop::CCoreRuntime& runtime = coop::core_runtime();
    runtime.tick();
    if (!runtime.active()) {
        std::cout << "client: runtime did not arm: " << runtime.status().error << "\n";
        return 2;
    }

    const fs::path observations = bot / "observations.jsonl";
    bool pickup_acked = false;
    bool debug_acked = false;
    bool pickup = false;
    bool inventory = false;
    bool sent_pickup = false;
    
    for (int elapsed = 0; elapsed < kClientBudgetMs; elapsed += kTickSleepMs) {
        runtime.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickSleepMs));
        const coop::SCoreStatus status = runtime.status();
        if (status.joined && !sent_pickup) {
            push_action(bot, 1, "\"type\":\"pickup\",\"item\":\"bread\"");
            push_action(bot, 2, "\"type\":\"send\",\"command\":\"debug.clock sync\"");
            sent_pickup = true;

        }
        if (elapsed % (kTickSleepMs * 5) != 0) {
            continue;
        }
        pickup_acked = pickup_acked || fs::exists(bot / "outbox" / "0001.json");
        debug_acked = debug_acked || fs::exists(bot / "outbox" / "0002.json");
        // The client sees the result only through mirrored snapshots.
        pickup = has_observation(observations, "\"event\":\"pickup\"");
        for (const std::string& line : lines_of(observations)) {
            if (line.find("\"kind\":\"state\"") != std::string::npos &&
                line.find("\"id\":\"bread\"") != std::string::npos) {
                inventory = true;
                break;
            }
        }
        if (pickup && debug_acked && inventory) {
            break;
        }
    }
    const coop::SCoreStatus status = runtime.status();
    runtime.shutdown();
    write_result(run_dir, "client-1", pickup, debug_acked, inventory, status.transport,
                 status.peers, status.joined);
    if (!pickup || !debug_acked || !inventory) {
        std::cout << "client: joined=" << status.joined << " pickup=" << pickup
                  << " debug_ack=" << debug_acked << " inventory=" << inventory
                  << " acked=" << pickup_acked << " transport=" << status.transport << "\n";
        return 1;
    }
    return 0;
}

// --- driver ---------------------------------------------------------------
std::string self_executable() {
    char buffer[4096] = {};
    const ssize_t size = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (size <= 0) {
        return std::string();
    }
    return std::string(buffer, static_cast<std::size_t>(size));
}

pid_t spawn(const std::string& exe, const char* mode) {
    const pid_t pid = ::fork();
    if (pid != 0) {
        return pid;
    }
    ::execl(exe.c_str(), exe.c_str(), mode, static_cast<char*>(nullptr));
    ::_exit(127);
}

// Waits for the child or kills it. Returns true when it exited with 0.
bool wait_for(pid_t pid, int budget_ms) {
    for (int elapsed = 0; elapsed < budget_ms; elapsed += kTickSleepMs) {
        int status = 0;
        const pid_t result = ::waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickSleepMs));
    }
    ::kill(pid, SIGKILL);
    static_cast<void>(::waitpid(pid, nullptr, 0));
    return false;
}

bool wait_for_file(const fs::path& path, int budget_ms) {
    for (int elapsed = 0; elapsed < budget_ms; elapsed += kTickSleepMs) {
        if (fs::exists(path)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickSleepMs));
    }
    return false;
}

int run_driver(const fs::path& run_dir) {
    const std::string exe = self_executable();
    if (exe.empty()) {
        std::cout << "driver: cannot resolve the executable path\n";
        return 2;
    }
    prepare_instance(run_dir, "host");
    prepare_instance(run_dir, "client-1");

    const pid_t host = spawn(exe, "host");
    if (host < 1) {
        std::cout << "driver: cannot start the host process\n";
        return 2;
    }
    // The host publishes its ephemeral port as soon as the transport is up.
    if (!wait_for_file(run_dir / "transport.port", kHostBudgetMs)) {
        std::cout << "driver: the host published no transport port\n";
        ::kill(host, SIGKILL);
        static_cast<void>(::waitpid(host, nullptr, 0));
        return 1;
    }
    const pid_t client = spawn(exe, "client-1");
    if (client < 1) {
        std::cout << "driver: cannot start the client process\n";
        ::kill(host, SIGKILL);
        static_cast<void>(::waitpid(host, nullptr, 0));
        return 2;
    }

    const bool client_ok = wait_for(client, kClientBudgetMs + 5000);
    const bool host_ok = wait_for(host, kHostBudgetMs);

    check(host_ok, "host process finished its checks");
    check(client_ok, "client process finished its checks");

    const std::string host_transport = result_value(run_dir, "host", "transport");
    const std::string client_transport = result_value(run_dir, "client-1", "transport");
    check(host_transport.find("xnt") != std::string::npos, "host reports the xnt transport");
    check(host_transport.find("udp host") != std::string::npos, "host endpoint is a UDP host");
    check(client_transport.find("udp client") != std::string::npos,
          "client endpoint is a UDP client");
    check(result_flag(run_dir, "client-1", "joined"), "client joined the session over UDP");
    check(result_flag(run_dir, "client-1", "pickup"), "client got its pickup verdict");
    check(result_flag(run_dir, "client-1", "debug_ack"),
          "client got the debug verdict over the network");
    check(result_flag(run_dir, "client-1", "inventory"),
          "client sees its inventory through snapshots");
    check(result_flag(run_dir, "host", "pickup"), "host applied the client's intent");
    check(result_value(run_dir, "host", "peers") == "2",
          "host saw the client's player row in the session");

    // Artifacts of both processes are valid F0-07 files with session events.
    for (const char* instance : {"host", "client-1"}) {
        const fs::path log = run_dir / (std::string(instance) + ".jsonl");
        check(fs::exists(log), std::string(instance) + ".jsonl is written");
        check(!lines_of(log).empty(), std::string(instance) + ".jsonl is not empty");
        const fs::path metrics = run_dir / (std::string(instance) + ".metrics.json");
        check(fs::exists(metrics), std::string(instance) + ".metrics.json is written");
    }

    if (g_failures != 0) {
        std::cout << g_failures << " cross-process check(s) failed\n";
        return 1;
    }
    std::cout << "cross-process transport: all checks passed\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "driver";
    if (mode == "host" || mode == "client-1") {
        // The child inherits the run directory from the driver's environment.
        const char* run_dir = std::getenv("XRN_HARNESS_RUN_DIR");
        if (run_dir == nullptr) {
            std::cout << mode << ": no run directory\n";
            return 2;
        }
        return mode == "host" ? run_host(run_dir) : run_client(run_dir);
    }

    const fs::path run_dir = fs::temp_directory_path() / "coop_transport_cross_process";
    fs::remove_all(run_dir);
    fs::create_directories(run_dir);
    // The children read the run directory from the environment; the driver sets
    // it once, so both processes agree on the artifacts.
    setenv("XRN_HARNESS_RUN_DIR", run_dir.string().c_str(), 1);
    return run_driver(run_dir);
}
