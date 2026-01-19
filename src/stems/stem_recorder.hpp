#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <obs-module.h>

#include "wav_writer.hpp"

namespace stems {

struct PcmChunk {
	std::vector<int16_t> samples; // interleaved int16
	uint32_t frames = 0;
};

class StemRecorder {
public:
	StemRecorder() = default;
	~StemRecorder();
	StemRecorder(const StemRecorder &) = delete;
	StemRecorder &operator=(const StemRecorder &) = delete;

	bool start(obs_source_t *source, const std::string &wav_path, uint32_t sample_rate, uint16_t channels);
	void stop();

	const std::string &wav_path() const { return wav_.path(); }
	const std::string &source_uuid() const { return source_uuid_; }
	const std::string &source_name() const { return source_name_; }

private:
	static void audio_cb(void *param, obs_source_t *source, const struct audio_data *audio, bool muted);
	void on_audio(const struct audio_data *audio, bool muted);
	void worker_main();

	obs_source_t *source_ = nullptr; // weak
	std::string source_uuid_;
	std::string source_name_;

	uint32_t sample_rate_ = 48000;
	uint16_t channels_ = 2;

	WavWriter wav_;

	std::mutex mtx_;
	std::deque<PcmChunk> queue_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stopping_{false};
	std::thread worker_;

	std::atomic<uint64_t> dropped_chunks_{0};
};

} // namespace stems
