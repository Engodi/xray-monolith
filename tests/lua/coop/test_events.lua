-- События игрока/мира: подписки, throttle, дедупликация, движковые колбэки.

local H = require("harness")

H.section("events")

local function counter()
	local state = { value = 0 }
	local function increment()
		state.value = state.value + 1
	end
	return state, increment
end

H.test("on/off subscribe and unsubscribe", function()
	H.setup()

	local state, increment = counter()
	H.assert_true(coop.events.on("custom_event", increment), "subscribe")
	H.assert_false(coop.events.on("custom_event", increment), "duplicate subscribe")
	coop.events.dispatch("custom_event", {})
	H.assert_equal(state.value, 1, "handler called")

	H.assert_true(coop.events.off("custom_event", increment), "unsubscribe")
	coop.events.dispatch("custom_event", {})
	H.assert_equal(state.value, 1, "handler must not be called after off")
end)

H.test("emit goes local and outbound with sequence", function()
	local env = H.setup()

	local received = nil
	coop.events.on(coop.events.NAME.PLAYER_HEALTH, function(payload)
		received = payload
	end)

	local ok, sequence = coop.events.emit(coop.events.NAME.PLAYER_HEALTH, { health = 77 })
	H.assert_true(ok, "emit ok")
	H.assert_equal(sequence, 1, "first outbound sequence")
	H.assert_equal(received.health, 77, "local dispatch")

	H.assert_equal(#env.bridge.outbound, 1, "outbound queue")
	H.assert_equal(env.bridge.outbound[1].channel, "player_health", "outbound channel")
	H.assert_equal(env.bridge.outbound[1].payload, "health=n:77", "outbound payload")
end)

H.test("emit rejects unknown/untyped payload", function()
	H.setup()

	local ok, reason = coop.events.emit("", {})
	H.assert_false(ok, "empty event name rejected")
	H.assert_equal(reason, "invalid_event_name", "reason")

	ok, reason = coop.events.emit("custom", { nested = {} })
	H.assert_false(ok, "nested payload rejected")
	H.assert_equal(reason, "unsupported_value_type:table", "reason")
end)

H.test("pulse emits each value once and throttles between ticks", function()
	local env = H.setup()

	local move, on_move = counter()
	local health, on_health = counter()
	local world, on_world = counter()
	local weather, on_weather = counter()
	coop.events.on(coop.events.NAME.PLAYER_MOVE, on_move)
	coop.events.on(coop.events.NAME.PLAYER_HEALTH, on_health)
	coop.events.on(coop.events.NAME.WORLD_TIME, on_world)
	coop.events.on(coop.events.NAME.WEATHER, on_weather)

	H.now = 1000
	local emitted = coop.events.pulse(false)
	H.assert_equal(emitted, 4, "first pulse emits all four signals")
	H.assert_equal(move.value, 1, "move")

	-- Внутри интервала (250 мс) повторный вызов не опрашивает состояние.
	H.now = 1100
	H.assert_equal(coop.events.pulse(false), 0, "throttled pulse emits nothing")

	-- Значения не изменились — событий нет даже после интервала.
	H.now = 1400
	H.assert_equal(coop.events.pulse(false), 0, "unchanged values emit nothing")

	-- Изменились позиция/HP/время/погода — ровно по одному событию.
	env.actor.x = 10
	env.actor.health = 55
	env.level.hours = 13
	env.level.weather = "rain"
	H.now = 2000
	H.assert_equal(coop.events.pulse(false), 4, "changed values emit")
	H.assert_equal(move.value, 2, "move once")
	H.assert_equal(health.value, 2, "health once")
	H.assert_equal(world.value, 2, "world time once")
	H.assert_equal(weather.value, 2, "weather once")
end)

H.test("movement below threshold is suppressed", function()
	local env = H.setup()

	local move_state, on_move = counter()
	coop.events.on(coop.events.NAME.PLAYER_MOVE, on_move)

	H.now = 1000
	coop.events.pulse(true)
	H.assert_equal(move_state.value, 1, "initial position")

	env.actor.x = 0.05
	coop.events.pulse(true)
	H.assert_equal(move_state.value, 1, "jitter below threshold")

	env.actor.x = 1.0
	coop.events.pulse(true)
	H.assert_equal(move_state.value, 2, "real movement")
end)

H.test("engine callbacks feed the event stream", function()
	local env = H.setup()
	coop.events.on_game_start()

	H.assert_not_nil(H.callbacks.actor_on_update, "actor_on_update registered")
	H.assert_not_nil(H.callbacks.actor_on_item_take, "item take registered")
	H.assert_not_nil(H.callbacks.npc_on_death_callback, "npc death registered")

	local spawns, on_spawn = counter()
	local pickups, on_pickup = counter()
	local inventory, on_inventory = counter()
	local deaths, on_death = counter()
	local npc_deaths, on_npc_death = counter()
	coop.events.on(coop.events.NAME.ACTOR_SPAWN, on_spawn)
	coop.events.on(coop.events.NAME.ITEM_PICKUP, on_pickup)
	coop.events.on(coop.events.NAME.PLAYER_INVENTORY, on_inventory)
	coop.events.on(coop.events.NAME.PLAYER_DEATH, on_death)
	coop.events.on(coop.events.NAME.NPC_DEATH, on_npc_death)

	H.fire_callback("actor_on_first_update", nil)
	H.assert_equal(spawns.value, 1, "actor spawn")

	local item = {
		id = function()
			return 501
		end,
		section = function()
			return "wpn_pm"
		end,
		position = function()
			return { x = 1, y = 2, z = 3 }
		end,
		level_vertex_id = function()
			return 5
		end,
		game_vertex_id = function()
			return 6
		end,
	}
	H.fire_callback("actor_on_item_take", item)
	H.assert_equal(pickups.value, 1, "item pickup")
	H.assert_equal(inventory.value, 1, "inventory take")

	local killer = {
		id = function()
			return 900
		end,
		section = function()
			return "stalker_bandit"
		end,
		position = function()
			return { x = 0, y = 0, z = 0 }
		end,
		level_vertex_id = function()
			return 7
		end,
		game_vertex_id = function()
			return 8
		end,
	}

	H.fire_callback("actor_on_before_death", nil, killer)
	H.assert_equal(deaths.value, 1, "player death")

	H.fire_callback("npc_on_death_callback", item, killer)
	H.assert_equal(npc_deaths.value, 1, "npc death")

	-- Найдём payload последнего события NPC в outbound-очереди.
	local last_npc = nil
	for _, message in ipairs(env.bridge.outbound) do
		if message.channel == "npc_death" then
			last_npc = message
		end
	end
	H.assert_not_nil(last_npc, "npc_death outbound")
	H.assert_match(last_npc.payload, "id=n:501", "npc id in payload")
	H.assert_match(last_npc.payload, "killer_id=n:900", "killer in payload")
end)

H.test("no bridge: events stay local and do not crash", function()
	local env = H.setup({ no_bridge = true })

	H.assert_nil(env.bridge, "bridge disabled")
	H.assert_false(coop.is_available(), "availability")

	local health, health_fn = counter()
	coop.events.on(coop.events.NAME.PLAYER_HEALTH, health_fn)

	H.now = 1000
	coop.events.pulse(true)
	H.assert_equal(health.value, 1, "local dispatch without bridge")
end)
