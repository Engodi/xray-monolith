--[[
	run.lua — запуск юнит-тестов coop Lua-слоя (F0-05).

	Из корня клона движка:
		luajit tests/lua/coop/run.lua

	Код возврата 0 — все тесты зелёные, 1 — есть падения.
]]

local script_path = arg and arg[0] or "tests/lua/coop/run.lua"
local test_dir = script_path:match("^(.*)/[^/]+$") or "."

package.path = test_dir .. "/?.lua;" .. package.path

local harness = require("harness")

local modules = {
	"test_codec",
	"test_events",
	"test_sync",
	"test_parity",
	"test_debug",
	"test_core",
}

for _, module_name in ipairs(modules) do
	local ok, load_error = pcall(require, module_name)
	if not ok then
		print("FAIL load " .. module_name .. ": " .. tostring(load_error))
		os.exit(1)
	end
end

local passed, total = harness.run()
os.exit(harness.report(passed, total))
