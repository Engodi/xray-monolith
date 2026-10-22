-- Кодирование состояния для сети: детерминизм, типы, экранирование.

local H = require("harness")

H.section("codec")

H.test("encode is deterministic regardless of key order", function()
	H.setup()

	local first = coop.events.encode({ b = 2, a = 1 })
	local second = coop.events.encode({ a = 1, b = 2 })

	H.assert_not_nil(first, "encoded payload")
	H.assert_equal(first, second, "key order must not affect encoding")
	H.assert_equal(first, "a=n:1\nb=n:2", "payload layout")
end)

H.test("encode/decode round trip keeps types", function()
	H.setup()

	local payload = {
		number = 42.5,
		text = "hello world",
		flag = true,
		disabled = false,
		dangerous = "a=b\\c\nd",
	}

	local encoded = coop.events.encode(payload)
	H.assert_not_nil(encoded, "encoded payload")

	local decoded = coop.events.decode(encoded)
	H.assert_not_nil(decoded, "decoded payload")
	H.assert_equal(decoded.number, 42.5, "number")
	H.assert_equal(decoded.text, "hello world", "string")
	H.assert_equal(decoded.flag, true, "true boolean")
	H.assert_equal(decoded.disabled, false, "false boolean")
	H.assert_equal(decoded.dangerous, payload.dangerous, "escaped string")
end)

H.test("encode rejects nested tables", function()
	H.setup()

	local encoded, reason = coop.events.encode({ nested = { 1, 2 } })
	H.assert_nil(encoded, "nested table must be rejected")
	H.assert_equal(reason, "unsupported_value_type:table", "reason")
end)

H.test("decode rejects malformed payloads", function()
	H.setup()

	local decoded, reason = coop.events.decode("broken-line-without-separator")
	H.assert_nil(decoded, "malformed line must be rejected")
	H.assert_match(reason, "^malformed_line")
end)

H.test("empty payload round trips to empty table", function()
	H.setup()

	H.assert_equal(coop.events.encode(nil), "", "nil payload")
	local decoded = coop.events.decode("")
	H.assert_not_nil(decoded, "empty payload")
	H.assert_equal(next(decoded), nil, "empty table")
end)
