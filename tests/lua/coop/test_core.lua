--[[
	test_core.lua — движковая проводка ядра на Lua-стороне (F0-07 wiring).

	Проверяется только то, что живёт в Lua: разбор канонического payload ядра,
	перевод событий среды в поток coop.events, сырая подписка coop.sync.on_raw и
	offline-поведение без движковой таблицы `coop_runtime`.

	`coop_runtime` (C++) подменяется моком: тест не требует движка.
]]

local H = require("harness")

local function fresh(options)
	options = options or {}
	local env = H.setup(options)
	if options.runtime ~= nil then
		coop_runtime = options.runtime
	else
		coop_runtime = nil
	end
	return env
end

local function make_runtime(options)
	options = options or {}
	local runtime = {
		started = false,
		start_calls = 0,
		commands = {},
		status_value = {
			active = options.active ~= false,
			host = options.host ~= false,
			role = options.role or "host",
			instance = options.instance or "host",
			run_dir = "/tmp/run",
			log_path = "/tmp/run/host.jsonl",
			error = "",
			steps = 7,
			world_tick = 3,
			peers = 4,
			events = 12,
		},
	}
	function runtime.available()
		return runtime.started
	end
	function runtime.start()
		runtime.started = true
		runtime.start_calls = runtime.start_calls + 1
		return true
	end
	function runtime.status()
		return runtime.status_value
	end
	function runtime.command(name, argument)
		runtime.commands[#runtime.commands + 1] = { name = name, argument = argument }
		if name == "debug.emission" then
			return { ok = true, detail = "emission triggered" }
		end
		return { ok = false, detail = "unknown debug command" }
	end
	return runtime
end

local function collect()
	local seen = {}
	local function handler(payload, event_name)
		seen[#seen + 1] = { event = event_name, payload = payload }
	end
	for _, name in ipairs({
		coop_core.EVENT.WEATHER,
		coop_core.EVENT.EMISSION_START,
		coop_core.EVENT.EMISSION_END,
		coop_core.EVENT.TIME_SYNC,
	}) do
		coop_events.on(name, handler)
	end
	return seen
end

-- --- разбор payload ---------------------------------------------------------

H.section("core/parse")

H.test("канонический payload разбирается в плоскую таблицу", function()
	fresh()

	local values = coop_core.parse("event=weather;at=123;type=fog;phase=peak;intensity=70;step=3")

	H.assert_not_nil(values, "payload parsed")
	H.assert_equal(values.event, "weather", "event")
	H.assert_equal(values.at, "123", "at")
	H.assert_equal(values.type, "fog", "type")
	H.assert_equal(values.step, "3", "step")
end)

H.test("%XX-экранирование снимается", function()
	fresh()

	local values = coop_core.parse("key=a%3Bb%3Dc%25d")

	H.assert_not_nil(values, "payload parsed")
	H.assert_equal(values.key, "a;b=c%d", "escaped value")
end)

H.test("битый payload не превращается в событие", function()
	fresh()

	H.assert_nil(coop_core.parse(""), "empty")
	H.assert_nil(coop_core.parse(nil), "nil")
	H.assert_nil(coop_core.parse("no-separator"), "no '='")
	H.assert_nil(coop_core.parse("=orphan"), "empty key")
end)

-- --- события среды -> coop.events -------------------------------------------

H.section("core/events")

H.test("погода публикуется как weather", function()
	fresh()
	local seen = collect()

	H.assert_true(coop_core.on_environment("event=weather;at=100;next=200;type=fog;phase=peak;intensity=70;step=2"), "handled")

	H.assert_equal(#seen, 1, "one event")
	H.assert_equal(seen[1].event, "weather", "event name")
	H.assert_equal(seen[1].payload.weather_type, "fog", "weather type")
	H.assert_equal(seen[1].payload.at, 100, "at is a number")
end)

H.test("выброс: warn/start -> emission_start, end -> emission_end, peak -> ничего", function()
	fresh()
	local seen = collect()

	H.assert_true(coop_core.on_environment("event=emission;transition=warn;phase=warning;cycle=1;at=10;end=20"), "warn")
	H.assert_equal(#seen, 1, "warn is a start")
	H.assert_equal(seen[1].event, "emission_start", "start event")

	H.assert_false(coop_core.on_environment("event=emission;transition=peak;phase=peak;cycle=1;at=15;end=20"), "peak")
	H.assert_equal(#seen, 1, "peak is not a start event")

	H.assert_true(coop_core.on_environment("event=emission;transition=end;phase=idle;cycle=1;at=20;end=0"), "end")
	H.assert_equal(#seen, 2, "end event appended")
	H.assert_equal(seen[2].event, "emission_end", "end event name")
	H.assert_equal(seen[2].payload.cycle, 1, "cycle")
end)

H.test("скачок часов публикуется как time_sync", function()
	fresh()
	local seen = collect()

	H.assert_true(coop_core.on_environment("event=clock;at=3600000;day=1;hour=9;minute=15;second=0"), "handled")

	H.assert_equal(#seen, 1, "one event")
	H.assert_equal(seen[1].event, "time_sync", "event name")
	H.assert_equal(seen[1].payload.hour, 9, "hour")
end)

H.test("неизвестное событие и битый payload не публикуются", function()
	fresh()
	local seen = collect()

	H.assert_false(coop_core.on_environment("event=unknown;at=1"), "unknown kind")
	H.assert_false(coop_core.on_environment("broken"), "malformed")
	H.assert_equal(#seen, 0, "no events")
end)

-- --- фасад над coop_runtime -------------------------------------------------

H.section("core/runtime")

H.test("без движковой таблицы слой работает offline", function()
	fresh()

	H.assert_false(coop_core.available(), "not available")
	H.assert_false(coop_core.start(), "start refused")
	local status = coop_core.status()
	H.assert_false(status.available, "status available")
	H.assert_equal(status.role, "offline", "offline role")
	local result = coop_core.command("debug.weather", "type=fog")
	H.assert_false(result.ok, "command refused")
	H.assert_equal(result.detail, "runtime_unavailable", "reason")
end)

H.test("status/command проксируются в движок", function()
	local runtime = make_runtime({ role = "host", instance = "host" })
	fresh({ runtime = runtime })

	H.assert_true(coop_core.start(), "started")
	H.assert_equal(runtime.start_calls, 1, "start forwarded")

	local status = coop_core.status()
	H.assert_true(status.active, "active")
	H.assert_equal(status.instance, "host", "instance")
	H.assert_equal(status.log_path, "/tmp/run/host.jsonl", "log path")

	local result = coop_core.command("debug.emission", "start")
	H.assert_true(result.ok, "command ok")
	H.assert_equal(runtime.commands[1].name, "debug.emission", "name forwarded")
	H.assert_equal(runtime.commands[1].argument, "start", "argument forwarded")

	local unknown = coop_core.command("debug.nope", "")
	H.assert_false(unknown.ok, "unknown rejected")
end)

H.test("on_game_start идемпотентен и подписывает канал ядра", function()
	local runtime = make_runtime({})
	local env = fresh({ runtime = runtime })

	local raw = {}
	local original_on_raw = coop.sync.on_raw
	coop.sync.on_raw = function(channel, callback)
		raw[#raw + 1] = { channel = channel, callback = callback }
		return original_on_raw(channel, callback)
	end

	H.assert_true(coop_core.on_game_start(), "game start")
	H.assert_true(coop_core.on_game_start(), "second call is a no-op")

	H.assert_equal(#raw, 1, "subscribed exactly once")
	H.assert_equal(raw[1].channel, "environment", "core channel")
	H.assert_equal(runtime.start_calls, 1, "runtime started once")
	H.assert_equal(env.bridge.role(), "offline", "role still offline (mock)")
	coop.sync.on_raw = original_on_raw
end)

H.test("сырая подписка доставляет payload канала environment как есть", function()
	local runtime = make_runtime({})
	fresh({ runtime = runtime })
	local seen = collect()
	local received = nil
	coop.sync.on_raw("environment", function(payload)
		received = payload
		return coop_core.on_environment(payload)
	end)

	-- Движок отдаёт сырой payload ядра через coop_bridge (dispatch -> on_inbound).
	H.assert_true(coop.sync.on_inbound("environment", "event=weather;at=5;type=rain;phase=peak", 1), "dispatched")
	H.assert_equal(received, "event=weather;at=5;type=rain;phase=peak", "raw payload untouched")
	H.assert_equal(#seen, 1, "event reached coop.events")
	H.assert_equal(seen[1].event, "weather", "event name")
end)

H.test("describe отражает состояние проводки", function()
	local runtime = make_runtime({ instance = "client-1", role = "client" })
	fresh({ runtime = runtime })

	H.assert_true(coop_core.on_game_start(), "game start")
	local described = coop_core.describe()
	H.assert_equal(described.version, 1, "version")
	H.assert_true(described.started, "started")
	H.assert_true(described.subscribed, "subscribed")
	H.assert_true(described.available, "available")
	H.assert_equal(described.role, "client", "role")
end)
