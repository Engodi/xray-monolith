-- Примитивы синхронизации: single-writer, request/response, таймауты.

local H = require("harness")

H.section("sync")

local function start(role, local_player)
	local env = H.setup({ command_line = "-coop_debug" })
	local ok = coop._set_role(role, local_player)
	H.assert_true(ok, "role set")
	return env
end

H.test("host writes state; client state applies inbound", function()
	local env = start("host", 1)

	H.assert_true(coop.sync.register("loot.chan", { writer = "host" }), "register")
	H.assert_true(coop.sync.set_state("loot.chan", "count", 3), "host write")
	H.assert_equal(coop.sync.get_state("loot.chan", "count"), 3, "cache value")
	H.assert_equal(coop.sync.snapshot("loot.chan").count, 3, "snapshot")

	local state_message = nil
	for _, message in ipairs(env.bridge.outbound) do
		if message.channel == "coop.state" then
			state_message = message
		end
	end
	H.assert_not_nil(state_message, "state broadcast")

	-- Клиентская сторона принимает state и обновляет кэш.
	H.setup({ command_line = "-coop_debug" })
	coop._set_role("client", 2)
	coop.sync.register("loot.chan", { writer = "host" })
	coop_sync.on_inbound(
		"coop.state",
		coop.events.encode({
			channel = "loot.chan",
			key = "count",
			value = 3,
		}),
		1
	)
	H.assert_equal(coop.sync.get_state("loot.chan", "count"), 3, "client cache value")
end)

H.test("client cannot write host-authoritative state (AC-2.10.2)", function()
	start("client", 2)

	coop.sync.register("loot.chan", { writer = "host" })

	local ok, reason = coop.sync.set_state("loot.chan", "count", 5)
	H.assert_false(ok, "write must be rejected")
	H.assert_equal(reason, "not_authority", "reason")
	H.assert_nil(coop.sync.get_state("loot.chan", "count"), "state unchanged")
	H.assert_equal(coop.sync.stats().rejected, 1, "rejection counter")

	ok, reason = coop.sync.publish("loot.chan", { count = 5 })
	H.assert_false(ok, "publish must be rejected")
	H.assert_equal(reason, "not_authority", "publish reason")
	H.assert_match(H.find_log("sync rejected") or "", "not_authority", "rejection logged")
end)

H.test("client can publish on an any-writer channel", function()
	local env = start("client", 2)
	coop.sync.register("input.chan", { writer = "any" })

	local ok, sequence = coop.sync.publish("input.chan", { action = "jump" })
	H.assert_true(ok, "publish allowed")
	H.assert_equal(sequence, 1, "sequence")
	H.assert_equal(env.bridge.outbound[1].channel, "input.chan", "channel")
	H.assert_equal(env.bridge.outbound[1].payload, "action=s:jump", "payload")
end)

H.test("registration validates channel and writer", function()
	start("host", 1)

	local ok, reason = coop.sync.register("", {})
	H.assert_false(ok, "empty channel")
	H.assert_equal(reason, "invalid_channel", "reason")

	ok, reason = coop.sync.register("bad.chan", { writer = "client" })
	H.assert_false(ok, "invalid writer")
	H.assert_equal(reason, "invalid_writer", "reason")

	H.assert_true(coop.sync.register("good.chan", { writer = "host" }), "valid channel")
	H.assert_true(coop.sync.register("good.chan", { writer = "host" }), "idempotent register")
	ok, reason = coop.sync.register("good.chan", { writer = "any" })
	H.assert_false(ok, "writer change rejected")
	H.assert_equal(reason, "already_registered", "reason")
end)

H.test("request/response round trip through loopback", function()
	local env = start("host", 1)
	coop.init()

	H.assert_true(coop.sync.register("loot.chan", { writer = "host" }), "register")
	H.assert_true(
		coop.sync.handle("loot.chan", function(payload, channel)
			H.assert_equal(channel, "loot.chan", "handler channel")
			local decoded = coop.events.decode(payload)
			return { accepted = true, item = decoded.item }
		end),
		"handler"
	)

	env.bridge.set_loopback(true)

	local response = nil
	local ok, request_id = coop.sync.request("loot.chan", { item = "artifact" }, function(result, error_message)
		H.assert_nil(error_message, "response error")
		response = result
	end)

	H.assert_true(ok, "request accepted")
	H.assert_equal(request_id, 1, "request id")
	H.assert_not_nil(response, "response received")
	H.assert_equal(response.accepted, true, "response payload")
	H.assert_equal(response.item, "artifact", "response echo")
	H.assert_equal(coop.sync.stats().pending, 0, "pending cleared")
end)

H.test("request without handler reports no_handler", function()
	local env = start("host", 1)
	coop.init()
	coop.sync.register("empty.chan", { writer = "host" })
	env.bridge.set_loopback(true)

	local error_message = nil
	coop.sync.request("empty.chan", { value = 1 }, function(_result, request_error)
		error_message = request_error
	end)

	H.assert_not_nil(error_message, "error delivered")
	H.assert_match(error_message, "no_handler", "reason")
end)

H.test("request times out and notifies callback", function()
	start("client", 2)
	coop.sync.register("slow.chan", { writer = "host" })

	H.now = 1000
	local timeout_error = nil
	local ok = coop.sync.request("slow.chan", { value = 1 }, function(_result, error_message)
		timeout_error = error_message
	end)
	H.assert_true(ok, "request accepted")
	H.assert_equal(coop.sync.stats().pending, 1, "pending request")

	H.now = 100000
	coop.sync.pump()

	H.assert_equal(timeout_error, "timeout", "timeout delivered")
	H.assert_equal(coop.sync.stats().timeouts, 1, "timeout counter")
	H.assert_equal(coop.sync.stats().pending, 0, "pending cleared")
end)

H.test("offline publish is rejected but local state still works", function()
	H.setup()
	H.assert_true(coop.is_offline(), "offline role")

	coop.sync.register("local.chan", { writer = "any" })

	local ok, reason = coop.sync.publish("local.chan", { value = 1 })
	H.assert_false(ok, "publish offline")
	H.assert_equal(reason, "not_connected", "reason")

	H.assert_true(coop.sync.set_state("local.chan", "value", 1), "local state works offline")
	H.assert_equal(coop.sync.get_state("local.chan", "value"), 1, "local state")
end)

H.test("registered subscribers receive channel data", function()
	local env = start("client", 2)
	coop.sync.register("input.chan", { writer = "any" })

	local received = nil
	coop.sync.on("input.chan", function(payload)
		received = payload
	end)

	env.bridge.set_loopback(false)
	coop_sync.on_inbound("input.chan", "action=s:jump", 1)

	H.assert_not_nil(received, "subscriber called")
	H.assert_equal(received.action, "jump", "payload decoded")
end)

H.test("network events reach coop.events subscribers on both sides", function()
	local env = start("client", 2)
	env.bridge.set_loopback(false)

	local moves = 0
	coop.events.on(coop.events.NAME.PLAYER_MOVE, function(payload)
		moves = moves + 1
		H.assert_equal(payload.x, 12.5, "remote position")
	end)

	coop_sync.on_inbound(coop.events.NAME.PLAYER_MOVE, "x=n:12.5", 7)
	H.assert_equal(moves, 1, "remote event dispatched to the event bus")
end)
