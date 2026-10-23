////////////////////////////////////////////////////////////////////////////
//	Host-side test of the bot action channel (F0-12 wiring).
//
//	`src/xrGame/coop/coop_bot_json.cpp` and `coop_bot_channel.cpp` are plain
//	C++17 with no engine dependency on purpose: the file protocol of the bot
//	contract can be verified on a host compiler, before the Windows engine build
//	(MSVC/VS2022, F0-01) ever sees it.
//
//	Covered:
//	  * JSON round-trip, escaping and the protocol's error cases;
//	  * ordering/token/seq rules of the inbox;
//	  * ack documents and the observation stream (`state`/`event`/`ack`);
//	  * `join_rejected` with the divergent paths (AC-1.10.1 shape).
//
//	Build: tests/cpp/coop/run.sh
////////////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "coop/coop_bot_channel.h"
#include "coop/coop_bot_json.h"

namespace fs = std::filesystem;
using coop::bot::CBotChannel;
using coop::bot::parse_json;
using coop::bot::SAction;
using coop::bot::SInboxMessage;
using coop::bot::SJson;
using coop::bot::SObservedState;
using coop::bot::to_json;

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

std::string inbox_action(std::uint32_t seq, const std::string& type, const std::string& field,
                         const std::string& value, const std::string& token) {
    std::ostringstream text;
    text << "{\"schema_version\":1,\"token\":\"" << token << "\",\"seq\":" << seq
         << ",\"sent_at_ms\":1000,\"action\":{\"type\":\"" << type << "\"";
    if (!field.empty()) {
        text << ",\"" << field << "\":\"" << value << "\"";
    }
    text << "}}";
    return text.str();
}

