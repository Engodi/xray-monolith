--[[
	e2e_loopback.lua — сквозная проверка Lua-моста «как пользователь» (F0-05).

	Движок локально не собирается (MSVC/CI), поэтому end-to-end проверяется
	на реальных coop-скриптах и реальном протоколе сообщений: хост и клиент
	разворачиваются как два независимых окружения, сообщения передаются
	между ними вручную — ровно так, как их передаст транспорт F0-02.

	Сценарий:
	  1. хост: роль host, канал состояния, запись состояния, событие игрока;
	  2. паритет экспортов: клиент сверяет манифест хоста (handshake);
	  3. клиент: применяет состояние и событие, проверяет кэш/подписку;
	  4. клиент: попытка записи авторитетного состояния отклоняется;
	  5. результат печатается, код возврата 0/1.

	Запуск: luajit tests/lua/coop/e2e_loopback.lua
]]

local script_path = arg and arg[0] or "tests/lua/coop/e2e_loopback.lua"
local test_dir = script_path:match("^(.*)/[^/]+$") or "."

package.path = test_dir .. "/?.lua;" .. package.path

local H = require("harness")

local checks = 0
local failures = {}

local function check(condition, message)
	checks = checks + 1
	if not condition then
		failures[#failures + 1] = message
	end
end

-- --- 1. ХОСТ ---------------------------------------------------------------
local host = H.setup({ command_line = "-coop_debug" })
H.start()
check(coop._set_role("host", 1), "host role")
check(coop.sync.register("world.time", { writer = "host" }), "host channel registered")
check(coop.sync.set_state("world.time", "hours", 13), "host writes world time")

local host_events = 0
coop.events.on(coop.events.NAME.PLAYER_DEATH, function(payload)
	host_events = host_events + 1
	check(payload.killer_id == 900, "host sees killer id")
end)

H.fire_callback("actor_on_before_death", nil, {
	id = function()
		return 900
	end,
	section = function()
		return "stalker_bandit"
	end,
	position = function()
		return { x = 1, y = 2, z = 3 }
	end,
	level_vertex_id = function()
		return 4
	end,
	game_vertex_id = function()
		return 5
	end,
})

check(host_events == 1, "host receives its own event locally")

local death_message = nil
local state_message = nil
for _, message in ipairs(host.bridge.outbound) do
	if message.channel == "player_death" then
		death_message = message
	elseif message.channel == "coop.state" then
		state_message = message
	end
end
check(death_message ~= nil, "host emits player_death outbound")
check(state_message ~= nil, "host emits coop.state outbound")

local host_manifest = coop.parity.serialize(coop.parity.local_manifest())
local host_fingerprint = coop.parity.local_fingerprint()

-- --- 2. КЛИЕНТ -------------------------------------------------------------
local client = H.setup({ command_line = "-coop_debug" })
H.start()
check(coop._set_role("client", 2), "client role")

-- Паритет сборок на подключении (AC-1.10).
local parity_ok, parity_result = coop.parity.handshake(host_manifest)
check(parity_ok, "export parity verified on connect")
check(parity_result.local_fp == host_fingerprint, "local fingerprint equals host")
check(parity_result.remote_fp == host_fingerprint, "remote fingerprint equals host")

check(coop.sync.register("world.time", { writer = "host" }), "client channel registered")

local client_deaths = 0
coop.events.on(coop.events.NAME.PLAYER_DEATH, function(payload)
	client_deaths = client_deaths + 1
	check(payload.killer_id == 900, "client sees killer id")
end)

-- Транспорт доставил два сообщения: состояние и событие.
check(
	coop_sync.on_inbound(state_message.channel, state_message.payload, state_message.seq),
	"client applies state message"
)
check(
	coop_sync.on_inbound(death_message.channel, death_message.payload, death_message.seq),
	"client applies event message"
)

check(coop.sync.get_state("world.time", "hours") == 13, "client sees host world time")
check(client_deaths == 1, "client receives replicated event")

-- --- 4. КЛИЕНТ НЕ ПИШЕТ АВТОРИТЕТНОЕ СОСТОЯНИЕ -----------------------------
local write_ok, write_reason = coop.sync.set_state("world.time", "hours", 3)
check(write_ok == false, "client write rejected")
check(write_reason == "not_authority", "client write reason")
check(coop.sync.get_state("world.time", "hours") == 13, "client state unchanged after rejection")
check(client.bridge.stats().sync_rejected == 0, "rejection handled in Lua, not sent to transport")

-- --- 5. ИТОГ ---------------------------------------------------------------
print(string.format("e2e loopback: %d checks, %d failures", checks, #failures))
print("host fingerprint:   " .. tostring(host_fingerprint))
print("client fingerprint: " .. tostring(parity_result.local_fp))
for _, message in ipairs(failures) do
	print("FAIL " .. message)
end

if #failures == 0 then
	print("OK")
	os.exit(0)
end
os.exit(1)
