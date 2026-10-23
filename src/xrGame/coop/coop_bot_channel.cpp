////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_bot_channel.cpp
//	Created 	: 23.10.2026
//	Description : File channel of a headless bot (F0-12 wiring, engine side)
////////////////////////////////////////////////////////////////////////////

#include "coop/coop_bot_channel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace coop {
namespace bot {
namespace {

namespace fs = std::filesystem;

std::int64_t epoch_ms() {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    out = buffer.str();
    return true;
}

// Parses one inbox document. A malformed message is a protocol error, not an
// action: the caller reports it and skips the file (the runner never retries).
bool parse_message(const std::string& text, SInboxMessage& out, std::string& error) {
    SJson document;
    if (!parse_json(text, document, error)) {
        return false;
    }
    if (document.type != SJson::EType::Object) {
        error = "message is not an object";
        return false;
    }
    const SJson* token = document.find("token");
    const SJson* seq = document.find("seq");
    const SJson* action = document.find("action");
    if (token == nullptr || seq == nullptr || action == nullptr) {
        error = "message has no token/seq/action";
        return false;
    }
    if (!token->as_string(out.token)) {
        error = "token is not a string";
        return false;
    }
    std::int64_t sequence = 0;
    if (!seq->as_int(sequence) || sequence <= 0) {
        error = "seq is not a positive integer";
        return false;
    }
    out.seq = static_cast<std::uint32_t>(sequence);
    if (const SJson* sent = document.find("sent_at_ms")) {
        std::int64_t at = 0;
        if (sent->as_int(at)) {
            out.sent_at_ms = at;
        }
    }
    if (action->type != SJson::EType::Object) {
        error = "action is not an object";
        return false;
    }
    const SJson* type = action->find("type");
    if (type == nullptr || !type->as_string(out.action.type)) {
        error = "action has no type";
        return false;
    }
    if (const SJson* item = action->find("item")) {
        static_cast<void>(item->as_string(out.action.item));
    }
    if (const SJson* command = action->find("command")) {
        static_cast<void>(command->as_string(out.action.command));
    }
    if (const SJson* target = action->find("target")) {
        static_cast<void>(target->as_string(out.action.target));
    }
    if (const SJson* pos = action->find("pos")) {
        if (pos->type == SJson::EType::Object) {
            const SJson* x_value = pos->find("x");
            const SJson* y_value = pos->find("y");
            const SJson* z_value = pos->find("z");
            if (x_value == nullptr || y_value == nullptr || z_value == nullptr ||
                !x_value->as_number(out.action.x) || !y_value->as_number(out.action.y) ||
                !z_value->as_number(out.action.z)) {
                error = "action.pos is incomplete";
                return false;
            }
            out.action.has_pos = true;
        }
    }
    return true;
}

SJson item_list(const std::vector<SItemState>& items) {
    SJson list = SJson::make_array();
    for (const SItemState& item : items) {
        SJson row = SJson::make_object();
        row.set("id", SJson::make_string(item.id));
        row.set("count", SJson::make_number(static_cast<double>(item.count)));
        list.push(std::move(row));
    }
    return list;
}

SJson actor_object(const SActorState& actor) {
    SJson row = SJson::make_object();
    row.set("name", SJson::make_string(actor.name));
    row.set("level", SJson::make_string(actor.level));
    SJson pos = SJson::make_array();
    pos.push(SJson::make_number(actor.x));
    pos.push(SJson::make_number(actor.y));
    pos.push(SJson::make_number(actor.z));
    row.set("pos", std::move(pos));
    row.set("hp", SJson::make_number(static_cast<double>(actor.hp)));
    SJson equipped = SJson::make_array();
    for (const std::string& item : actor.equipped) {
        equipped.push(SJson::make_string(item));
    }
    row.set("equipped", std::move(equipped));
    row.set("alive", SJson::make_bool(actor.alive));
    return row;
}

std::string four_digits(std::uint32_t seq) {
    std::string digits = std::to_string(seq);
    while (digits.size() < 4) {
        digits.insert(digits.begin(), '0');
    }
    return digits;
}

}  // namespace

CBotChannel::~CBotChannel() {
    if (m_log != nullptr) {
        std::fclose(static_cast<std::FILE*>(m_log));
        m_log = nullptr;
    }
}

std::string CBotChannel::inbox_dir() const {
    return (fs::path(m_run_dir) / m_instance / "bot" / "inbox").string();
}

std::string CBotChannel::outbox_dir() const {
    return (fs::path(m_run_dir) / m_instance / "bot" / "outbox").string();
}

std::string CBotChannel::observations_path() const {
    return (fs::path(m_run_dir) / m_instance / "bot" / "observations.jsonl").string();
}

bool CBotChannel::open(const std::string& run_dir, const std::string& instance) {
    m_run_dir = run_dir;
    m_instance = instance;
    m_ready = false;
    m_error.clear();
    m_token.clear();
    m_last_seq = 0;

    if (m_run_dir.empty() || m_instance.empty()) {
        m_error = "run dir / instance is not set";
        return false;
    }

    std::error_code code;
    fs::create_directories(inbox_dir(), code);
    if (code) {
        m_error = "cannot create the inbox: " + code.message();
        return false;
    }
    fs::create_directories(outbox_dir(), code);
    if (code) {
        m_error = "cannot create the outbox: " + code.message();
        return false;
    }

    if (m_log == nullptr) {
        std::FILE* file = std::fopen(observations_path().c_str(), "ab");
        if (file == nullptr) {
            m_error = "cannot open observations.jsonl";
            return false;
        }
        m_log = file;
    }
    m_ready = true;
    return true;
}

bool CBotChannel::poll(SInboxMessage& out) {
    if (!m_ready) {
        return false;
    }

    std::error_code code;
    std::vector<std::pair<std::uint32_t, std::string>> candidates;
    for (fs::directory_iterator it(inbox_dir(), code), end; !code && it != end;
         it.increment(code)) {
        if (!it->is_regular_file()) {
            continue;
        }
        const std::string name = it->path().filename().string();
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) {
            continue;
        }
        const std::string stem = name.substr(0, name.size() - 5);
        std::uint32_t seq = 0;
        bool number = !stem.empty();
        for (const char digit : stem) {
            if (digit < '0' || digit > '9') {
                number = false;
                break;
            }
            seq = seq * 10U + static_cast<std::uint32_t>(digit - '0');
        }
        if (number) {
            candidates.emplace_back(seq, it->path().string());
        }
    }
    if (candidates.empty()) {
        return false;
    }
    std::sort(candidates.begin(), candidates.end());

