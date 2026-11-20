#include "stem_plugin.hpp"

#include "settings_dialog.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QDesktopServices>
#include <QDir>
#include <QUrl>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace stems {

namespace {

std::atomic<StemPlugin *> g_instance{nullptr};

constexpr const char *k_hotkey_manual = "AudioStems.ToggleCapture";
constexpr const char *k_hotkey_marker = "AudioStems.AddMarker";
constexpr const char *k_hotkeys_file = "hotkeys.json";
constexpr const char *k_dock_id = "audio-stems-recorder-dock";

std::string take_bstr(char *p)
{
	if (!p)
		return {};
	std::string s = p;
	bfree(p);
	return s;
}

// Path of the file the recording output is writing to (first segment when
// automatic file splitting is enabled).
std::string current_recording_path()
{
	std::string path;
	obs_output_t *out = obs_frontend_get_recording_output();
	if (!out)
		return path;
	if (obs_data_t *s = obs_output_get_settings(out)) {
		const char *p = obs_data_get_string(s, "path");
		if (!p || !*p)
			p = obs_data_get_string(s, "url");
		if (p && *p && !std::strstr(p, "://"))
			path = p;
		obs_data_release(s);
	}
	obs_output_release(out);
	return path;
}

template<typename F> void guarded(const char *what, F &&fn)
{
	try {
		fn();
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[Audio Stems] %s failed: %s", what, e.what());
	} catch (...) {
		blog(LOG_ERROR, "[Audio Stems] %s failed (unknown error)", what);
	}
}

std::string format_elapsed(uint64_t ns)
{
	const uint64_t s = ns / 1000000000ull;
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu", (unsigned long long)(s / 3600),
		      (unsigned long long)((s / 60) % 60), (unsigned long long)(s % 60));
	return buf;
}

} // namespace

StemPlugin *plugin_instance()
{
	return g_instance.load();
}

void set_plugin_instance(StemPlugin *p)
{
	g_instance.store(p);
}

StemPlugin::~StemPlugin()
{
	shutdown();
}

void StemPlugin::startup()
{
	std::lock_guard<std::recursive_mutex> lock(mtx_);
	settings_ = load_settings();
	finalizer_.start();

	if (!hooked_) {
		obs_frontend_add_event_callback(&StemPlugin::frontend_event_cb, this);
		obs_frontend_add_save_callback(&StemPlugin::frontend_save_cb, this);
		obs_frontend_add_tools_menu_item(obs_module_text("AudioStems.ToolsMenu"), &StemPlugin::tools_menu_cb,
						 this);
		hooked_ = true;
	}
	register_hotkeys();
	load_hotkeys();
	create_dock();
}

void StemPlugin::shutdown()
{
	{
		std::lock_guard<std::recursive_mutex> lock(mtx_);
		// Normally already done on OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN;
		// this is the safety net for an unload without a frontend exit.
		if (!exited_) {
			stop_all_sessions();
			unregister_hotkeys();
			if (hooked_) {
				obs_frontend_remove_event_callback(&StemPlugin::frontend_event_cb, this);
				obs_frontend_remove_save_callback(&StemPlugin::frontend_save_cb, this);
			}
		}
		// After a normal exit the frontend API is already torn down (OBS
		// clears its callbacks right after EXIT), so do not call into it.
		hooked_ = false;
	}
	// Outside the lock: may wait for the finalizer thread to wind down.
	finalizer_.shutdown();
}

// ---------------------------------------------------------------------------
// Callbacks from OBS (C boundary: never let an exception escape)
// ---------------------------------------------------------------------------

void StemPlugin::frontend_event_cb(enum obs_frontend_event event, void *param)
{
	auto *self = static_cast<StemPlugin *>(param);
	if (self)
		guarded("frontend event", [&] { self->on_frontend_event(event); });
}

void StemPlugin::frontend_save_cb(obs_data_t *, bool saving, void *param)
{
	auto *self = static_cast<StemPlugin *>(param);
	if (self && saving)
		guarded("saving hotkeys", [&] { self->save_hotkeys(); });
}

void StemPlugin::tools_menu_cb(void *param)
{
	auto *self = static_cast<StemPlugin *>(param);
	if (self)
		guarded("settings dialog", [&] { self->open_settings_dialog(); });
}

void StemPlugin::hotkey_manual_cb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	// Hotkeys fire on the hotkey thread; sessions live on the UI thread.
	obs_queue_task(
		OBS_TASK_UI,
		[](void *) {
			if (StemPlugin *p = plugin_instance())
				guarded("capture hotkey", [&] { p->toggle_manual(); });
		},
		nullptr, false);
}

void StemPlugin::hotkey_marker_cb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	obs_queue_task(
		OBS_TASK_UI,
		[](void *) {
			if (StemPlugin *p = plugin_instance())
				guarded("marker hotkey", [&] { p->add_user_marker(); });
		},
		nullptr, false);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

