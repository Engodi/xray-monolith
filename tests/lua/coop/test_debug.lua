-- Debug-поверхность: закрыта по умолчанию, работает только с -coop_debug.

local H = require("harness")

H.section("debug")

H.test("role changes are gated outside debug mode", function()
	H.setup()

	local ok, reason = coop._set_role("host", 1)
	H.assert_false(ok, "role change rejected")
	H.assert_equal(reason, "not_allowed", "reason")
	H.assert_true(coop.is_offline(), "role unchanged")
	H.assert_match(H.find_log("role changes belong to the session manager") or "", "set_role", "logged")
end)

H.test("debug surface is disabled by default", function()
	H.setup()

	H.assert_false(coop.debug.enabled(), "disabled by default")

	local ok, reason = coop.debug.set_alife(false)
	H.assert_false(ok, "set_alife rejected")
	H.assert_equal(reason, "debug_disabled", "reason")

	ok, reason = coop.debug.god_mode(true)
	H.assert_false(ok, "god mode rejected")
	H.assert_equal(reason, "debug_disabled", "reason")

	ok, reason = coop.debug.teleport(1, 2, 3)
	H.assert_false(ok, "teleport rejected")
	H.assert_equal(reason, "debug_disabled", "reason")

	ok, reason = coop.debug.spawn("stalker", 1, 2, 3)
	H.assert_false(ok, "spawn rejected")
	H.assert_equal(reason, "debug_disabled", "reason")

	H.assert_match(H.find_log("debug surface is disabled") or "", "rejected", "rejection logged")
end)

H.test("cannot enable without the launcher switch", function()
	H.setup({ command_line = "+sv_net 1" })

	local ok, reason = coop.debug.enable(true)
	H.assert_false(ok, "enable rejected")
	H.assert_equal(reason, "debug_not_allowed", "reason")
	H.assert_false(coop.debug.enabled(), "still disabled")
end)

H.test("switch must be a whole token", function()
	H.setup({ command_line = "-coop_debug_trace 1" })

	H.assert_false(coop.launcher_allows_debug(), "substring is not a switch")
	local ok, reason = coop.debug.enable(true)
	H.assert_false(ok, "enable rejected")
	H.assert_equal(reason, "debug_not_allowed", "reason")
end)

H.test("launcher switch enables A-Life toggle and god mode", function()
	local env = H.setup({ command_line = "-coop_debug" })

	H.assert_true(coop.launcher_allows_debug(), "switch detected")
	H.assert_true(coop.debug.enable(true), "enable")
	H.assert_true(coop.debug.enabled(), "enabled")

	H.assert_true(coop.debug.set_alife(false), "disable A-Life")
	H.assert_false(env.bridge.alife_enabled(), "A-Life frozen")
	H.assert_false(coop.debug.status().alife_enabled, "status reflects freeze")

	H.assert_true(coop.debug.set_alife(true), "enable A-Life")
	H.assert_true(env.bridge.alife_enabled(), "A-Life running")

	H.assert_true(coop.debug.god_mode(true), "god mode on")
	H.assert_equal(env.actor.invulnerable_state, true, "actor invulnerable")
	H.assert_true(coop.debug.god_mode(false), "god mode off")
	H.assert_equal(env.actor.invulnerable_state, false, "actor mortal again")
end)

H.test("teleport moves the actor", function()
	local env = H.setup({ command_line = "-coop_debug" })
	coop.debug.enable(true)

	H.assert_true(coop.debug.teleport(10, 20, 30), "teleport")
	H.assert_equal(env.actor.x, 10, "x")
	H.assert_equal(env.actor.y, 20, "y")
	H.assert_equal(env.actor.z, 30, "z")
	H.assert_equal(env.actor.teleport_count, 1, "teleport calls")

	local ok, reason = coop.debug.teleport("10", 20, 30)
	H.assert_false(ok, "invalid position")
	H.assert_equal(reason, "invalid_position", "reason")
end)

H.test("teleport to an object uses its position", function()
	local env = H.setup({ command_line = "-coop_debug" })
	coop.debug.enable(true)

	env.level.objects[42] = {
		position = function()
			return { x = 7, y = 8, z = 9 }
		end,
	}

	H.assert_true(coop.debug.teleport_to_object(42), "teleport to object")
	H.assert_equal(env.actor.x, 7, "x")

	local ok, reason = coop.debug.teleport_to_object(404)
	H.assert_false(ok, "unknown object")
	H.assert_equal(reason, "object_not_found", "reason")
end)

H.test("forced spawn creates the section at the requested position", function()
	local env = H.setup({ command_line = "-coop_debug" })
	coop.debug.enable(true)

	local ok, object_id = coop.debug.spawn("stalker_soldier", 3, 4, 5)
	H.assert_true(ok, "spawn")
	H.assert_equal(object_id, 777, "object id")
	H.assert_equal(H.spawned.section, "stalker_soldier", "section")
	H.assert_equal(H.spawned.position.x, 3, "x")
	H.assert_equal(H.spawned.level_vertex_id, 11, "level vertex")

	local reason
	ok, reason = coop.debug.spawn("", 1, 2, 3)
	H.assert_false(ok, "empty section")
	H.assert_equal(reason, "invalid_section", "reason")
end)

H.test("command dispatcher and status", function()
	H.setup({ command_line = "-coop_debug" })

	local ok, reason = coop.debug.command("unknown")
	H.assert_false(ok, "unknown command")
	H.assert_equal(reason, "unknown_command", "reason")

	ok, reason = coop.debug.command("alife", false)
	H.assert_false(ok, "alife rejected while disabled")
	H.assert_equal(reason, "debug_disabled", "reason")

	coop.debug.enable(true)
	H.assert_true(coop.debug.command("alife", false), "alife via dispatcher")

	local status = coop.debug.command("status")
	H.assert_true(status.enabled, "status enabled")
	H.assert_false(status.alife_enabled, "status alife frozen")
	H.assert_true(status.debug_allowed, "status launcher switch")
end)

H.test("reset disables the surface and restores defaults", function()
	local env = H.setup({ command_line = "-coop_debug" })
	coop.debug.enable(true)
	coop.debug.set_alife(false)
	coop.debug.god_mode(true)

	H.assert_true(coop.debug.reset(), "reset")
	H.assert_false(coop.debug.enabled(), "disabled after reset")
	H.assert_true(env.bridge.alife_enabled(), "A-Life running")
	H.assert_false(env.bridge.god_mode(), "god mode off")
	H.assert_equal(env.actor.invulnerable_state, false, "actor mortal")
end)

H.test("without the engine bridge debug commands fail closed", function()
	H.setup({ no_bridge = true })

	H.assert_false(coop.debug.enabled(), "no bridge, disabled")
	local ok, reason = coop.debug.enable(true)
	H.assert_false(ok, "cannot enable")
	H.assert_equal(reason, "no_bridge", "reason")

	ok, reason = coop.debug.teleport(0, 0, 0)
	H.assert_false(ok, "teleport rejected")
	H.assert_equal(reason, "no_bridge", "reason")
end)
