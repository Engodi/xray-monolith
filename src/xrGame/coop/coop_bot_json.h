////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_bot_json.h
//	Created 	: 23.10.2026
//	Description : Minimal JSON for the bot action channel (F0-12 wiring)
//
//	The bot channel (`<run-dir>/<instance>/bot/**`) is a file protocol of small
//	JSON documents (see tools/bots/README.md in the coop repository). The
//	engine has no JSON library, so this header carries the smallest possible
//	reader/writer for that protocol:
//
//	  * parse: null / true / false / number / string / array / object,
//	    objects as std::map (stable key order, deterministic output);
//	  * write: compact JSON, UTF-8 passes through, control characters escaped;
//	  * no exceptions, no locale-dependent formatting, no engine dependency, so
//	    the unit test in tests/cpp/coop can compile the same sources with g++.
//
//	This is deliberately not a general-purpose JSON library: it exists to make
//	the bot protocol readable in C++ and to keep the write path deterministic.
////////////////////////////////////////////////////////////////////////////

#ifndef COOP_BOT_JSON_H_INCLUDED
#define COOP_BOT_JSON_H_INCLUDED

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace coop {
namespace bot {

class SJson {
public:
    enum class EType : std::uint8_t {
        Null = 0,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    EType type = EType::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<SJson> items;
    std::map<std::string, SJson> members;

    SJson() = default;

    static SJson make_object();
    static SJson make_array();
    static SJson make_string(std::string value);
    static SJson make_number(double value);
    static SJson make_bool(bool value);

    // Object member access. Never throws; a miss returns nullptr.
    const SJson* find(const std::string& key) const;
    void set(const std::string& key, SJson value);
    void push(SJson value);

    // Typed reads. False means "absent or of another type" — a caller treats
    // that as a protocol error, never as a default value.
    bool as_string(std::string& out) const;
    bool as_number(double& out) const;
    bool as_int(std::int64_t& out) const;
    bool as_bool(bool& out) const;
};

// Parses one JSON document. On failure `out` is untouched and `error` explains
// the position. Trailing whitespace is allowed, trailing garbage is not.
bool parse_json(std::string_view text, SJson& out, std::string& error);

// Appends the compact form of `value` to `out`.
void write_json(std::string& out, const SJson& value);

// Escapes a string for a JSON document (quotes included).
void write_json_string(std::string& out, std::string_view value);

}  // namespace bot
}  // namespace coop

#endif  // COOP_BOT_JSON_H_INCLUDED
