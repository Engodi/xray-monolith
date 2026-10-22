#include "stdafx.h"
#pragma hdrstop

#include "SoundRender_CoreA.h"
#include "../xrCore/headless.h"

XRSOUND_API xr_token* snd_devices_token = NULL;
XRSOUND_API xr_string snd_device_name;

void CSound_manager_interface::_create(int stage)
{
	if (stage == 0)
	{
		SoundRenderA = xr_new<CSoundRender_CoreA>();
		SoundRender = SoundRenderA;
		Sound = SoundRender;

		// Headless profiles never open an audio device: the sound manager stays
		// a valid no-op object so gameplay sound calls are safe (F0-04).
		if (strstr(Core.Params, "-nosound") || Headless::Active())
		{
			SoundRender->bPresent = FALSE;
			return;
		}
		else
			SoundRender->bPresent = TRUE;
	}

	if (!SoundRender->bPresent) return;
	Sound->_initialize(stage);
}

void CSound_manager_interface::_destroy()
{
	Sound->_clear();
	xr_delete(SoundRender);
	Sound = 0;
}
