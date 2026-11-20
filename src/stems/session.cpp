#include "session.hpp"

#include "paths.hpp"
#include "transcode.hpp"
#include "wav_postprocess.hpp"

#include "plugin-support.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace stems {
namespace fs = std::filesystem;

namespace {

constexpr const char *k_inprogress_marker = ".inprogress";
constexpr uint64_t k_header_refresh_ns = 5000000000ull; // 5 s

std::string local_stamp(bool iso)
{
	std::time_t t = std::time(nullptr);
	std::tm tmv{};
#if defined(_WIN32)
	localtime_s(&tmv, &t);
#else
	localtime_r(&t, &tmv);
#endif
	char buf[64] = {};
	if (iso)
		std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
	else
		std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tmv);
	return buf;
}

// Keeps Unicode (UTF-8) intact and only strips what file systems reject.
std::string sanitize_filename(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s) {
		if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
		    c == '?' || c == '*')
			out.push_back('_');
		else
			out.push_back((char)c);
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
		out.pop_back();
	while (!out.empty() && out.front() == ' ')
		out.erase(out.begin());
	if (out.size() > 120)
		out.resize(120); // may cut a UTF-8 sequence; fix below
	while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80)
		out.pop_back();
	if (!out.empty() && ((unsigned char)out.back() & 0x80))
		out.pop_back();
	if (out.empty())
		out = "source";

	static const char *reserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
					 "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3"};
	std::string upper = out;
	std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)toupper(c); });
	for (const char *r : reserved)
		if (upper == r)
			return out + "_";
	return out;
}

uint16_t speaker_channels(enum speaker_layout speakers)
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

std::string take_bstr(char *p)
{
	if (!p)
		return {};
	std::string s = p;
	bfree(p);
	return s;
}

fs::path unique_dir(const fs::path &base, const std::string &name)
{
	std::error_code ec;
	fs::path p = base / u8path(name);
	for (int i = 2; fs::exists(p, ec) && i < 1000; i++)
		p = base / u8path(name + "_" + std::to_string(i));
	return p;
}

struct AudioSourceList {
	std::vector<obs_source_t *> sources; // strong refs
	~AudioSourceList()
	{
		for (auto *s : sources)
			obs_source_release(s);
	}
};

void enumerate_audio_sources(AudioSourceList &list)
{
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			auto *l = static_cast<AudioSourceList *>(param);
			if (!src || (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
				return true;
			if (obs_source_t *ref = obs_source_get_ref(src))
				l->sources.push_back(ref);
			return true;
		},
		&list);
}

// ---------------------------------------------------------------------------
// Finalisation (runs on the Finalizer thread)
// ---------------------------------------------------------------------------

struct StemReport {
	StemKind kind = StemKind::Source;
	size_t track = 0;
	std::string wav_path;
	std::string final_path;
	std::string uuid;
	std::string name;
	uint64_t frames = 0;
	uint64_t dropped = 0;
	uint64_t padded = 0;
	bool failed = false;
	bool removed = false;
	PostprocessResult pp;
	bool processed = false;
};

struct MarkerReport {
	int64_t frames = 0;
	std::string type;
	std::string value;
};

struct SessionReport {
	Settings settings;
	SessionKind kind = SessionKind::Recording;
	std::string session_dir;
	std::string recording_path;
	std::string started_at;
	uint32_t sample_rate = 48000;
	uint16_t channels = 2;
	SampleFormat sample_format = SampleFormat::S24;
	uint64_t frames = 0;
	std::vector<StemReport> stems;
	std::vector<MarkerReport> markers;
};

void write_audacity_labels(const SessionReport &r)
{
	bool any = false;
	for (const auto &m : r.markers)
		any = any || (m.type != "session_start" && m.type != "session_stop");
	if (!any)
		return;
	const fs::path p = u8path(r.session_dir) / "markers.txt";
	std::FILE *f = fopen_u8(to_u8(p), "wb");
	if (!f)
		return;
	for (const auto &m : r.markers) {
		const double t = (double)std::max<int64_t>(0, m.frames) / (double)r.sample_rate;
		std::string label = m.type == "marker" ? m.value : (m.type + (m.value.empty() ? "" : ": " + m.value));
		std::replace(label.begin(), label.end(), '\t', ' ');
		std::replace(label.begin(), label.end(), '\n', ' ');
		std::fprintf(f, "%.6f\t%.6f\t%s\n", t, t, label.c_str());
	}
	std::fclose(f);
}

