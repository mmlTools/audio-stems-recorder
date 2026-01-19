#include "session.hpp"

#include "wav_postprocess.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

// os_gettime_ns
#include <util/platform.h>

#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace stems {
namespace fs = std::filesystem;

static std::string now_stamp()
{
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
#if defined(_WIN32)
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif

	char buf[64] = {};
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d_%02d-%02d-%02d",
			  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
			  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
	return buf;
}

static std::string sanitize_filename(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			out.push_back((char)c);
		else if (c == ' ')
			out.push_back('_');
		else
			out.push_back('_');
	}
	while (!out.empty() && out.back() == '_')
		out.pop_back();
	if (out.empty())
		out = "source";
	return out;
}

static void enumerate_audio_sources(std::vector<obs_source_t *> &out_sources)
{
	out_sources.clear();
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			auto *vec = static_cast<std::vector<obs_source_t *> *>(param);
			if (!src)
				return true;
			uint32_t flags = obs_source_get_output_flags(src);
			if ((flags & OBS_SOURCE_AUDIO) == 0)
				return true;
			// Acquire a strong reference for the caller.
			obs_source_t *ref = obs_source_get_ref(src);
			if (ref)
				vec->push_back(ref);
			return true;
		},
		&out_sources);
}

static uint16_t speaker_channels(enum speaker_layout speakers)
{
	switch (speakers) {
	case SPEAKERS_MONO:
		return 1;
	case SPEAKERS_STEREO:
		return 2;
	case SPEAKERS_2POINT1:
		return 3;
	case SPEAKERS_4POINT0:
		return 4;
	case SPEAKERS_4POINT1:
		return 5;
	case SPEAKERS_5POINT1:
		return 6;
	case SPEAKERS_7POINT1:
		return 8;
	default:
		return 2;
	}
}

Session::Session(SessionKind kind, const Settings &settings) : kind_(kind), settings_(settings) {}

Session::~Session()
{
	stop();
}

bool Session::start()
{
	stop();
	markers_.clear();
	start_ns_ = os_gettime_ns();

	obs_audio_info aoi{};
	if (!obs_get_audio_info(&aoi)) {
		blog(LOG_ERROR, "Audio Stems: obs_get_audio_info failed");
		return false;
	}

	sample_rate_ = aoi.samples_per_sec ? aoi.samples_per_sec : 48000;
	channels_ = speaker_channels(aoi.speakers);

	const std::string stamp = now_stamp();
	const std::string mode = (kind_ == SessionKind::Recording) ? "RECORDING" : "STREAMING";

	fs::path base = settings_.output_dir.empty() ? fs::current_path() : fs::path(settings_.output_dir);
	fs::path session_dir = base / (stamp + "_" + mode);

	std::error_code ec;
	fs::create_directories(session_dir, ec);
	if (ec) {
		blog(LOG_ERROR, "Audio Stems: failed creating output directory: %s", ec.message().c_str());
		return false;
	}
	session_dir_ = session_dir.string();
	mark_inprogress(true);

	// Record start marker and initial scene
	markers_.push_back(Marker{0, "session_start", mode});
	if (settings_.record_scene_markers) {
		obs_source_t *scene = obs_frontend_get_current_scene();
		if (scene) {
			const char *sn = obs_source_get_name(scene);
			if (sn && *sn)
				markers_.push_back(Marker{0, "scene", sn});
			obs_source_release(scene);
		}
	}

	auto is_selected = [&](const char *uuid) {
		if (!uuid || !*uuid)
			return false;
		for (const auto &u : settings_.selected_source_uuids) {
			if (u == uuid)
				return true;
		}
		return false;
	};

	std::vector<obs_source_t *> sources;
	enumerate_audio_sources(sources);

	bool any = false;
	for (obs_source_t *src : sources) {
		const char *uuid = obs_source_get_uuid(src);
		if (!is_selected(uuid)) {
			obs_source_release(src);
			continue;
		}

		const char *name = obs_source_get_name(src);
		std::string fname = sanitize_filename(name ? name : "source");
		// Apply alias naming if enabled
		if (settings_.use_source_aliases) {
			for (const auto &p : settings_.source_aliases) {
				if (p.first == (uuid ? uuid : "") && !p.second.empty()) {
					fname = sanitize_filename(p.second);
					break;
				}
			}
		}
		fs::path wavp = session_dir / (fname + ".wav");

		auto rec = std::make_unique<StemRecorder>();
		if (!rec->start(src, wavp.string(), sample_rate_, channels_)) {
			blog(LOG_ERROR, "Audio Stems: failed starting stem for %s", name ? name : "(null)");
			obs_source_release(src);
			continue;
		}

		StemOutput o;
		o.recorder = std::move(rec);
		o.wav_path = wavp.string();
		o.source_uuid = uuid ? uuid : "";
		o.source_name = name ? name : "";
		stems_.push_back(std::move(o));
		any = true;

		obs_source_release(src);
	}

	if (!any) {
		blog(LOG_WARNING, "Audio Stems: no selected audio sources to record (%s)", mode.c_str());
		stop();
		return false;
	}

	running_ = true;
	blog(LOG_INFO, "Audio Stems: session started (%s)", mode.c_str());
	return true;
}

