#include <obs-module.h>

#include "plugin-support.h"

#include "stems/stem_plugin.hpp"

#include <exception>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("AudioStems.Description");
}

static stems::StemPlugin *g_plugin = nullptr;

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "loaded (version %s, OBS %s)", PLUGIN_VERSION, obs_get_version_string());

	try {
		g_plugin = new stems::StemPlugin();
		g_plugin->startup();
		stems::set_plugin_instance(g_plugin);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "failed to initialize: %s", e.what());
	} catch (...) {
		obs_log(LOG_ERROR, "failed to initialize");
	}
	if (!stems::plugin_instance()) {
		delete g_plugin;
		g_plugin = nullptr;
		return false;
	}
	return true;
}

void obs_module_unload(void)
{
	stems::set_plugin_instance(nullptr);
	if (g_plugin) {
		try {
			g_plugin->shutdown();
		} catch (...) {
		}
		delete g_plugin;
		g_plugin = nullptr;
	}
	obs_log(LOG_INFO, "unloaded");
}
