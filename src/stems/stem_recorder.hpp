#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <obs-module.h>

#include "spsc_ring.hpp"
#include "timeline.hpp"
#include "wav_io.hpp"

namespace stems {

enum class StemKind {
	Source, // a single OBS audio source (pre- or post-fader)
	Track,  // one of OBS' six mixer tracks
};

struct StemFormat {
	uint32_t sample_rate = 48000;
	uint16_t channels = 2;
	SampleFormat sample_format = SampleFormat::S24;
	double ring_seconds = 8.0;
};

// Captures one stem.
//
// Threading model:
//   * start/detach/finish/release: OBS UI thread (owned by Session).
//   * on_audio(): OBS audio thread. Lock-free, allocation-free, never blocks.
//   * drain()/refresh(): the session's writer thread.
class StemRecorder {
public:
	StemRecorder() = default;
	~StemRecorder();
	StemRecorder(const StemRecorder &) = delete;
	StemRecorder &operator=(const StemRecorder &) = delete;

	bool start_source(obs_source_t *source, bool post_fader, const std::string &wav_path, const StemFormat &fmt);
	bool start_track(size_t mix_idx, const std::string &wav_path, const StemFormat &fmt);

	// Stop receiving audio. Idempotent; safe to call more than once.
	void detach();

	// Writer thread: move queued audio to disk. Returns frames consumed.
	size_t drain(const Timeline &timeline, int64_t now_frames, size_t max_frames);
	void refresh_header();

	// After the writer thread has stopped: pad to `total_frames`, close file.
	bool finish(uint64_t total_frames);
	// Drops the source reference and signal connections (UI thread).
	void release();

	StemKind kind() const { return kind_; }
	size_t track_index() const { return mix_idx_; }
	const std::string &wav_path() const { return wav_path_; }
	const std::string &source_uuid() const { return source_uuid_; }
	const std::string &source_name() const { return source_name_; }
	uint64_t frames_written() const { return frames_written_.load(std::memory_order_relaxed); }
	uint64_t dropped_frames() const { return dropped_frames_.load(std::memory_order_relaxed); }
	uint64_t padded_frames() const { return padded_frames_; }
	bool write_failed() const { return write_failed_.load(std::memory_order_relaxed); }
	bool source_removed() const { return source_removed_.load(std::memory_order_relaxed); }

private:
	struct ChunkHdr {
		uint64_t timestamp = 0;
		uint32_t frames = 0;
	};

	bool open_common(const std::string &wav_path, const StemFormat &fmt);
	void push(const struct audio_data *audio, bool silent, float gain);
	void place_chunk(const ChunkHdr &h, const float *data, const Timeline &timeline, int64_t now_frames);
	bool write_frames(const float *data, size_t frames);
	bool write_silence(uint64_t frames);

	static void source_audio_cb(void *param, obs_source_t *source, const struct audio_data *audio, bool muted);
	static void track_audio_cb(void *param, size_t mix_idx, struct audio_data *audio);
	static void source_remove_cb(void *param, calldata_t *cd);

	StemKind kind_ = StemKind::Source;
	obs_source_t *source_ = nullptr; // strong reference held for the session
	size_t mix_idx_ = 0;
	audio_t *audio_ = nullptr;
	bool post_fader_ = false;
	bool signals_connected_ = false;

	std::mutex attach_mtx_;
	bool attached_ = false;

	std::string wav_path_;
	std::string source_uuid_;
	std::string source_name_;
	StemFormat fmt_;

	// audio thread -> writer thread
	std::atomic<bool> accepting_{false};
	SpscRing<float> samples_;
	SpscRing<ChunkHdr> headers_;
	std::atomic<uint64_t> dropped_frames_{0};
	std::atomic<bool> source_removed_{false};

	// writer thread only
	WavWriter wav_;
	std::vector<float> scratch_;
	bool timeline_started_ = false;
	uint64_t padded_frames_ = 0;
	std::atomic<uint64_t> frames_written_{0};
	std::atomic<bool> write_failed_{false};
};

} // namespace stems