void write_sidecar_json(const SessionReport &r, bool finalized)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "plugin", PLUGIN_NAME);
	obs_data_set_string(root, "plugin_version", PLUGIN_VERSION);
	obs_data_set_string(root, "mode", session_kind_label(r.kind));
	obs_data_set_string(root, "started_at", r.started_at.c_str());
	obs_data_set_string(root, "session_dir", r.session_dir.c_str());
	if (!r.recording_path.empty())
		obs_data_set_string(root, "recording_file", r.recording_path.c_str());
	obs_data_set_int(root, "sample_rate", (long long)r.sample_rate);
	obs_data_set_int(root, "channels", (long long)r.channels);
	obs_data_set_string(root, "wav_sample_format", sample_format_name(r.sample_format));
	obs_data_set_int(root, "length_frames", (long long)r.frames);
	obs_data_set_double(root, "length_seconds", (double)r.frames / (double)r.sample_rate);
	obs_data_set_bool(root, "finalized", finalized);

	obs_data_t *cfg = obs_data_create();
	const Settings &s = r.settings;
	obs_data_set_string(cfg, "output_format", s.output_format.c_str());
	obs_data_set_int(cfg, "wav_bit_depth", s.wav_bit_depth);
	obs_data_set_bool(cfg, "post_fader", s.post_fader);
	obs_data_set_bool(cfg, "trim_silence", s.trim_silence);
	obs_data_set_double(cfg, "trim_threshold_dbfs", s.trim_threshold_dbfs);
	obs_data_set_int(cfg, "trim_lead_ms", s.trim_lead_ms);
	obs_data_set_int(cfg, "trim_trail_ms", s.trim_trail_ms);
	obs_data_set_bool(cfg, "normalize_audio", s.normalize_audio);
	obs_data_set_double(cfg, "normalize_target_dbfs", s.normalize_target_dbfs);
	obs_data_set_bool(cfg, "normalize_limiter", s.normalize_limiter);
	obs_data_set_obj(root, "settings", cfg);
	obs_data_release(cfg);

	obs_data_array_t *stems = obs_data_array_create();
	for (const auto &st : r.stems) {
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "type", st.kind == StemKind::Track ? "track" : "source");
		if (st.kind == StemKind::Track)
			obs_data_set_int(it, "track", (long long)st.track + 1);
		obs_data_set_string(it, "name", st.name.c_str());
		obs_data_set_string(it, "source_uuid", st.uuid.c_str());
		obs_data_set_string(it, "file", to_u8(u8path(st.final_path).filename()).c_str());
		obs_data_set_string(it, "path", st.final_path.c_str());
		obs_data_set_int(it, "frames", (long long)st.frames);
		obs_data_set_int(it, "dropped_frames", (long long)st.dropped);
		obs_data_set_int(it, "silence_filled_frames", (long long)st.padded);
		obs_data_set_bool(it, "write_error", st.failed);
		obs_data_set_bool(it, "source_removed_during_session", st.removed);
		if (st.processed) {
			obs_data_set_int(it, "trimmed_start_frames", (long long)st.pp.trimmed_start_frames);
			obs_data_set_double(it, "trimmed_start_seconds",
					    (double)st.pp.trimmed_start_frames / (double)r.sample_rate);
			obs_data_set_double(it, "gain_db", st.pp.gain_db);
			obs_data_set_double(it, "peak_dbfs", st.pp.peak_dbfs);
			obs_data_set_double(it, "rms_dbfs", st.pp.rms_dbfs);
			obs_data_set_bool(it, "silent", st.pp.silent);
		}
		obs_data_array_push_back(stems, it);
		obs_data_release(it);
	}
	obs_data_set_array(root, "stems", stems);
	obs_data_array_release(stems);

	obs_data_array_t *marks = obs_data_array_create();
	for (const auto &m : r.markers) {
		obs_data_t *it = obs_data_create();
		obs_data_set_string(it, "type", m.type.c_str());
		obs_data_set_string(it, "value", m.value.c_str());
		obs_data_set_int(it, "frame", (long long)m.frames);
		obs_data_set_double(it, "seconds", (double)m.frames / (double)r.sample_rate);
		obs_data_array_push_back(marks, it);
		obs_data_release(it);
	}
	obs_data_set_array(root, "markers", marks);
	obs_data_array_release(marks);

	const fs::path sidecar = u8path(r.session_dir) / "session.json";
	obs_data_save_json_pretty_safe(root, to_u8(sidecar).c_str(), "tmp", "bak");
	obs_data_release(root);
}

