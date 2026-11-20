#include "stem_recorder.hpp"

#include <algorithm>
#include <cstring>

namespace stems {

namespace {
// Gaps in a source's timestamps shorter than this are treated as jitter.
constexpr int64_t k_gap_tolerance_ms = 40;
// A source running ahead of the timeline by more than this is re-synced.
constexpr int64_t k_overlap_resync_ms = 500;
} // namespace

StemRecorder::~StemRecorder()
{
	detach();
	if (wav_.is_open())
		wav_.close();
	release();
}

bool StemRecorder::open_common(const std::string &wav_path, const StemFormat &fmt)
{
	fmt_ = fmt;
	if (fmt_.channels == 0 || fmt_.channels > MAX_AUDIO_CHANNELS)
		fmt_.channels = 2;
	if (fmt_.sample_rate == 0)
		fmt_.sample_rate = 48000;
	wav_path_ = wav_path;

	const double secs = fmt_.ring_seconds < 1.0 ? 1.0 : fmt_.ring_seconds;
	samples_.init((size_t)(secs * fmt_.sample_rate) * fmt_.channels);
	headers_.init(8192);
	// Largest chunk OBS delivers in one callback is bounded by the ring;
	// reserve once so the writer thread never reallocates in steady state.
	scratch_.reserve((size_t)fmt_.sample_rate / 5 * fmt_.channels);

	if (!wav_.open(wav_path_, fmt_.sample_rate, fmt_.channels, fmt_.sample_format)) {
		blog(LOG_ERROR, "[Audio Stems] could not create '%s'", wav_path_.c_str());
		return false;
	}
	return true;
}

bool StemRecorder::start_source(obs_source_t *source, bool post_fader, const std::string &wav_path,
				const StemFormat &fmt)
{
	if (!source)
		return false;
	source_ = obs_source_get_ref(source);
	if (!source_)
		return false;

	kind_ = StemKind::Source;
	post_fader_ = post_fader;
	const char *uuid = obs_source_get_uuid(source_);
	const char *name = obs_source_get_name(source_);
	source_uuid_ = uuid ? uuid : "";
	source_name_ = name ? name : "";

	if (!open_common(wav_path, fmt)) {
		release();
		return false;
	}

	// Detach immediately if the user deletes the source mid-session; we keep
	// our reference so the object stays valid until the session ends.
	signal_handler_connect(obs_source_get_signal_handler(source_), "remove", &StemRecorder::source_remove_cb, this);
	signals_connected_ = true;

	accepting_.store(true, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(attach_mtx_);
		obs_source_add_audio_capture_callback(source_, &StemRecorder::source_audio_cb, this);
		attached_ = true;
	}
	return true;
}

bool StemRecorder::start_track(size_t mix_idx, const std::string &wav_path, const StemFormat &fmt)
{
	if (mix_idx >= MAX_AUDIO_MIXES)
		return false;
	audio_ = obs_get_audio();
	if (!audio_)
		return false;

	kind_ = StemKind::Track;
	mix_idx_ = mix_idx;
	source_name_ = "Track " + std::to_string(mix_idx + 1);

	if (!open_common(wav_path, fmt))
		return false;

	accepting_.store(true, std::memory_order_release);
	std::lock_guard<std::mutex> lock(attach_mtx_);
	if (!audio_output_connect(audio_, mix_idx_, nullptr, &StemRecorder::track_audio_cb, this)) {
		accepting_.store(false, std::memory_order_release);
		wav_.close();
		blog(LOG_ERROR, "[Audio Stems] could not connect to mixer track %zu", mix_idx + 1);
		return false;
	}
	attached_ = true;
	return true;
}

void StemRecorder::detach()
{
	accepting_.store(false, std::memory_order_release);
	std::lock_guard<std::mutex> lock(attach_mtx_);
	if (!attached_)
		return;
	attached_ = false;
	if (kind_ == StemKind::Source) {
		if (source_)
			obs_source_remove_audio_capture_callback(source_, &StemRecorder::source_audio_cb, this);
	} else if (audio_ && audio_ == obs_get_audio()) {
		// Once this returns the audio thread is guaranteed not to be inside
		// our callback anymore.
		audio_output_disconnect(audio_, mix_idx_, &StemRecorder::track_audio_cb, this);
	}
}

void StemRecorder::release()
{
	if (source_) {
		if (signals_connected_) {
			signal_handler_disconnect(obs_source_get_signal_handler(source_), "remove",
						  &StemRecorder::source_remove_cb, this);
			signals_connected_ = false;
		}
		obs_source_release(source_);
		source_ = nullptr;
	}
	audio_ = nullptr;
}

// ---------------------------------------------------------------------------
// audio thread
// ---------------------------------------------------------------------------

void StemRecorder::source_audio_cb(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	auto *self = static_cast<StemRecorder *>(param);
	if (!self || !audio)
		return;
	float gain = 1.0f;
	if (self->post_fader_ && source)
		gain = obs_source_get_volume(source);
	self->push(audio, muted, gain);
}

void StemRecorder::track_audio_cb(void *param, size_t, struct audio_data *audio)
{
	auto *self = static_cast<StemRecorder *>(param);
	if (!self || !audio)
		return;
	self->push(audio, false, 1.0f);
}

void StemRecorder::source_remove_cb(void *param, calldata_t *)
{
	auto *self = static_cast<StemRecorder *>(param);
	if (!self)
		return;
	self->source_removed_.store(true, std::memory_order_relaxed);
	self->detach();
}

void StemRecorder::push(const struct audio_data *audio, bool silent, float gain)
{
	if (!accepting_.load(std::memory_order_acquire))
		return;
	const uint32_t frames = audio->frames;
	if (frames == 0)
		return;

	const uint16_t ch = fmt_.channels;
	const size_t need = (size_t)frames * ch;
	if (headers_.write_available() < 1 || samples_.write_available() < need) {
		// Disk is not keeping up; never block the audio thread.
		dropped_frames_.fetch_add(frames, std::memory_order_relaxed);
		return;
	}

	const size_t base = samples_.write_pos();
	if (silent || gain == 0.0f) {
		for (size_t i = 0; i < need; i++)
			samples_.at(base + i) = 0.0f;
	} else {
		for (uint16_t c = 0; c < ch; c++) {
			const float *plane = reinterpret_cast<const float *>(audio->data[c]);
			size_t idx = base + c;
			if (!plane) {
				for (uint32_t i = 0; i < frames; i++, idx += ch)
					samples_.at(idx) = 0.0f;
			} else if (gain == 1.0f) {
				for (uint32_t i = 0; i < frames; i++, idx += ch)
					samples_.at(idx) = plane[i];
			} else {
				for (uint32_t i = 0; i < frames; i++, idx += ch)
					samples_.at(idx) = plane[i] * gain;
			}
		}
	}
	samples_.commit_write(need);

	ChunkHdr h;
	h.timestamp = audio->timestamp;
	h.frames = frames;
	headers_.push(h);
}

// ---------------------------------------------------------------------------
// writer thread
// ---------------------------------------------------------------------------

size_t StemRecorder::drain(const Timeline &timeline, int64_t now_frames, size_t max_frames)
{
	size_t consumed = 0;
	while (consumed < max_frames && headers_.read_available() > 0) {
		const ChunkHdr h = headers_.read_slot(0);
		const size_t n = (size_t)h.frames * fmt_.channels;
		if (samples_.read_available() < n)
			break; // cannot happen (samples are published first), but be defensive
		scratch_.resize(n);
		samples_.read(scratch_.data(), n);
		headers_.commit_read(1);
		place_chunk(h, scratch_.data(), timeline, now_frames);
		consumed += h.frames;
	}
	return consumed;
}

void StemRecorder::place_chunk(const ChunkHdr &h, const float *data, const Timeline &timeline, int64_t now_frames)
{
	if (write_failed_.load(std::memory_order_relaxed))
		return;

	const Timeline::Pos pos = timeline.map(h.timestamp);
	if (pos.valid && pos.paused)
		return; // recording is paused: keep stems in lock-step with the video

	size_t skip = 0;
	if (pos.valid) {
		const int64_t rate = timeline.sample_rate();
		const int64_t written = (int64_t)wav_.frames_written();
		const int64_t diff = pos.frames - written;
		const int64_t tol = rate * k_gap_tolerance_ms / 1000;
		// Never pad toward a timestamp that lies in the future (bogus clock).
		const bool plausible = pos.frames <= now_frames + rate;

		if (!timeline_started_) {
			if (diff < 0) {
				// Audio captured before the session anchor.
				if ((uint64_t)(-diff) >= h.frames)
					return;
				skip = (size_t)(-diff);
			} else if (diff > 0 && plausible) {
				write_silence((uint64_t)diff);
			}
		} else if (diff > tol && plausible) {
			// Source went quiet (no audio delivered): fill the hole so the
			// stem stays aligned with every other stem and the recording.
			write_silence((uint64_t)diff);
		} else if (diff < -std::max<int64_t>(tol, rate * k_overlap_resync_ms / 1000)) {
			skip = (size_t)std::min<int64_t>(-diff, h.frames);
		}
	}

	timeline_started_ = true;
	if (skip < h.frames)
		write_frames(data + skip * fmt_.channels, h.frames - skip);
}

bool StemRecorder::write_frames(const float *data, size_t frames)
{
	if (!wav_.write(data, frames)) {
		if (!write_failed_.exchange(true))
			blog(LOG_ERROR, "[Audio Stems] write failed for '%s' (disk full or removed?). Stem stopped.",
			     wav_path_.c_str());
		return false;
	}
	frames_written_.store(wav_.frames_written(), std::memory_order_relaxed);
	return true;
}

bool StemRecorder::write_silence(uint64_t frames)
{
	if (frames == 0)
		return true;
	if (!wav_.write_silence(frames)) {
		if (!write_failed_.exchange(true))
			blog(LOG_ERROR, "[Audio Stems] write failed for '%s' (disk full or removed?). Stem stopped.",
			     wav_path_.c_str());
		return false;
	}
	padded_frames_ += frames;
	frames_written_.store(wav_.frames_written(), std::memory_order_relaxed);
	return true;
}

void StemRecorder::refresh_header()
{
	if (wav_.is_open() && !write_failed_.load(std::memory_order_relaxed))
		wav_.refresh_header();
}

bool StemRecorder::finish(uint64_t total_frames)
{
	if (!wav_.is_open())
		return false;
	const uint64_t written = wav_.frames_written();
	if (total_frames > written && !write_failed_.load(std::memory_order_relaxed)) {
		// Pad so every stem in the session has exactly the same length.
		const uint64_t pad = total_frames - written;
		if (wav_.write_silence(pad))
			padded_frames_ += pad;
	}
	frames_written_.store(wav_.frames_written(), std::memory_order_relaxed);
	return wav_.close();
}

} // namespace stems
