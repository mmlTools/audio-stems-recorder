#include "stem_recorder.hpp"

#include <obs-module.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace stems {

static inline int16_t f32_to_s16(float v)
{
	if (v > 1.0f)
		v = 1.0f;
	else if (v < -1.0f)
		v = -1.0f;
	int x = (int)lrintf(v * 32767.0f);
	if (x > 32767)
		x = 32767;
	if (x < -32768)
		x = -32768;
	return (int16_t)x;
}

StemRecorder::~StemRecorder()
{
	stop();
}

bool StemRecorder::start(obs_source_t *source, const std::string &wav_path, uint32_t sample_rate, uint16_t channels)
{
	stop();
	if (!source)
		return false;

	source_ = source;
	const char *uuid = obs_source_get_uuid(source);
	const char *name = obs_source_get_name(source);
	source_uuid_ = uuid ? uuid : "";
	source_name_ = name ? name : "";

	sample_rate_ = sample_rate ? sample_rate : 48000;
	channels_ = channels ? channels : 2;

	if (!wav_.open(wav_path, sample_rate_, channels_))
		return false;

	running_ = true;
	stopping_ = false;

	worker_ = std::thread(&StemRecorder::worker_main, this);

	obs_source_add_audio_capture_callback(source_, &StemRecorder::audio_cb, this);
	return true;
}

void StemRecorder::stop()
{
	if (!running_)
		return;

	stopping_ = true;
	if (source_)
		obs_source_remove_audio_capture_callback(source_, &StemRecorder::audio_cb, this);

	running_ = false;
	if (worker_.joinable())
		worker_.join();

	{
		std::lock_guard<std::mutex> lock(mtx_);
		queue_.clear();
	}
	wav_.close();
	source_ = nullptr;
}

void StemRecorder::audio_cb(void *param, obs_source_t *, const struct audio_data *audio, bool muted)
{
	auto *self = static_cast<StemRecorder *>(param);
	if (!self || !audio)
		return;
	self->on_audio(audio, muted);
}

void StemRecorder::on_audio(const struct audio_data *audio, bool muted)
{
	if (stopping_)
		return;

	const uint32_t frames = audio->frames;
	if (frames == 0)
		return;

	// OBS typically provides float planar. We interleave to int16.
	PcmChunk chunk;
	chunk.frames = frames;
	chunk.samples.resize((size_t)frames * (size_t)channels_);

	for (uint32_t i = 0; i < frames; i++) {
		for (uint16_t ch = 0; ch < channels_; ch++) {
			float v = 0.0f;
			if (!muted && audio->data[ch]) {
				const float *plane = reinterpret_cast<const float *>(audio->data[ch]);
				v = plane[i];
			}
			chunk.samples[(size_t)i * (size_t)channels_ + ch] = f32_to_s16(v);
		}
	}

	// Keep queue bounded to avoid unbounded memory usage.
	// If worker falls behind, drop oldest chunks.
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (queue_.size() >= 128) {
			queue_.pop_front();
			dropped_chunks_++;
		}
		queue_.push_back(std::move(chunk));
	}
}

void StemRecorder::worker_main()
{
	while (running_ || stopping_) {
		PcmChunk chunk;
		bool got = false;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			if (!queue_.empty()) {
				chunk = std::move(queue_.front());
				queue_.pop_front();
				got = true;
			}
		}

		if (!got) {
			if (!running_)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}

	if (!wav_.write_samples(chunk.samples.data(), chunk.frames)) {
		// If IO fails, stop capturing for this stem.
		blog(LOG_ERROR, "Audio Stems: failed writing WAV for %s", source_name_.c_str());
		break;
	}
	}
}

} // namespace stems
