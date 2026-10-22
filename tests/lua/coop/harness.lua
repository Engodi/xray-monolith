--[[
	harness.lua — минимальный тест-раннер для coop-скриптов (F0-05).

	Запуск (из корня клона движка):
		luajit tests/lua/coop/run.lua

	Движок не нужен: движковые глобалы (db, level, alife, vector, time_global,
	RegisterScriptCallback, coop_bridge) подменяются моками. Скрипты
	загружаются штатно через loadfile — тем же Lua 5.1, что и в движке.
]]

local H = {}

local repo_root = (arg and arg[0] or ""):match("^(.-)/tests/") or "."
local script_dir = repo_root .. "/gamedata/scripts/"

H.script_order = {
	"coop.script",
	"coop_debug.script",
	"coop_events.script",
	"coop_parity.script",
	"coop_sync.script",
}

local suite_name = "?"
local tests = {}
local failures = {}
local assertions = 0

function H.section(name)
	suite_name = name
end

function H.test(name, body)
	tests[#tests + 1] = { suite = suite_name, name = name, body = body }
end

local function record_failure(message)
	failures[#failures + 1] = string.format("%s / %s: %s", suite_name, tests[#tests].name, message)
end

function H.fail(message)
	record_failure(message)
end

function H.assert_true(value, message)
	assertions = assertions + 1
	if not value then
		error(message or "expected true", 2)
	end
end

function H.assert_false(value, message)
	assertions = assertions + 1
	if value then
		error(message or "expected false", 2)
	end
end

function H.assert_equal(actual, expected, message)
	assertions = assertions + 1
	if actual ~= expected then
		error(
			string.format("%s: expected %s, got %s", message or "assert_equal", tostring(expected), tostring(actual)),
			2
		)
	end
end

function H.assert_nil(value, message)
	assertions = assertions + 1
	if value ~= nil then
		error(string.format("%s: expected nil, got %s", message or "assert_nil", tostring(value)), 2)
	end
end

function H.assert_not_nil(value, message)
	assertions = assertions + 1
	if value == nil then
		error((message or "assert_not_nil") .. ": got nil", 2)
	end
end

function H.assert_match(text, pattern, message)
	assertions = assertions + 1
	if type(text) ~= "string" or not text:match(pattern) then
		error(string.format("%s: %q does not match %q", message or "assert_match", tostring(text), pattern), 2)
	end
end

function H.run()
	local passed = 0
	for _, test in ipairs(tests) do
		local ok, error_message = pcall(test.body)
		if ok then
			passed = passed + 1
		else
			failures[#failures + 1] = string.format("%s / %s: %s", test.suite, test.name, tostring(error_message))
		end
	end
	return passed, #tests
end

function H.report(passed, total)
	print(string.format("coop lua tests: %d/%d passed, %d assertions", passed, total, assertions))
	for _, message in ipairs(failures) do
		print("FAIL " .. message)
	end
	if #failures == 0 then
		print("OK")
		return 0
	end
	return 1
end

-- --- движковые моки --------------------------------------------------------

H.logs = {}
H.now = 0
H.callbacks = {}
H.bridge_events = {}

local function make_vector()
	local vector_value = { x = 0, y = 0, z = 0 }
	function vector_value:set(x, y, z)
		self.x, self.y, self.z = x or 0, y or 0, z or 0
		return self
	end
	return vector_value
end

local function make_actor()
	local actor = {
		x = 0,
		y = 0,
		z = 0,
		health = 100,
		invulnerable_state = false,
		teleport_count = 0,
	}
	function actor:position()
		return { x = self.x, y = self.y, z = self.z }
	end
	function actor:set_actor_position(position, _skip_collision, _keep_speed)
		self.x, self.y, self.z = position.x, position.y, position.z
		self.teleport_count = self.teleport_count + 1
	end
	function actor:invulnerable(value)
		self.invulnerable_state = value
	end
	function actor:level_vertex_id()
		return 11
	end
	function actor:game_vertex_id()
		return 22
	end
	function actor:id()
		return 1
	end
	function actor:section()
		return "actor"
	end
	return actor
end

local function make_level(options)
	local level_mock = {
		command_line_value = options.command_line or "",
		days = 1,
		hours = 12,
		minutes = 30,
		weather = "clear",
		objects = {},
	}
	level_mock.command_line = function()
		return level_mock.command_line_value
	end
	level_mock.get_time_days = function()
		return level_mock.days
	end
	level_mock.get_time_hours = function()
		return level_mock.hours
	end
	level_mock.get_time_minutes = function()
		return level_mock.minutes
	end
	level_mock.get_weather = function()
		return level_mock.weather
	end
	level_mock.object_by_id = function(object_id)
		return level_mock.objects[object_id]
	end
	return level_mock
end

-- Мок C++ coop_bridge: те же имена/семантика, что в src/xrGame/coop.
function H.make_bridge()
	local bridge = {
		role_value = "offline",
		local_player_value = 0,
		peer_count_value = 0,
		event_seq = 0,
		sync_seq = 0,
		loopback = false,
		debug = false,
		alife = true,
		god = false,
		last_error_value = "",
		inbox = {},
		dispatch = nil,
		counters = {
			events_out = 0,
			events_in = 0,
			sync_out = 0,
			sync_in = 0,
			sync_rejected = 0,
			inbound_dropped = 0,
		},
		outbound = {},
	}

	local function deliver(message)
		bridge.counters.events_in = bridge.counters.events_in + 1
		if bridge.dispatch ~= nil then
			bridge.dispatch(message)
			return
		end
		bridge.inbox[#bridge.inbox + 1] = message
	end

	local function route(message)
		bridge.outbound[#bridge.outbound + 1] = message
		if bridge.loopback then
			deliver(message)
		end
	end

	function bridge.session()
		return {
			role = bridge.role_value,
			role_id = 0,
			local_player = bridge.local_player_value,
			peer_count = bridge.peer_count_value,
			max_slots = 4,
			api_version = 1,
			loopback = bridge.loopback,
			inbound = #bridge.inbox,
		}
	end

	function bridge.role()
		return bridge.role_value
	end

	function bridge.set_role(role, local_player)
		if role ~= "offline" and role ~= "host" and role ~= "client" then
			bridge.last_error_value = "invalid_role"
			return false
		end
		bridge.role_value = role
		bridge.local_player_value = role == "offline" and 0 or (local_player or 0)
		bridge.last_error_value = ""
		return true
	end

	function bridge.is_host()
		return bridge.role_value == "host"
	end

	function bridge.is_client()
		return bridge.role_value == "client"
	end

	function bridge.is_offline()
		return bridge.role_value == "offline"
	end

	function bridge.local_player()
		return bridge.local_player_value
	end

	function bridge.peer_count()
		return bridge.peer_count_value
	end

	function bridge.set_peer_count(count)
		bridge.peer_count_value = count
	end

	function bridge.emit_event(name, payload)
		if name == nil or name == "" then
			bridge.last_error_value = "empty_event_name"
			return 0
		end
		bridge.event_seq = bridge.event_seq + 1
		bridge.counters.events_out = bridge.counters.events_out + 1
		route({ seq = bridge.event_seq, channel = name, payload = payload or "" })
		bridge.last_error_value = ""
		return bridge.event_seq
	end

	function bridge.sync(channel, payload)
		if channel == nil or channel == "" then
			bridge.last_error_value = "empty_sync_channel"
			return 0
		end
		if bridge.role_value == "offline" then
			bridge.counters.sync_rejected = bridge.counters.sync_rejected + 1
			bridge.last_error_value = "not_connected"
			return 0
		end
		bridge.sync_seq = bridge.sync_seq + 1
		bridge.counters.sync_out = bridge.counters.sync_out + 1
		route({ seq = bridge.sync_seq, channel = channel, payload = payload or "" })
		bridge.last_error_value = ""
		return bridge.sync_seq
	end

	function bridge.pump()
		return 0
	end

	function bridge.inbound(max_count)
		local result = {}
		local count = math.min(max_count or #bridge.inbox, #bridge.inbox)
		for index = 1, count do
			result[index] = bridge.inbox[1]
			table.remove(bridge.inbox, 1)
		end
		return result
	end

	function bridge.enable_dispatch()
		bridge.dispatch = function(message)
			if type(coop_sync) == "table" and type(coop_sync.on_inbound) == "function" then
				coop_sync.on_inbound(message.channel, message.payload, message.seq)
			else
				bridge.inbox[#bridge.inbox + 1] = message
			end
		end
	end

	function bridge.set_loopback(enabled)
		bridge.loopback = enabled == true
	end

	function bridge.api_manifest()
		return {
			{ name = "emit_event", argc = 2, since = 1 },
			{ name = "session", argc = 0, since = 1 },
			{ name = "sync", argc = 2, since = 1 },
		}
	end

	function bridge.api_fingerprint()
		return "deadbeefdeadbeef"
	end

	function bridge.api_version()
		return 1
	end

	function bridge.stats()
		return bridge.counters
	end

	function bridge.last_error()
		return bridge.last_error_value
	end

	function bridge.clear()
		bridge.role_value = "offline"
		bridge.local_player_value = 0
		bridge.peer_count_value = 0
		bridge.event_seq = 0
		bridge.sync_seq = 0
		bridge.loopback = false
		bridge.debug = false
		bridge.alife = true
		bridge.god = false
		bridge.last_error_value = ""
		bridge.inbox = {}
		bridge.outbound = {}
		bridge.counters = {
			events_out = 0,
			events_in = 0,
			sync_out = 0,
			sync_in = 0,
			sync_rejected = 0,
			inbound_dropped = 0,
		}
	end

	function bridge.set_debug_enabled(enabled, launcher_allows)
		if enabled and not launcher_allows then
			bridge.last_error_value = "debug_not_allowed"
			return false
		end
		bridge.debug = enabled == true
		if not bridge.debug then
			bridge.alife = true
			bridge.god = false
		end
		bridge.last_error_value = ""
		return true
	end

	function bridge.debug_enabled()
		return bridge.debug
	end

	function bridge.set_alife_enabled(enabled)
		if not bridge.debug then
			bridge.last_error_value = "debug_disabled"
			return false
		end
		bridge.alife = enabled == true
		return true
	end

	function bridge.alife_enabled()
		return bridge.alife
	end

	function bridge.set_god_mode(enabled)
		if not bridge.debug then
			bridge.last_error_value = "debug_disabled"
			return false
		end
		bridge.god = enabled == true
		return true
	end

	function bridge.god_mode()
		return bridge.god
	end

	return bridge
end

-- Полная пересборка окружения: чистые глобалы + свежие моки + загрузка скриптов.
function H.setup(options)
	options = options or {}

	coop = nil
	coop_events = nil
	coop_sync = nil
	coop_debug = nil
	coop_parity = nil
	coop_bridge = nil

	H.logs = {}
	H.callbacks = {}
	H.now = 0
	H.bridge_events = {}

	printf = function(text)
		H.logs[#H.logs + 1] = tostring(text)
	end

	time_global = function()
		return H.now
	end

	vector = function()
		return make_vector()
	end

	local actor = make_actor()
	local level_mock = make_level(options)
	db = { actor = actor }
	level = level_mock

	alife = function()
		return {
			create = function(_self, section, position, level_vertex_id, game_vertex_id)
				H.spawned = {
					section = section,
					position = position,
					level_vertex_id = level_vertex_id,
					game_vertex_id = game_vertex_id,
				}
				return { id = 777 }
			end,
		}
	end

	RegisterScriptCallback = function(name, callback)
		H.callbacks[name] = callback
	end

	ini_file_ex = nil

	if not options.no_bridge then
		coop_bridge = options.bridge or H.make_bridge()
	end

	for _, file in ipairs(H.script_order) do
		local chunk, load_error = loadfile(script_dir .. file)
		if chunk == nil then
			error("failed to load " .. file .. ": " .. tostring(load_error))
		end
		chunk()
	end

	return {
		actor = actor,
		level = level_mock,
		bridge = coop_bridge,
	}
end

function H.fire_callback(name, ...)
	local callback = H.callbacks[name]
	if callback == nil then
		error("callback is not registered: " .. tostring(name))
	end
	return callback(...)
end

-- Эмуляция старта игры: axr_main вызывает on_game_start у каждого модуля.
function H.start()
	for _, module_name in ipairs({ "coop", "coop_debug", "coop_events", "coop_parity", "coop_sync" }) do
		local module = _G[module_name]
		if type(module) == "table" and type(module.on_game_start) == "function" then
			module.on_game_start()
		end
	end
end

function H.find_log(pattern)
	for _, message in ipairs(H.logs) do
		if message:match(pattern) then
			return message
		end
	end
	return nil
end

return H
