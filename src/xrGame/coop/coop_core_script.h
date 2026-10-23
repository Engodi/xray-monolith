////////////////////////////////////////////////////////////////////////////
//	Module 		: coop/coop_core_script.h
//	Created 	: 23.10.2026
//	Description : Lua export of the coop core runtime (F0-04/F0-07 wiring)
//
//	Exports the `coop_runtime` table. The Lua gameplay layer (gamedata/scripts/
//	coop_core.script) is the only consumer: it starts the debug surface and
//	forwards bot commands. The engine never calls Lua for state per frame
//	(NFR-ARCH-10) — only events cross the bridge.
////////////////////////////////////////////////////////////////////////////

#ifndef COOP_CORE_SCRIPT_H_INCLUDED
#define COOP_CORE_SCRIPT_H_INCLUDED

#pragma once

#include "script_export_space.h"

struct coop_core_registrator {
    DECLARE_SCRIPT_REGISTER_FUNCTION
};

#endif  // COOP_CORE_SCRIPT_H_INCLUDED