void StemPlugin::on_frontend_event(enum obs_frontend_event event)
{
	std::lock_guard<std::recursive_mutex> lock(mtx_);
	if (exited_)
		return;

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (settings_.trigger_recording)
			start_session(rec_session_, SessionKind::Recording, current_recording_path());
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		stop_session(rec_session_);
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
		if (rec_session_)
			rec_session_->pause();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		if (rec_session_)
			rec_session_->resume();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (settings_.trigger_streaming)
			start_session(stream_session_, SessionKind::Streaming);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		stop_session(stream_session_);
		break;

	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		if (!settings_.record_scene_markers)
			break;
		std::string name;
		if (obs_source_t *scene = obs_frontend_get_current_scene()) {
			const char *sn = obs_source_get_name(scene);
			name = sn ? sn : "";
			obs_source_release(scene);
		}
		if (name.empty())
			break;
		for (auto *s : {rec_session_.get(), stream_session_.get(), manual_session_.get()})
			if (s && s->is_running())
				s->add_marker("scene", name);
		break;
	}

	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		// A profile switch may reset OBS audio (sample rate / channels);
		// a manual capture must not straddle that.
		stop_session(manual_session_);
		break;

	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		std::vector<std::string> dirs = {settings_.output_dir.empty() ? default_output_dir()
									     : settings_.output_dir};
		if (settings_.save_next_to_recordings) {
			const std::string rec = take_bstr(obs_frontend_get_current_record_output_path());
			if (!rec.empty() && rec != dirs.front())
				dirs.push_back(rec);
		}
		finalizer_.enqueue([dirs](const std::atomic<bool> &cancel) {
			for (const auto &d : dirs)
				repair_interrupted_sessions(d, cancel);
		});
		break;
	}

	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		// Both arrive before OBS tears down scenes and sources: the last
		// safe moment to detach from them.
		on_exit();
		break;

	default:
		break;
	}
}

void StemPlugin::on_exit()
{
	if (exited_)
		return;
	stop_all_sessions();
	save_hotkeys();
	unregister_hotkeys();
	if (dock_)
		dock_->detach();
	exited_ = true;
}

void StemPlugin::start_session(std::unique_ptr<Session> &slot, SessionKind kind, const std::string &recording_path)
{
	stop_session(slot);
	auto s = std::make_unique<Session>(kind, settings_, finalizer_);
	if (s->start(recording_path))
		slot = std::move(s);
}

void StemPlugin::stop_session(std::unique_ptr<Session> &slot)
{
	if (!slot)
		return;
	slot->stop();
	slot.reset();
}

void StemPlugin::stop_all_sessions()
{
	stop_session(rec_session_);
	stop_session(stream_session_);
	stop_session(manual_session_);
}

// ---------------------------------------------------------------------------
// User actions (UI thread)
// ---------------------------------------------------------------------------

void StemPlugin::toggle_manual()
{
	std::lock_guard<std::recursive_mutex> lock(mtx_);
	if (exited_)
		return;
	if (manual_session_)
		stop_session(manual_session_);
	else
		start_session(manual_session_, SessionKind::Manual);
}

void StemPlugin::add_user_marker()
{
	std::lock_guard<std::recursive_mutex> lock(mtx_);
	int n = 0;
	for (auto *s : {rec_session_.get(), stream_session_.get(), manual_session_.get()}) {
		if (s && s->is_running()) {
			s->add_marker("marker", "Marker");
			n++;
		}
	}
	if (n > 0)
		blog(LOG_INFO, "[Audio Stems] marker added");
}

void StemPlugin::open_settings_dialog()
{
	Settings current;
	bool active = false;
	{
		std::lock_guard<std::recursive_mutex> lock(mtx_);
		current = settings_;
		active = rec_session_ || stream_session_ || manual_session_;
	}

	auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	SettingsDialog dlg(parent);
	dlg.set_settings(current);
	dlg.set_sessions_active(active);
	// Do not hold the lock across exec(): it runs a nested event loop in
	// which OBS keeps delivering frontend events.
	if (dlg.exec() != QDialog::Accepted)
		return;

	std::lock_guard<std::recursive_mutex> lock(mtx_);
	settings_ = dlg.get_settings();
	save_settings(settings_);
	blog(LOG_INFO, "[Audio Stems] settings saved (%zu source(s), track mask 0x%02x, %s %d-bit)",
	     settings_.selected_sources.size(), settings_.track_mask, settings_.output_format.c_str(),
	     settings_.wav_bit_depth);
}