void finalize_session(SessionReport &r, const std::atomic<bool> &cancel)
{
	const Settings &s = r.settings;
	const OutputFormat out_fmt = output_format_from_string(s.output_format);
	const bool want_pp = s.trim_silence || s.normalize_audio;

	std::string ffmpeg;
	if (out_fmt != OutputFormat::Wav) {
		ffmpeg = find_ffmpeg(s.ffmpeg_path);
		if (ffmpeg.empty())
			blog(LOG_WARNING,
			     "[Audio Stems] %s export requested but ffmpeg was not found (set its path in "
			     "Tools > Audio Stems Recorder). Keeping WAV files.",
			     output_format_to_string(out_fmt));
	}

	PostprocessOptions opt;
	opt.trim = s.trim_silence;
	opt.trim_threshold_dbfs = s.trim_threshold_dbfs;
	opt.trim_lead_ms = s.trim_lead_ms;
	opt.trim_trail_ms = s.trim_trail_ms;
	opt.normalize = s.normalize_audio;
	opt.normalize_target_dbfs = s.normalize_target_dbfs;
	opt.normalize_limiter = s.normalize_limiter;

	for (auto &st : r.stems) {
		st.final_path = st.wav_path;
		if (cancel.load())
			break;

		if (want_pp && st.frames > 0) {
			if (postprocess_wav(st.wav_path, opt, cancel, st.pp))
				st.processed = true;
			else if (!cancel.load())
				blog(LOG_WARNING, "[Audio Stems] post-processing failed for %s", st.wav_path.c_str());
		}

		if (!ffmpeg.empty() && !cancel.load()) {
			fs::path out = u8path(st.wav_path);
			out.replace_extension(output_format_extension(out_fmt));
			if (export_audio(ffmpeg, st.wav_path, to_u8(out), out_fmt, s.mp3_bitrate_kbps, cancel)) {
				st.final_path = to_u8(out);
				if (!s.keep_wav_after_export) {
					std::error_code ec;
					fs::remove(u8path(st.wav_path), ec);
				}
			}
		}
	}

	const bool finalized = !cancel.load();
	if (s.write_sidecar_json)
		write_sidecar_json(r, finalized);
	write_audacity_labels(r);

	std::error_code ec;
	fs::remove(u8path(r.session_dir) / k_inprogress_marker, ec);

	blog(LOG_INFO, "[Audio Stems] session %s: %zu stem(s), %.1f s%s -> %s", finalized ? "finalized" : "saved (raw)",
	     r.stems.size(), (double)r.frames / (double)r.sample_rate, finalized ? "" : " [post-processing skipped]",
	     r.session_dir.c_str());
}

} // namespace

