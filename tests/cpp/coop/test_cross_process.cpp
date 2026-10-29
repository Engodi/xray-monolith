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

std::size_t count_observations(const fs::path& observations, const std::string& needle) {
    std::size_t count = 0;
    for (const std::string& line : lines_of(observations)) {
        if (line.find(needle) != std::string::npos) {
            ++count;
        }
    }
    return count;
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

// One more `key=1|0` line of the result file (the fixed table above stays small,
// while a check added later does not have to renumber the arguments).
void append_result(const fs::path& run_dir, const std::string& instance, const char* key,
                   bool value) {
    std::ofstream output(run_dir / instance / "result", std::ios::binary | std::ios::app);
    output << key << '=' << (value ? 1 : 0) << '\n';
}

// Observations appended after `from` (0-based line index): used to prove that a
// client still sees its state *after* the reconnect, not only before it.
bool has_observation_from(const fs::path& observations, std::size_t from,
                          const std::string& needle) {
    const std::vector<std::string> lines = lines_of(observations);
    for (std::size_t index = from; index < lines.size(); ++index) {
        if (lines[index].find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::size_t line_count(const fs::path& path) { return lines_of(path).size(); }

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
    // The client writes this marker when its scenario (pickup, weather, move,
    // drop, reconnect both ways) is done: the host must stay up for all of it.
    const fs::path done = run_dir / "client-1" / "done";
    bool pickup = false;
    bool drop = false;
    unsigned max_peers = 0;
    for (int elapsed = 0; elapsed < kHostBudgetMs; elapsed += kTickSleepMs) {
        runtime.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickSleepMs));
        max_peers = std::max(max_peers, runtime.status().peers);
        if (elapsed % (kTickSleepMs * 5) == 0) {
            // The client's pickup/drop intents must have been applied by the authority.
            pickup = pickup || has_observation(observations, "\"event\":\"pickup\"");
            drop = drop || has_observation(observations, "\"event\":\"drop\"");
        }
        if (fs::exists(done)) {
            // Let the last verdicts/events reach the client before the session ends.
            for (int grace = 0; grace < kHostGraceMs; grace += kTickSleepMs) {
                runtime.tick();
                std::this_thread::sleep_for(std::chrono::milliseconds(kTickSleepMs));
            }
            break;
        }
    }
    const coop::SCoreStatus status = runtime.status();
    runtime.shutdown();
    write_result(run_dir, "host", pickup, false, false, status.transport, max_peers,
                 status.joined);
    append_result(run_dir, "host", "drop", drop);
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
    const fs::path outbox = bot / "outbox";
    bool sent_pickup = false;
    bool pickup = false;
    bool inventory = false;
    bool inventory_after_rejoin = false;
    bool weather = false;
    bool rejoin = false;
    bool expired = false;
    bool rejoin_sent = false;
    bool expired_sent = false;
    bool rejoin_acked = false;
    bool expired_acked = false;
    bool joined_after_rejoin = false;
    bool moved = false;
    bool dropped = false;
    bool move_sent = false;
    bool drop_sent = false;
    bool repick_sent = false;
    bool repicked = false;
    std::size_t rejoin_index = 0;

    for (int elapsed = 0; elapsed < kClientBudgetMs; elapsed += kTickSleepMs) {
        runtime.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(kTickSleepMs));
        const coop::SCoreStatus status = runtime.status();
        if (status.joined && !sent_pickup) {
            // 1: a pickup intent (the host's authority decides), 2: a host-side
            // world event (weather) the client must observe over the network.
            push_action(bot, 1, "\"type\":\"pickup\",\"item\":\"bread\"");
            push_action(bot, 2, "\"type\":\"send\",\"command\":\"debug.weather type=fog\"");
            sent_pickup = true;
        }
        if (elapsed % (kTickSleepMs * 5) != 0) {
            continue;
        }
        const bool pickup_acked = fs::exists(outbox / "0001.json");
        const bool weather_acked = fs::exists(outbox / "0002.json");
        // The client sees the result only through mirrored snapshots and the
        // host's event stream (F0-02).
        pickup = pickup || has_observation(observations, "\"event\":\"pickup\"");
        weather = weather || has_observation(observations, "\"event\":\"weather\"");
        for (const std::string& line : lines_of(observations)) {
            if (line.find("\"kind\":\"state\"") != std::string::npos &&
                line.find("\"id\":\"bread\"") != std::string::npos) {
                inventory = true;
                break;
            }
        }

        // Step 3: F1-01 movement. The intent travels to the host, which moves the
        // actor record of this peer's slot; the new position is only observable
        // through the mirror.
        if (!move_sent && pickup_acked && weather_acked && inventory && weather) {
            push_action(bot, 3,
                        "\"type\":\"move_to\",\"target\":\"waypoint\","
                        "\"pos\":{\"x\":12,\"y\":0,\"z\":4}");
            move_sent = true;
        }
        if (move_sent && !moved) {
            for (const std::string& line : lines_of(observations)) {
                if (line.find("\"kind\":\"state\"") != std::string::npos &&
                    line.find("\"actor\":{") != std::string::npos &&
                    line.find("\"pos\":[12,0,4]") != std::string::npos) {
                    moved = true;
                    break;
                }
            }
        }

        // Step 4: F1-02 drop. The same item entity goes back into the world (no
        // duplicate) and leaves this player's inventory.
        if (moved && !drop_sent) {
            push_action(bot, 4, "\"type\":\"drop\",\"item\":\"bread\"");
            drop_sent = true;
        }
        dropped = dropped || has_observation(observations, "\"event\":\"drop\"");

        // Step 5: pick the dropped item up again, so the inventory of the resumed
        // character has something to keep across the reconnect (F1-10).
        if (dropped && !repick_sent) {
            push_action(bot, 5, "\"type\":\"pickup\",\"item\":\"bread\"");
            repick_sent = true;
        }
        repicked = repicked || count_observations(observations, "\"event\":\"pickup\"") >= 2;

        // Step 6: the host drops this peer's link; the client must recover the
        // session on its own and be resumed inside the window (F1-10).
        if (repicked && !rejoin_sent) {
            push_action(bot, 6, "\"type\":\"send\",\"command\":\"debug.reconnect window=ok\"");
            rejoin_sent = true;
        }
        if (rejoin_sent) {
            rejoin_acked = rejoin_acked || fs::exists(outbox / "0006.json");
            if (!rejoin) {
                rejoin = has_observation(observations, "\"event\":\"player_rejoin\"");
                if (rejoin) {
                    // Everything after this line is observed *after* the rejoin.
                    rejoin_index = line_count(observations);
                }
            }
            joined_after_rejoin = joined_after_rejoin || status.joined;
            if (rejoin) {
                inventory_after_rejoin =
                    inventory_after_rejoin ||
                    has_observation_from(observations, rejoin_index, "\"id\":\"bread\"");
            }
        }

        // Step 7: the same drop, but the host closes the window at once: the
        // return is an explicit "window expired" with a new character.
        if (rejoin && rejoin_acked && !expired_sent) {
            push_action(bot, 7, "\"type\":\"send\",\"command\":\"debug.reconnect window=expired\"");
            expired_sent = true;
        }
        if (expired_sent) {
            expired_acked = expired_acked || fs::exists(outbox / "0007.json");
            expired = expired || has_observation(observations, "\"event\":\"window_expired\"");
        }
        if (expired && expired_acked && inventory_after_rejoin) {
            break;
        }
    }
    // The host stays up until this marker appears.
    write_text(run_dir / "client-1" / "done", "done\n");
    const coop::SCoreStatus status = runtime.status();
    runtime.shutdown();
    write_result(run_dir, "client-1", pickup, expired_acked, inventory, status.transport,
                 status.peers, joined_after_rejoin || status.joined);
    append_result(run_dir, "client-1", "moved", moved);
    append_result(run_dir, "client-1", "move_ack", fs::exists(outbox / "0003.json"));
    append_result(run_dir, "client-1", "dropped", dropped);
    append_result(run_dir, "client-1", "drop_ack", fs::exists(outbox / "0004.json"));
    append_result(run_dir, "client-1", "repick", repicked);
    if (!pickup || !expired_acked || !inventory) {
        std::cout << "client: joined=" << status.joined << " pickup=" << pickup
                  << " expired_ack=" << expired_acked << " inventory=" << inventory
                  << " transport=" << status.transport << "\n";
        return 1;
    }
    if (!weather || !rejoin || !inventory_after_rejoin || !expired || !rejoin_acked ||
        !joined_after_rejoin || !moved || !dropped || !repicked) {
        std::cout << "client: weather=" << weather << " rejoin=" << rejoin
                  << " rejoin_ack=" << rejoin_acked << " inventory_after=" << inventory_after_rejoin
                  << " expired=" << expired << " joined_after=" << joined_after_rejoin
                  << " moved=" << moved << " dropped=" << dropped << " repick=" << repicked
                  << " transport=" << status.transport << "\n";
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
          "client got the reconnect verdict over the network");
    check(result_flag(run_dir, "client-1", "inventory"),
          "client sees its inventory through snapshots");
    check(result_flag(run_dir, "host", "pickup"), "host applied the client's intent");
    check(result_value(run_dir, "host", "peers") == "2",
          "host saw the client's player row in the session");

    // F1-01/F1-02 over the network: the host applied the client's movement and drop
    // intents, and the client saw the results through mirrored snapshots only.
    check(result_flag(run_dir, "client-1", "move_ack"), "client got its move verdict over UDP");
    check(result_flag(run_dir, "client-1", "moved"),
          "the client sees its own character at the waypoint");
    check(result_flag(run_dir, "client-1", "drop_ack"), "client got its drop verdict over UDP");
    check(result_flag(run_dir, "client-1", "dropped"), "the client observed the item drop");
    check(result_flag(run_dir, "host", "drop"), "host applied the client's drop");

    // F0-02 event fan-out: the world events the host publishes reach the client
    // bot (`observer = peer` over the network, not only in-process).
    const fs::path host_observations = run_dir / "host" / "bot" / "observations.jsonl";
    const fs::path client_observations = run_dir / "client-1" / "bot" / "observations.jsonl";
    for (const char* event : {"weather", "player_rejoin", "window_expired"}) {
        const std::string needle = std::string("\"event\":\"") + event + "\"";
        check(has_observation(host_observations, needle),
              std::string("host observed ") + event);
        check(has_observation(client_observations, needle),
              std::string("client observed ") + event);
    }
    // F1-10 over the network: the client came back into the session after the drop.
    check(has_observation(client_observations, "\"event\":\"player_rejoin\""),
          "client returned inside the reconnect window");

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
