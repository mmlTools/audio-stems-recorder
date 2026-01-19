/*
  Audio Stems Recorder

  Records selected OBS audio sources individually when Recording/Streaming starts,
  writing per-source WAV stems and converting them to MP3 on stop.

  This is implemented as a frontend plugin (Tools menu).
*/

#include <obs-module.h>

#include "plugin-support.h"

#include "stems/stem_plugin.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static stems::StemPlugin *g_plugin = nullptr;

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "%s loaded (version %s)", PLUGIN_NAME, PLUGIN_VERSION);

	try {
		g_plugin = new stems::StemPlugin();
		g_plugin->startup();
	} catch (...) {
		obs_log(LOG_ERROR, "%s failed to initialize", PLUGIN_NAME);
		delete g_plugin;
		g_plugin = nullptr;
		return false;
	}

	return true;
}

void obs_module_unload(void)
{
	if (g_plugin) {
		g_plugin->shutdown();
		delete g_plugin;
		g_plugin = nullptr;
	}
	obs_log(LOG_INFO, "%s unloaded", PLUGIN_NAME);
}