const char *session_kind_label(SessionKind k)
{
	switch (k) {
	case SessionKind::Recording:
		return "recording";
	case SessionKind::Streaming:
		return "streaming";
	default:
		return "manual";
	}
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Session::Session(SessionKind kind, const Settings &settings, Finalizer &finalizer)
	: kind_(kind),
	  settings_(settings),
	  finalizer_(finalizer)
{
}

Session::~Session()
{
	stop();
}

std::string Session::resolve_base_dir(const std::string &recording_path) const
{
	if (settings_.save_next_to_recordings) {
		if (!recording_path.empty()) {
			const fs::path parent = u8path(recording_path).parent_path();
			if (!parent.empty())
				return to_u8(parent);
		}
		const std::string rec_dir = take_bstr(obs_frontend_get_current_record_output_path());
		if (!rec_dir.empty())
			return rec_dir;
	}
	return settings_.output_dir.empty() ? default_output_dir() : settings_.output_dir;
}

void Session::write_inprogress_marker()
{
	const fs::path p = u8path(session_dir_) / k_inprogress_marker;
	if (std::FILE *f = fopen_u8(to_u8(p), "wb")) {
		std::fputs("Audio Stems Recorder session in progress\n", f);
		std::fclose(f);
	}
}

bool Session::start(const std::string &recording_path)
{
	if (running_)
		return true;

	obs_audio_info aoi{};
	if (!obs_get_audio_info(&aoi)) {
		blog(LOG_ERROR, "[Audio Stems] obs_get_audio_info failed");
		return false;
	}
	fmt_.sample_rate = aoi.samples_per_sec ? aoi.samples_per_sec : 48000;
	fmt_.channels = speaker_channels(aoi.speakers);
	fmt_.sample_format = sample_format_from_bits(settings_.wav_bit_depth);

	// ---- resolve which stems to record before touching the disk ----
	struct Planned {
		obs_source_t *source = nullptr; // borrowed from `list`
		size_t track = 0;
		bool is_track = false;
		std::string file_base;
	};
	std::vector<Planned> plan;
	AudioSourceList list;
	enumerate_audio_sources(list);

	std::set<obs_source_t *> used;
	for (const auto &sel : settings_.selected_sources) {
		obs_source_t *match = nullptr;
		if (!sel.uuid.empty()) {
			for (auto *src : list.sources) {
				const char *u = obs_source_get_uuid(src);
				if (u && sel.uuid == u) {
					match = src;
					break;
				}
			}
		}
		if (!match && !sel.name.empty()) {
			for (auto *src : list.sources) {
				const char *n = obs_source_get_name(src);
				if (n && sel.name == n) {
					match = src;
					break;
				}
			}
		}
		if (!match || used.count(match))
			continue;
		used.insert(match);

		const char *name = obs_source_get_name(match);
		const char *uuid = obs_source_get_uuid(match);
		std::string base = name ? name : "source";
		if (settings_.use_source_aliases) {
			for (const auto &p : settings_.source_aliases) {
				if (uuid && p.first == uuid && !p.second.empty()) {
					base = p.second;
					break;
				}
			}
		}
		Planned pl;
		pl.source = match;
		pl.file_base = sanitize_filename(base);
		plan.push_back(std::move(pl));
	}
	for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (settings_.track_mask & (1u << i)) {
			Planned pl;
			pl.is_track = true;
			pl.track = i;
			pl.file_base = "Track_" + std::to_string(i + 1);
			plan.push_back(std::move(pl));
		}
	}

	if (plan.empty()) {
		blog(LOG_WARNING,
		     "[Audio Stems] no audio sources or tracks selected (or none found in this scene collection); "
		     "%s session not started",
		     session_kind_label(kind_));
		return false;
	}

	// ---- session folder ----
	recording_path_ = recording_path;
	const fs::path base = u8path(resolve_base_dir(recording_path));
	std::string folder;
	if (!recording_path.empty() && settings_.save_next_to_recordings)
		folder = to_u8(u8path(recording_path).stem()) + "_stems";
	else {
		std::string mode = session_kind_label(kind_);
		std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return (char)toupper(c); });
		folder = local_stamp(false) + "_" + mode;
	}

	std::error_code ec;
	fs::create_directories(base, ec);
	const fs::path dir = unique_dir(base, folder);
	fs::create_directories(dir, ec);
	if (ec || !fs::is_directory(dir, ec)) {
		blog(LOG_ERROR, "[Audio Stems] cannot create output folder '%s': %s", to_u8(dir).c_str(),
		     ec.message().c_str());
		return false;
	}
	session_dir_ = to_u8(dir);
	write_inprogress_marker();

	start_ns_ = os_gettime_ns();
	started_at_ = local_stamp(true);
	timeline_.start(start_ns_, fmt_.sample_rate);
	{
		std::lock_guard<std::mutex> lock(markers_mtx_);
		markers_.clear();
		markers_.push_back(Marker{0, start_ns_, "session_start", session_kind_label(kind_)});
	}
	if (settings_.record_scene_markers) {
		if (obs_source_t *scene = obs_frontend_get_current_scene()) {
			const char *sn = obs_source_get_name(scene);
			if (sn && *sn)
				add_marker("scene", sn);
			obs_source_release(scene);
		}
	}

	// ---- start capture ----
	std::set<std::string> used_names;
	for (auto &pl : plan) {
		std::string fname = pl.file_base;
		for (int i = 2; used_names.count(fname); i++)
			fname = pl.file_base + "_" + std::to_string(i);
		used_names.insert(fname);
		const std::string wav = to_u8(dir / u8path(fname + ".wav"));

		auto rec = std::make_unique<StemRecorder>();
		const bool ok = pl.is_track ? rec->start_track(pl.track, wav, fmt_)
					    : rec->start_source(pl.source, settings_.post_fader, wav, fmt_);
		if (!ok) {
			blog(LOG_ERROR, "[Audio Stems] failed to start stem '%s'", fname.c_str());
			continue;
		}
		recs_.push_back(std::move(rec));
	}

	if (recs_.empty()) {
		fs::remove_all(dir, ec);
		session_dir_.clear();
		return false;
	}

	writer_stop_.store(false);
	writer_ = std::thread(&Session::writer_main, this);
	running_ = true;

	blog(LOG_INFO, "[Audio Stems] %s session started: %zu stem(s), %u Hz, %u ch, %s -> %s", session_kind_label(kind_),
	     recs_.size(), fmt_.sample_rate, fmt_.channels, sample_format_name(fmt_.sample_format),
	     session_dir_.c_str());
	return true;
}

