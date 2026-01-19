#pragma once

#include <mutex>
#include <memory>

#include <obs-frontend-api.h>

#include "settings.hpp"
#include "session.hpp"

namespace stems {

class StemPlugin {
public:
	StemPlugin() = default;
	~StemPlugin();

	void startup();
	void shutdown();

private:
	static void frontend_event_cb(enum obs_frontend_event event, void *param);
	static void tools_menu_cb(void *param);

	void on_frontend_event(enum obs_frontend_event event);
	void open_settings_dialog();

	std::mutex mtx_;
	Settings settings_;
	std::unique_ptr<Session> rec_session_;
	std::unique_ptr<Session> stream_session_;
	bool hooked_ = false;
};

} // namespace stems