    for (const auto& candidate : candidates) {
        std::string text;
        if (!read_file(candidate.second, text)) {
            m_error = "cannot read " + candidate.second;
            return false;
        }
        SInboxMessage message;
        std::string parse_error;
        if (!parse_message(text, message, parse_error)) {
            m_error = fs::path(candidate.second).filename().string() + ": " + parse_error;
            // A malformed message cannot be executed; skip it so a single broken
            // document does not block the run.
            m_last_seq = std::max(m_last_seq, candidate.first);
            continue;
        }
        if (message.token != m_token) {
            // New run: the runner clears inbox/outbox and starts at seq 1.
            m_token = message.token;
            m_last_seq = 0;
        }
        if (message.seq <= m_last_seq) {
            continue;  // replay of an already handled action
        }
        if (message.seq != m_last_seq + 1) {
            return false;  // wait for the missing action: the runner sends in order
        }
        m_last_seq = message.seq;
        out = std::move(message);
        return true;
    }
    return false;
}

bool CBotChannel::write_atomic(const std::string& path, const std::string& payload) {
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            m_error = "cannot write " + temporary;
            return false;
        }
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.put('\n');
        output.flush();
        if (!output) {
            m_error = "cannot flush " + temporary;
            return false;
        }
    }
    std::error_code code;
    fs::rename(temporary, path, code);
    if (code) {
        m_error = "cannot rename " + temporary + ": " + code.message();
        return false;
    }
    return true;
}