std::vector<std::string> observation_lines(const fs::path& path) {
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

void test_json() {
    SJson document;
    std::string error;
    const bool parsed = parse_json(
        "{\"kind\":\"event\",\"event\":\"join_rejected\",\"files\":[\"a.ltx\",\"b b.ltx\"],"
        "\"ok\":true,\"n\":3,\"ratio\":1.5,\"nil\":null,\"text\":\"quote\\\" and \\u0041\"}",
        document, error);
    check(parsed, "json: document parses");
    check(error.empty(), "json: no error on a valid document");

    std::string text;
    check(document.find("kind") != nullptr && document.find("kind")->as_string(text) &&
              text == "event",
          "json: string member");
    check(document.find("files") != nullptr && document.find("files")->items.size() == 2,
          "json: array member");
    std::int64_t integer = 0;
    check(document.find("n") != nullptr && document.find("n")->as_int(integer) && integer == 3,
          "json: integer member");
    bool flag = false;
    check(document.find("ok") != nullptr && document.find("ok")->as_bool(flag) && flag,
          "json: bool member");
    check(document.find("text") != nullptr && document.find("text")->as_string(text) &&
              text == "quote\" and A",
          "json: escapes and \\u");

    // Serialisation is compact, sorted and round-trips through the parser.
    const std::string encoded = to_json(document);
    SJson again;
    check(parse_json(encoded, again, error), "json: own output parses");
    check(encoded.find(": ") == std::string::npos && encoded.find(", ") == std::string::npos,
          "json: compact output");

    // Error cases: the parser must not accept partial documents.
    SJson bad;
    check(!parse_json("{\"a\":1,}", bad, error), "json: trailing comma rejected");
    check(!parse_json("{\"a\":", bad, error), "json: truncated rejected");
    check(!parse_json("{\"a\":1} extra", bad, error), "json: trailing garbage rejected");
    check(!parse_json("[1,2", bad, error), "json: unterminated array rejected");
}

void test_channel_ordering() {
    const fs::path root = fs::temp_directory_path() / "coop_bot_channel_ordering";
    fs::remove_all(root);

    CBotChannel channel;
    check(channel.open(root.string(), "client-1"), "channel opens");
    check(channel.ready(), "channel ready");

    const fs::path inbox = root / "client-1" / "bot" / "inbox";
    // Out of order on disk: only seq 1 may come first.
    write_text(inbox / "0002.json", inbox_action(2, "wait", "", "", "run-a"));
    write_text(inbox / "0001.json", inbox_action(1, "pickup", "item", "bread", "run-a"));

    SInboxMessage message;
    check(channel.poll(message), "first action is polled");
    check(message.seq == 1, "first action has seq 1");
    check(message.action.type == "pickup", "action type parsed");
    check(message.action.item == "bread", "action item parsed");
    check(message.token == "run-a", "token parsed");

    check(channel.poll(message), "second action is polled");
    check(message.seq == 2, "second action has seq 2");
    check(!channel.poll(message), "no third action");

    // A gap waits for the missing action instead of skipping it.
    write_text(inbox / "0004.json", inbox_action(4, "wait", "", "", "run-a"));
    check(!channel.poll(message), "gap is not skipped");

    // A new token restarts the sequence (a fresh run).
    write_text(inbox / "0001.json", inbox_action(1, "wait", "", "", "run-b"));
    check(channel.poll(message), "new token restarts the sequence");
    check(message.token == "run-b" && message.seq == 1, "new token action");
}

void test_ack_and_observations() {
    const fs::path root = fs::temp_directory_path() / "coop_bot_channel_observations";
    fs::remove_all(root);

    CBotChannel channel;
    check(channel.open(root.string(), "host"), "channel opens");

    SInboxMessage message;
    message.token = "run-1";
    message.seq = 3;
    message.action.type = "send";
    channel.ack(message, "done", 12);

    const fs::path ack_path = root / "host" / "bot" / "outbox" / "0003.json";
    check(fs::exists(ack_path), "ack file written");
    SJson ack;
    std::string error;
    check(parse_json(read_text(ack_path), ack, error), "ack is valid JSON");
    std::string status;
    std::int64_t seq = 0;
    check(ack.find("status") != nullptr && ack.find("status")->as_string(status) &&
              status == "done",
          "ack status");
    check(ack.find("seq") != nullptr && ack.find("seq")->as_int(seq) && seq == 3, "ack seq");
    check(ack.find("token") != nullptr, "ack token");

    channel.observe_event("weather", {{"world_ms", SJson::make_number(42)}});
    channel.observe_event_files("join_rejected", {"weapon_ammo.ltx", "mod_load_order.ltx"},
                                "cfg_diff");

    SObservedState state;
    state.actor.name = "client-1";
    state.actor.level = "l01_escape";
    state.actor.x = 1.0;
    state.inventory.push_back({"bread", 2});
    state.world_items.push_back({"artifact_01", 1});
    state.session_players = {"host", "client-1"};
    channel.observe_state(state);
    channel.flush();

    const std::vector<std::string> lines =
        observation_lines(root / "host" / "bot" / "observations.jsonl");
    check(lines.size() == 4, "four observations written");

    SJson first;
    check(parse_json(lines[0], first, error), "ack observation is JSON");
    check(first.find("kind") != nullptr, "ack observation has kind");

    SJson rejected;
    check(parse_json(lines[2], rejected, error), "join_rejected observation is JSON");
    std::string reason;
    check(rejected.find("reason") != nullptr && rejected.find("reason")->as_string(reason) &&
              reason == "cfg_diff",
          "join_rejected reason");
    check(rejected.find("files") != nullptr && rejected.find("files")->items.size() == 2,
          "join_rejected carries the divergent paths");

    SJson observed;
    check(parse_json(lines[3], observed, error), "state observation is JSON");
    check(observed.find("kind") != nullptr, "state observation has kind");
    check(observed.find("inventory") != nullptr &&
              observed.find("inventory")->find("items")->items.size() == 1,
          "state inventory");
    check(observed.find("world_items") != nullptr &&
              observed.find("world_items")->items.size() == 1,
          "state world items");
    check(observed.find("session") != nullptr &&
              observed.find("session")->find("players")->items.size() == 2,
          "state session players");
    check(observed.find("actor") != nullptr && observed.find("actor")->find("name") != nullptr,
          "state actor");
}

void test_action_fields() {
    const fs::path root = fs::temp_directory_path() / "coop_bot_channel_action";
    fs::remove_all(root);

    CBotChannel channel;
    check(channel.open(root.string(), "client-1"), "channel opens");

    const fs::path inbox = root / "client-1" / "bot" / "inbox";
    write_text(inbox / "0001.json",
               "{\"token\":\"t\",\"seq\":1,\"sent_at_ms\":5,\"action\":{\"type\":\"move_to\","
               "\"target\":\"waypoint\",\"pos\":{\"x\":1.5,\"y\":-2,\"z\":0.25}}}");

    SInboxMessage message;
    check(channel.poll(message), "action polled");
    check(message.action.has_pos, "position parsed");
    check(message.action.x == 1.5 && message.action.y == -2.0 && message.action.z == 0.25,
          "position values");
    check(message.action.target == "waypoint", "target parsed");
    check(message.sent_at_ms == 5, "sent_at_ms parsed");
}

}  // namespace

int main() {
    test_json();
    test_channel_ordering();
    test_ack_and_observations();
    test_action_fields();

    if (g_failures != 0) {
        std::cout << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "bot channel: all checks passed\n";
    return 0;
}