void Session::stop()
{
	if (!running_ && stems_.empty())
		return;

	// marker
	uint64_t off = 0;
	if (start_ns_ != 0) {
		uint64_t now = os_gettime_ns();
		off = (now > start_ns_) ? (now - start_ns_) : 0;
	}
	markers_.push_back(Marker{off, "session_stop", ""});

	std::vector<StemOutput> finished;
	finished.swap(stems_);
	for (auto &o : finished) {
		if (o.recorder)
			o.recorder->stop();
		// Keep recorder metadata while it exists
		if (o.source_uuid.empty())
			o.source_uuid = o.recorder ? o.recorder->source_uuid() : "";
		if (o.source_name.empty())
			o.source_name = o.recorder ? o.recorder->source_name() : "";
		o.recorder.reset();
	}
	running_ = false;
	mark_inprogress(false);
	postprocess_stems(finished);
	if (settings_.write_sidecar_json)
		write_sidecar_json(finished);
}

void Session::on_scene_changed(const std::string &scene_name)
{
	if (!running_ || !settings_.record_scene_markers)
		return;
	uint64_t now = os_gettime_ns();
	uint64_t off = (now > start_ns_) ? (now - start_ns_) : 0;
	markers_.push_back(Marker{off, "scene", scene_name});
}

void Session::mark_inprogress(bool inprogress)
{
	if (session_dir_.empty())
		return;
	fs::path marker = fs::path(session_dir_) / ".inprogress";
	std::error_code ec;
	if (inprogress) {
		std::FILE *f = std::fopen(marker.string().c_str(), "wb");
		if (f) {
			std::fwrite("inprogress", 1, 10, f);
			std::fclose(f);
		}
	} else {
		fs::remove(marker, ec);
	}
}

void Session::postprocess_stems(const std::vector<StemOutput> &finished)
{
	for (const auto &o : finished) {
		if (o.wav_path.empty())
			continue;
		if (settings_.trim_silence)
			trim_silence_wav(o.wav_path, channels_, sample_rate_, settings_.trim_threshold_dbfs,
					 settings_.trim_lead_ms, settings_.trim_trail_ms);
		if (settings_.normalize_audio)
			normalize_wav_rms(o.wav_path, channels_, sample_rate_, settings_.normalize_target_dbfs,
					 settings_.normalize_limiter);
	}
}

void Session::write_sidecar_json(const std::vector<StemOutput> &finished) const
{
	if (session_dir_.empty())
		return;
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "session_dir", session_dir_.c_str());
	obs_data_set_string(root, "mode", kind_ == SessionKind::Recording ? "recording" : "streaming");
	obs_data_set_int(root, "sample_rate", (int64_t)sample_rate_);
	obs_data_set_int(root, "channels", (int64_t)channels_);
	obs_data_set_int(root, "start_ns", (int64_t)start_ns_);

	obs_data_t *cfg = obs_data_create();
	obs_data_set_bool(cfg, "trim_silence", settings_.trim_silence);
	obs_data_set_double(cfg, "trim_threshold_dbfs", settings_.trim_threshold_dbfs);
	obs_data_set_int(cfg, "trim_lead_ms", settings_.trim_lead_ms);
	obs_data_set_int(cfg, "trim_trail_ms", settings_.trim_trail_ms);
	obs_data_set_bool(cfg, "normalize_audio", settings_.normalize_audio);
	obs_data_set_double(cfg, "normalize_target_dbfs", settings_.normalize_target_dbfs);
	obs_data_set_bool(cfg, "normalize_limiter", settings_.normalize_limiter);
	obs_data_set_bool(cfg, "record_scene_markers", settings_.record_scene_markers);
	obs_data_set_bool(cfg, "use_source_aliases", settings_.use_source_aliases);
	obs_data_set_obj(root, "settings", cfg);
	obs_data_release(cfg);

	obs_data_array_t *stems = obs_data_array_create();
	for (const auto &o : finished) {
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "wav", o.wav_path.c_str());
		obs_data_set_string(it, "source_uuid", o.source_uuid.c_str());
		obs_data_set_string(it, "source_name", o.source_name.c_str());
		obs_data_array_push_back(stems, it);
		obs_data_release(it);
	}
	obs_data_set_array(root, "stems", stems);
	obs_data_array_release(stems);

	obs_data_array_t *marks = obs_data_array_create();
	for (const auto &m : markers_) {
		obs_data_t *it = obs_data_create();
		obs_data_set_int(it, "offset_ns", (int64_t)m.offset_ns);
		obs_data_set_string(it, "type", m.type.c_str());
		obs_data_set_string(it, "value", m.value.c_str());
		obs_data_array_push_back(marks, it);
		obs_data_release(it);
	}
	obs_data_set_array(root, "markers", marks);
	obs_data_array_release(marks);

	fs::path sidecar = fs::path(session_dir_) / "session.json";
	obs_data_save_json_safe(root, sidecar.string().c_str(), "tmp", "bak");
	obs_data_release(root);
}

} // namespace stems