void CBotChannel::ack(const SInboxMessage& message, const char* status, std::int64_t latency_ms,
                      const std::string& failure) {
    if (!m_ready) {
        return;
    }
    const char* const verdict = status != nullptr ? status : "done";
    SJson payload = SJson::make_object();
    payload.set("schema_version", SJson::make_number(1));
    payload.set("token", SJson::make_string(message.token));
    payload.set("seq", SJson::make_number(static_cast<double>(message.seq)));
    payload.set("status", SJson::make_string(verdict));
    payload.set("action", SJson::make_string(message.action.type));
    payload.set("at_ms", SJson::make_number(static_cast<double>(epoch_ms())));
    payload.set("latency_ms", SJson::make_number(static_cast<double>(latency_ms)));
    if (!failure.empty()) {
        payload.set("error", SJson::make_string(failure));
    }

    const std::string path =
        (fs::path(outbox_dir()) / (four_digits(message.seq) + ".json")).string();
    static_cast<void>(write_atomic(path, to_json(payload)));

    SJson observation = SJson::make_object();
    observation.set("kind", SJson::make_string("ack"));
    observation.set("seq", SJson::make_number(static_cast<double>(message.seq)));
    observation.set("action", SJson::make_string(message.action.type));
    observation.set("status", SJson::make_string(verdict));
    observation.set("latency_ms", SJson::make_number(static_cast<double>(latency_ms)));
    append_observation(observation);
}

void CBotChannel::observe_event(const std::string& name,
                                const std::vector<std::pair<std::string, SJson>>& fields) {
    SJson observation = SJson::make_object();
    observation.set("kind", SJson::make_string("event"));
    observation.set("event", SJson::make_string(name));
    for (const auto& field : fields) {
        observation.set(field.first, field.second);
    }
    append_observation(observation);
}

void CBotChannel::observe_event_files(const std::string& name,
                                      const std::vector<std::string>& files,
                                      const std::string& reason) {
    std::vector<std::pair<std::string, SJson>> fields;
    if (!reason.empty()) {
        fields.emplace_back("reason", SJson::make_string(reason));
    }
    SJson list = SJson::make_array();
    for (const std::string& file : files) {
        list.push(SJson::make_string(file));
    }
    fields.emplace_back("files", std::move(list));
    observe_event(name, fields);
}

void CBotChannel::observe_state(const SObservedState& state) {
    SJson observation = SJson::make_object();
    observation.set("kind", SJson::make_string("state"));
    observation.set("actor", actor_object(state.actor));

    SJson inventory = SJson::make_object();
    inventory.set("items", item_list(state.inventory));
    inventory.set("money_uah", SJson::make_number(static_cast<double>(state.money_uah)));
    observation.set("inventory", std::move(inventory));

    SJson players = SJson::make_array();
    for (const SActorState& player : state.players) {
        players.push(actor_object(player));
    }
    observation.set("players", std::move(players));
    observation.set("world_items", item_list(state.world_items));

    SJson session = SJson::make_object();
    SJson names = SJson::make_array();
    for (const std::string& name : state.session_players) {
        names.push(SJson::make_string(name));
    }
    session.set("players", std::move(names));
    session.set("slots", SJson::make_number(4));
    observation.set("session", std::move(session));
    observation.set("paused", SJson::make_bool(state.paused));

    append_observation(observation);
}

void CBotChannel::append_observation(const SJson& payload) {
    if (m_log == nullptr) {
        return;
    }
    // `at_ms` is the shared epoch-ms timeline of the bot contract; the members
    // are ordered by key, so the observation is deterministic apart from time.
    SJson document = SJson::make_object();
    document.set("at_ms", SJson::make_number(static_cast<double>(epoch_ms())));
    for (const auto& member : payload.members) {
        document.members[member.first] = member.second;
    }
    const std::string line = to_json(document);
    static_cast<void>(std::fwrite(line.data(), 1, line.size(), static_cast<std::FILE*>(m_log)));
    static_cast<void>(std::fputc('\n', static_cast<std::FILE*>(m_log)));
    static_cast<void>(std::fflush(static_cast<std::FILE*>(m_log)));
}

void CBotChannel::flush() {
    if (m_log != nullptr) {
        static_cast<void>(std::fflush(static_cast<std::FILE*>(m_log)));
    }
}

}  // namespace bot
}  // namespace coop
