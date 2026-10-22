-- Паритет экспортов хост<->клиент: манифест, отпечаток, диффы.

local H = require("harness")

H.section("parity")

local function find_entry(manifest, name)
	for _, entry in ipairs(manifest) do
		if entry.name == name then
			return entry
		end
	end
	return nil
end

H.test("local manifest contains both layers", function()
	H.setup()

	local manifest = coop.parity.local_manifest()
	H.assert_true(#manifest > 0, "manifest not empty")

	local cpp_entry = find_entry(manifest, "emit_event")
	H.assert_not_nil(cpp_entry, "cpp entry")
	H.assert_equal(cpp_entry.layer, "cpp", "cpp layer")

	local lua_entry = find_entry(manifest, "events.on")
	H.assert_not_nil(lua_entry, "lua entry")
	H.assert_equal(lua_entry.layer, "lua", "lua layer")
	H.assert_equal(lua_entry.argc, 2, "lua arity")
end)

H.test("fingerprint is stable and deterministic", function()
	H.setup()

	local first = coop.parity.local_fingerprint()
	local second = coop.parity.local_fingerprint()

	H.assert_not_nil(first, "fingerprint")
	H.assert_match(first, "^%x%x%x%x%x%x%x%x$", "8 hex digits")
	H.assert_equal(first, second, "stable fingerprint")

	local manifest = coop.parity.local_manifest()
	local serialized, serialized_fingerprint = coop.parity.serialize(manifest)
	H.assert_not_nil(serialized, "serialized")
	H.assert_equal(serialized_fingerprint, first, "fingerprint of serialized manifest")
end)

H.test("local registry matches real functions", function()
	H.setup()

	local ok, problems = coop.parity.validate_local()
	H.assert_true(ok, "registry consistent: " .. table.concat(problems or {}, "; "))
end)

H.test("serialize/parse round trip", function()
	H.setup()

	local manifest = coop.parity.local_manifest()
	local serialized = coop.parity.serialize(manifest)
	local parsed = coop.parity.parse(serialized)

	H.assert_not_nil(parsed, "parsed manifest")
	H.assert_equal(#parsed, #manifest, "entry count")
	H.assert_equal(coop.parity.fingerprint(parsed), coop.parity.local_fingerprint(), "same fingerprint")
end)

H.test("parse rejects malformed manifests", function()
	H.setup()

	local parsed, reason = coop.parity.parse("not-a-manifest-line")
	H.assert_nil(parsed, "malformed input")
	H.assert_match(reason, "^malformed_line", "reason")

	parsed, reason = coop.parity.parse(42)
	H.assert_nil(parsed, "non-string input")
	H.assert_equal(reason, "payload_not_string", "reason")
end)

H.test("identical manifests verify as match", function()
	H.setup()

	local serialized = coop.parity.serialize(coop.parity.local_manifest())
	local ok, result = coop.parity.handshake(serialized)

	H.assert_true(ok, "handshake ok")
	H.assert_true(result.ok, "result ok")
	H.assert_equal(result.reason, "match", "reason")
	H.assert_equal(#result.missing, 0, "no missing")
	H.assert_equal(#result.extra, 0, "no extra")
	H.assert_equal(#result.mismatched, 0, "no mismatched")
	H.assert_equal(result.local_fp, result.remote_fp, "fingerprints match")
end)

H.test("missing entry is detected", function()
	H.setup()

	local manifest = coop.parity.local_manifest()
	-- Удаляем запись о событии на "удалённой" стороне.
	local mutated = {}
	for _, entry in ipairs(manifest) do
		if not (entry.layer == "lua" and entry.name == "events.on") then
			mutated[#mutated + 1] = entry
		end
	end

	local ok, result = coop.parity.handshake(coop.parity.serialize(mutated))
	H.assert_false(ok, "handshake must fail")
	H.assert_equal(result.reason, "mismatch", "reason")
	H.assert_equal(result.missing[1], "lua:events.on", "missing entry")
end)

H.test("extra entry is detected", function()
	H.setup()

	local manifest = coop.parity.local_manifest()
	manifest[#manifest + 1] = { name = "future.feature", argc = 1, since = 1, layer = "lua" }

	local ok, result = coop.parity.handshake(coop.parity.serialize(manifest))
	H.assert_false(ok, "handshake must fail")
	H.assert_equal(result.extra[1], "lua:future.feature", "extra entry")
end)

H.test("arity mismatch is detected", function()
	H.setup()

	local manifest = coop.parity.local_manifest()
	local mutated = {}
	for _, entry in ipairs(manifest) do
		local copy = {
			name = entry.name,
			argc = entry.argc,
			since = entry.since,
			layer = entry.layer,
		}
		if copy.layer == "lua" and copy.name == "sync.set_state" then
			copy.argc = copy.argc + 1
		end
		mutated[#mutated + 1] = copy
	end

	local ok, result = coop.parity.handshake(coop.parity.serialize(mutated))
	H.assert_false(ok, "handshake must fail")
	H.assert_equal(#result.mismatched, 1, "one mismatch")
	H.assert_match(result.mismatched[1], "sync.set_state", "mismatched entry named")
end)

H.test("handshake payload is ready to send", function()
	H.setup()

	H.assert_equal(coop.parity.CHANNEL, "coop.parity", "handshake channel")
	local payload = coop.parity.payload()
	H.assert_not_nil(payload, "payload")
	H.assert_true(payload:match("cpp\temit_event\t2\t1") ~= nil, "cpp entry in payload")

	local ok = coop.parity.handshake(payload)
	H.assert_true(ok, "own payload verifies")
end)

H.test("invalid remote manifest is reported, not crashed", function()
	H.setup()

	local ok, result = coop.parity.handshake("{garbage}")
	H.assert_false(ok, "handshake must fail")
	H.assert_match(result.reason, "remote_parse_failed", "parse error reported")
end)