void Session::writer_main()
{
	os_set_thread_name("audio-stems-writer");
	try {
		const int64_t rate = fmt_.sample_rate;
		const size_t max_per_pass = (size_t)rate; // fairness between stems
		std::vector<uint64_t> last_dropped(recs_.size(), 0);
		uint64_t last_refresh = os_gettime_ns();

		auto now_frames = [&](uint64_t now) {
			return (int64_t)(timeline_.active_ns(now) / 1000) * rate / 1000000;
		};

		while (!writer_stop_.load(std::memory_order_acquire)) {
			const uint64_t now = os_gettime_ns();
			const int64_t nf = now_frames(now);
			size_t n = 0;
			for (auto &r : recs_)
				n += r->drain(timeline_, nf, max_per_pass);

			if (now - last_refresh >= k_header_refresh_ns) {
				last_refresh = now;
				for (size_t i = 0; i < recs_.size(); i++) {
					recs_[i]->refresh_header();
					const uint64_t d = recs_[i]->dropped_frames();
					if (d != last_dropped[i]) {
						blog(LOG_WARNING,
						     "[Audio Stems] '%s': disk too slow, %llu frames dropped so far",
						     recs_[i]->source_name().c_str(), (unsigned long long)d);
						last_dropped[i] = d;
					}
				}
			}
			if (n == 0)
				os_sleep_ms(10);
		}

		// Final drain after capture was detached.
		const int64_t nf = now_frames(os_gettime_ns());
		for (auto &r : recs_)
			while (r->drain(timeline_, nf, max_per_pass) > 0) {
			}
	} catch (const std::exception &e) {
		blog(LOG_ERROR, "[Audio Stems] writer thread error: %s", e.what());
	} catch (...) {
		blog(LOG_ERROR, "[Audio Stems] writer thread: unknown error");
	}
}

