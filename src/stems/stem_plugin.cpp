#include "stem_plugin.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QWidget>

#include "settings_dialog.hpp"

#include "wav_writer.hpp"

#include <filesystem>

namespace stems {

static void repair_inprogress_sessions(const Settings &settings)
{
	namespace fs = std::filesystem;
	fs::path base = settings.output_dir.empty() ? fs::current_path() : fs::path(settings.output_dir);
	std::error_code ec;
	if (!fs::exists(base, ec))
		return;
	for (auto &entry : fs::directory_iterator(base, ec)) {
		if (ec)
			break;
		if (!entry.is_directory())
			continue;
		fs::path marker = entry.path() / ".inprogress";
		if (!fs::exists(marker, ec))
			continue;
		// Repair all wav files in this folder and remove marker.
		for (auto &f : fs::directory_iterator(entry.path(), ec)) {
			if (ec)
				break;
			if (!f.is_regular_file())
				continue;
			if (f.path().extension() == ".wav") {
				WavWriter::repair_header(f.path().string());
			}
		}
		ec.clear();
		fs::remove(marker, ec);
		blog(LOG_WARNING, "Audio Stems: repaired in-progress session: %s", entry.path().string().c_str());
	}
}

StemPlugin::~StemPlugin()
{
	shutdown();
}

void StemPlugin::startup()
{
	std::lock_guard<std::mutex> lock(mtx_);
	settings_ = load_settings();
	repair_inprogress_sessions(settings_);

	if (!hooked_) {
		obs_frontend_add_event_callback(&StemPlugin::frontend_event_cb, this);
		obs_frontend_add_tools_menu_item("Audio Stems Recorder...", &StemPlugin::tools_menu_cb, this);
		hooked_ = true;
	}
}

void StemPlugin::shutdown()
{
	std::lock_guard<std::mutex> lock(mtx_);
	// Stop sessions if active
	if (rec_session_) {
		rec_session_->stop();
		rec_session_.reset();
	}
	if (stream_session_) {
		stream_session_->stop();
		stream_session_.reset();
	}
	// OBS does not provide a public API to remove the tools menu item; keeping the callback registered is ok.
}

void StemPlugin::frontend_event_cb(enum obs_frontend_event event, void *param)
{
	auto *self = static_cast<StemPlugin *>(param);
	if (self)
		self->on_frontend_event(event);
}

void StemPlugin::tools_menu_cb(void *param)
{
	auto *self = static_cast<StemPlugin *>(param);
	if (self)
		self->open_settings_dialog();
}

void StemPlugin::on_frontend_event(enum obs_frontend_event event)
{
	std::lock_guard<std::mutex> lock(mtx_);

	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (settings_.trigger_recording) {
			// Always start a fresh session folder per start event
			if (rec_session_) {
				rec_session_->stop();
				rec_session_.reset();
			}
			rec_session_ = std::make_unique<Session>(SessionKind::Recording, settings_);
			rec_session_->start();
		}
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		if (rec_session_)
			rec_session_->stop();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (settings_.trigger_streaming) {
			// Always start a fresh session folder per start event
			if (stream_session_) {
				stream_session_->stop();
				stream_session_.reset();
			}
			stream_session_ = std::make_unique<Session>(SessionKind::Streaming, settings_);
			stream_session_->start();
		}
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		if (stream_session_)
			stream_session_->stop();
		break;

#ifdef OBS_FRONTEND_EVENT_SCENE_CHANGED
	case OBS_FRONTEND_EVENT_SCENE_CHANGED: {
		obs_source_t *scene = obs_frontend_get_current_scene();
		std::string name;
		if (scene) {
			const char *sn = obs_source_get_name(scene);
			if (sn)
				name = sn;
			obs_source_release(scene);
		}
		if (!name.empty()) {
			if (rec_session_ && rec_session_->is_running())
				rec_session_->on_scene_changed(name);
			if (stream_session_ && stream_session_->is_running())
				stream_session_->on_scene_changed(name);
		}
		break;
	}
#endif
	default:
		break;
	}
}

static QWidget *obs_main_window_qt()
{
	// Frontend API provides the main window as a QWidget pointer on Qt builds.
	return reinterpret_cast<QWidget *>(obs_frontend_get_main_window());
}

void StemPlugin::open_settings_dialog()
{
	// UI must run on the Qt main thread; obs_frontend_get_main_window() is only valid when Qt is available.
	QWidget *parent = obs_main_window_qt();
	SettingsDialog dlg(parent);
	dlg.set_settings(settings_);
	if (dlg.exec() == QDialog::Accepted) {
		settings_ = dlg.get_settings();
		save_settings(settings_);
		blog(LOG_INFO, "Audio Stems: settings saved");
	}
}

} // namespace stems
