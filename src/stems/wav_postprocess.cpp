#include "wav_postprocess.hpp"

#include "wav_writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace stems {
namespace fs = std::filesystem;

static inline int16_t clamp_s16(int32_t v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (int16_t)v;
}

static bool read_pcm16_data(const std::string &path, std::vector<int16_t> &out, uint16_t channels)
{
	out.clear();
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	// Minimal WAV: we expect 44-byte header written by WavWriter
	if (std::fseek(f, 0, SEEK_END) != 0) {
		std::fclose(f);
		return false;
	}
	long sz = std::ftell(f);
	if (sz < 44) {
		std::fclose(f);
		return false;
	}
	const long data_bytes = sz - 44;
	if (data_bytes <= 0) {
		std::fclose(f);
		return true;
	}
	if (data_bytes % 2 != 0) {
		std::fclose(f);
		return false;
	}
	const size_t samples = (size_t)(data_bytes / 2);
	if (samples < channels) {
		std::fclose(f);
		return true;
	}
	if (std::fseek(f, 44, SEEK_SET) != 0) {
		std::fclose(f);
		return false;
	}
	out.resize(samples);
	size_t rd = std::fread(out.data(), sizeof(int16_t), samples, f);
	std::fclose(f);
	if (rd != samples)
		return false;
	return true;
}

static bool write_pcm16_data(const std::string &path, const std::vector<int16_t> &samples,
			     uint32_t sample_rate, uint16_t channels)
{
	WavWriter w;
	if (!w.open(path, sample_rate, channels))
		return false;
	const size_t frames = channels ? (samples.size() / channels) : 0;
	if (frames > 0) {
		// WavWriter expects frames and interleaved samples
		if (!w.write_samples(samples.data(), frames)) {
			w.close();
			return false;
		}
	}
	w.close();
	return true;
}

static bool swap_in_tmp(const std::string &original, const std::string &tmp)
{
	std::error_code ec;
	fs::remove(original, ec);
	ec.clear();
	fs::rename(tmp, original, ec);
	return !ec;
}

bool trim_silence_wav(const std::string &wav_path, uint16_t channels, uint32_t sample_rate,
			      float threshold_dbfs, int lead_ms, int trail_ms)
{
	if (channels == 0)
		channels = 2;
	if (sample_rate == 0)
		sample_rate = 48000;

	std::vector<int16_t> s;
	if (!read_pcm16_data(wav_path, s, channels))
		return false;
	if (s.empty())
		return true;

	const int32_t thr = (int32_t)std::round(std::pow(10.0f, threshold_dbfs / 20.0f) * 32767.0f);
	const int32_t athr = std::max<int32_t>(1, std::min<int32_t>(32767, std::abs(thr)));

	const size_t total_frames = s.size() / channels;
	if (total_frames == 0)
		return true;

	auto frame_is_audible = [&](size_t frame) {
		for (uint16_t ch = 0; ch < channels; ch++) {
			int32_t v = s[frame * channels + ch];
			if (v < 0)
				v = -v;
			if (v >= athr)
				return true;
		}
		return false;
	};

	size_t first = 0;
	while (first < total_frames && !frame_is_audible(first))
		first++;
	if (first >= total_frames) {
		// All silence: keep as-is
		return true;
	}

	size_t last = total_frames - 1;
	while (last > first && !frame_is_audible(last))
		last--;

	const size_t lead_frames = (size_t)std::max(0, lead_ms) * sample_rate / 1000u;
	const size_t trail_frames = (size_t)std::max(0, trail_ms) * sample_rate / 1000u;

	size_t start = (first > lead_frames) ? (first - lead_frames) : 0;
	size_t end = std::min(total_frames, last + 1 + trail_frames);
	if (start == 0 && end == total_frames)
		return true;

	std::vector<int16_t> out;
	out.reserve((end - start) * channels);
	for (size_t f = start; f < end; f++) {
		for (uint16_t ch = 0; ch < channels; ch++)
			out.push_back(s[f * channels + ch]);
	}

	fs::path p = fs::path(wav_path);
	fs::path tmp = p;
	tmp += ".trim.tmp";
	if (!write_pcm16_data(tmp.string(), out, sample_rate, channels)) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}
	if (!swap_in_tmp(wav_path, tmp.string())) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}
	return true;
}

bool normalize_wav_rms(const std::string &wav_path, uint16_t channels, uint32_t sample_rate,
			       float target_dbfs, bool limiter_enabled)
{
	if (channels == 0)
		channels = 2;
	if (sample_rate == 0)
		sample_rate = 48000;

	std::vector<int16_t> s;
	if (!read_pcm16_data(wav_path, s, channels))
		return false;
	if (s.empty())
		return true;

	// RMS over all samples
	long double sum_sq = 0.0L;
	int32_t peak = 0;
	for (int16_t v : s) {
		int32_t a = v;
		if (a < 0)
			a = -a;
		if (a > peak)
			peak = a;
		long double f = (long double)v / 32768.0L;
		sum_sq += f * f;
	}
	const long double mean_sq = sum_sq / (long double)s.size();
	const long double rms = std::sqrt(mean_sq);
	if (rms <= 0.0000001L)
		return true;

	const long double target_lin = std::pow(10.0L, (long double)target_dbfs / 20.0L);
	long double gain = target_lin / rms;
	if (limiter_enabled && peak > 0) {
		const long double max_gain = (32767.0L / (long double)peak);
		if (gain > max_gain)
			gain = max_gain;
	}
	if (gain <= 0.0L)
		return true;

	// Apply gain
	std::vector<int16_t> out;
	out.resize(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		int32_t v = (int32_t)std::llround((long double)s[i] * gain);
		out[i] = clamp_s16(v);
	}

	fs::path p = fs::path(wav_path);
	fs::path tmp = p;
	tmp += ".norm.tmp";
	if (!write_pcm16_data(tmp.string(), out, sample_rate, channels)) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}
	if (!swap_in_tmp(wav_path, tmp.string())) {
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
	}
	return true;
}

} // namespace stems
