#pragma once

#include <memory>
#include <mutex>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QPointer>

#include "finalizer.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "stems_dock.hpp"

namespace stems {

// Owns all plugin state. Every session operation runs on the OBS UI thread:
// frontend events are delivered there, hotkeys are re-dispatched there with
// obs_queue_task(), and the dock / dialog live there.
class StemPlugin {
public:
	StemPlugin() = default;
	~StemPlugin();

	void startup();
	void shutdown();

	void toggle_manual();
	void add_user_marker();
	void open_settings_dialog();
	void open_output_folder();
	DockModel dock_model() const;

private:
	static void frontend_event_cb(enum obs_frontend_event event, void *param);
	static void frontend_save_cb(obs_data_t *save_data, bool saving, void *param);
	static void tools_menu_cb(void *param);
	static void hotkey_manual_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void hotkey_marker_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);

	void on_frontend_event(enum obs_frontend_event event);
	void start_session(std::unique_ptr<Session> &slot, SessionKind kind, const std::string &recording_path = {});
	void stop_session(std::unique_ptr<Session> &slot);
	void stop_all_sessions();
	void on_exit();

	void register_hotkeys();
	void load_hotkeys();
	void save_hotkeys();
	void unregister_hotkeys();
	void create_dock();

	mutable std::recursive_mutex mtx_;
	Settings settings_;
	Finalizer finalizer_;
	std::unique_ptr<Session> rec_session_;
	std::unique_ptr<Session> stream_session_;
	std::unique_ptr<Session> manual_session_;

	obs_hotkey_id hk_manual_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_marker_ = OBS_INVALID_HOTKEY_ID;
	QPointer<StemsDock> dock_;
	bool hooked_ = false;
	bool exited_ = false;
};

// Global accessor used by deferred UI tasks (null after unload).
StemPlugin *plugin_instance();
void set_plugin_instance(StemPlugin *p);

} // namespace stems