void StemPlugin::open_output_folder()
{
	std::string dir;
	{
		std::lock_guard<std::recursive_mutex> lock(mtx_);
		for (auto *s : {rec_session_.get(), stream_session_.get(), manual_session_.get()}) {
			if (s && s->is_running()) {
				dir = s->status().session_dir;
				break;
			}
		}
		if (dir.empty())
			dir = settings_.output_dir.empty() ? default_output_dir() : settings_.output_dir;
	}
	const QString qdir = QString::fromUtf8(dir.c_str());
	QDir().mkpath(qdir);
	QDesktopServices::openUrl(QUrl::fromLocalFile(qdir));
}

DockModel StemPlugin::dock_model() const
{
	std::lock_guard<std::recursive_mutex> lock(mtx_);
	DockModel m;
	auto add = [&](const Session *s, const char *title) {
		if (!s || !s->is_running())
			return;
		const SessionStatus st = s->status();
		DockRow r;
		r.title = title;
		r.detail = format_elapsed(st.elapsed_ns) + "  " + std::to_string(st.stems) + " stem(s)";
		if (st.paused)
			r.detail += "  [paused]";
		if (st.dropped_frames > 0) {
			r.detail += "  dropped " + std::to_string(st.dropped_frames) + " frames";
			r.warn = true;
		}
		if (st.failed_stems > 0) {
			r.detail += "  " + std::to_string(st.failed_stems) + " write error(s)";
			r.warn = true;
		}
		m.rows.push_back(std::move(r));
		m.any_running = true;
	};
	add(rec_session_.get(), "Recording");
	add(stream_session_.get(), "Streaming");
	add(manual_session_.get(), "Manual");
	m.manual_running = manual_session_ != nullptr;
	m.finalizing = finalizer_.pending();
	return m;
}

// ---------------------------------------------------------------------------
// Hotkeys and dock
// ---------------------------------------------------------------------------

void StemPlugin::register_hotkeys()
{
	if (hk_manual_ == OBS_INVALID_HOTKEY_ID)
		hk_manual_ = obs_hotkey_register_frontend(k_hotkey_manual, obs_module_text("AudioStems.Hotkey.Toggle"),
							  &StemPlugin::hotkey_manual_cb, this);
	if (hk_marker_ == OBS_INVALID_HOTKEY_ID)
		hk_marker_ = obs_hotkey_register_frontend(k_hotkey_marker, obs_module_text("AudioStems.Hotkey.Marker"),
							  &StemPlugin::hotkey_marker_cb, this);
}

void StemPlugin::unregister_hotkeys()
{
	if (hk_manual_ != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_manual_);
	if (hk_marker_ != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(hk_marker_);
	hk_manual_ = hk_marker_ = OBS_INVALID_HOTKEY_ID;
}

void StemPlugin::load_hotkeys()
{
	char *path = obs_module_config_path(k_hotkeys_file);
	if (!path)
		return;
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!data)
		return;
	auto load = [&](obs_hotkey_id id, const char *key) {
		if (id == OBS_INVALID_HOTKEY_ID)
			return;
		obs_data_array_t *arr = obs_data_get_array(data, key);
		if (arr) {
			obs_hotkey_load(id, arr);
			obs_data_array_release(arr);
		}
	};
	load(hk_manual_, k_hotkey_manual);
	load(hk_marker_, k_hotkey_marker);
	obs_data_release(data);
}

void StemPlugin::save_hotkeys()
{
	if (hk_manual_ == OBS_INVALID_HOTKEY_ID && hk_marker_ == OBS_INVALID_HOTKEY_ID)
		return;
	char *path = obs_module_config_path(k_hotkeys_file);
	if (!path)
		return;
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	obs_data_t *data = obs_data_create();
	auto save = [&](obs_hotkey_id id, const char *key) {
		if (id == OBS_INVALID_HOTKEY_ID)
			return;
		obs_data_array_t *arr = obs_hotkey_save(id);
		obs_data_set_array(data, key, arr);
		obs_data_array_release(arr);
	};
	save(hk_manual_, k_hotkey_manual);
	save(hk_marker_, k_hotkey_marker);
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
}

void StemPlugin::create_dock()
{
	if (dock_)
		return;
	auto *dock = new StemsDock();
	StemsDock::Callbacks cb;
	cb.model = [this] { return dock_model(); };
	cb.toggle_manual = [this] { guarded("capture toggle", [&] { toggle_manual(); }); };
	cb.add_marker = [this] { guarded("marker", [&] { add_user_marker(); }); };
	cb.open_settings = [this] { guarded("settings dialog", [&] { open_settings_dialog(); }); };
	cb.open_folder = [this] { guarded("open folder", [&] { open_output_folder(); }); };
	dock->set_callbacks(std::move(cb));

	if (!obs_frontend_add_dock_by_id(k_dock_id, obs_module_text("AudioStems.DockTitle"), dock)) {
		blog(LOG_WARNING, "[Audio Stems] could not add status dock");
		delete dock;
		return;
	}
	dock_ = dock; // owned by the OBS main window from here on
}

} // namespace stems