void Session::stop()
{
	if (!running_)
		return;
	running_ = false;

	stop_ns_ = os_gettime_ns();
	add_marker("session_stop", "");

	for (auto &r : recs_)
		r->detach();

	writer_stop_.store(true, std::memory_order_release);
	if (writer_.joinable())
		writer_.join();

	// Every stem gets the same length so they line up in any editor.
	uint64_t total = 0;
	for (auto &r : recs_)
		total = std::max(total, r->frames_written());

	auto report = std::make_shared<SessionReport>();
	report->settings = settings_;
	report->kind = kind_;
	report->session_dir = session_dir_;
	report->recording_path = recording_path_;
	report->started_at = started_at_;
	report->sample_rate = fmt_.sample_rate;
	report->channels = fmt_.channels;
	report->sample_format = fmt_.sample_format;
	report->frames = total;

	for (auto &r : recs_) {
		r->finish(total);
		StemReport st;
		st.kind = r->kind();
		st.track = r->track_index();
		st.wav_path = r->wav_path();
		st.uuid = r->source_uuid();
		st.name = r->source_name();
		st.frames = r->frames_written();
		st.dropped = r->dropped_frames();
		st.padded = r->padded_frames();
		st.failed = r->write_failed();
		st.removed = r->source_removed();
		report->stems.push_back(std::move(st));
		r->release();
	}
	recs_.clear();

	{
		std::lock_guard<std::mutex> lock(markers_mtx_);
		for (const auto &m : markers_) {
			MarkerReport mr;
			mr.frames = std::min<int64_t>(std::max<int64_t>(0, m.frames), (int64_t)total);
			mr.type = m.type;
			mr.value = m.value;
			report->markers.push_back(std::move(mr));
		}
	}

	blog(LOG_INFO, "[Audio Stems] %s session stopped (%.1f s); finalizing in background",
	     session_kind_label(kind_), (double)total / (double)fmt_.sample_rate);

	finalizer_.enqueue([report](const std::atomic<bool> &cancel) { finalize_session(*report, cancel); });
}

void Session::pause()
{
	if (running_ && timeline_.pause(os_gettime_ns()))
		add_marker("pause", "");
}

void Session::resume()
{
	if (running_ && timeline_.resume(os_gettime_ns()))
		add_marker("resume", "");
}

void Session::add_marker(const std::string &type, const std::string &value)
{
	const uint64_t now = os_gettime_ns();
	const int64_t frames = (int64_t)(timeline_.active_ns(now) / 1000) * (int64_t)fmt_.sample_rate / 1000000;
	std::lock_guard<std::mutex> lock(markers_mtx_);
	markers_.push_back(Marker{frames, now, type, value});
}

SessionStatus Session::status() const
{
	SessionStatus st;
	st.running = running_;
	if (!running_)
		return st;
	st.paused = timeline_.is_paused();
	st.stems = recs_.size();
	st.elapsed_ns = timeline_.active_ns(os_gettime_ns());
	st.session_dir = session_dir_;
	for (const auto &r : recs_) {
		st.dropped_frames += r->dropped_frames();
		st.failed_stems += r->write_failed() ? 1 : 0;
	}
	return st;
}

// ---------------------------------------------------------------------------
// Crash recovery
// ---------------------------------------------------------------------------

void repair_interrupted_sessions(const std::string &base_dir, const std::atomic<bool> &cancel)
{
	if (base_dir.empty())
		return;
	std::error_code ec;
	const fs::path base = u8path(base_dir);
	if (!fs::is_directory(base, ec))
		return;

	for (fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
	     !ec && it != end && !cancel.load(); it.increment(ec)) {
		std::error_code ec2;
		if (!it->is_directory(ec2))
			continue;
		const fs::path marker = it->path() / k_inprogress_marker;
		if (!fs::exists(marker, ec2))
			continue;

		size_t repaired = 0;
		for (fs::directory_iterator f(it->path(), ec2), fend; !ec2 && f != fend; f.increment(ec2)) {
			std::error_code ec3;
			if (!f->is_regular_file(ec3))
				continue;
			const fs::path p = f->path();
			const std::string ext = p.extension().string();
			if (ext == ".tmp" || ext == ".part") {
				fs::remove(p, ec3); // leftovers of an interrupted post-process/export
				continue;
			}
			if ((ext == ".wav" || ext == ".WAV") && repair_wav_file(to_u8(p)))
				repaired++;
		}
		fs::remove(marker, ec2);
		blog(LOG_WARNING, "[Audio Stems] recovered %zu stem(s) from interrupted session: %s", repaired,
		     to_u8(it->path()).c_str());
	}
}

} // namespace stems
