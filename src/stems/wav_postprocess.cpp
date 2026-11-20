#include "wav_postprocess.hpp"

#include "paths.hpp"
#include "wav_io.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <vector>

namespace stems {
namespace fs = std::filesystem;

namespace {

constexpr size_t k_block_frames = 16384;
constexpr double k_gate_dbfs = -60.0; // ignore near-silence when measuring loudness
constexpr double k_limiter_ceiling = 0.966; // -0.3 dBFS

inline double to_db(double lin)
{
	return lin > 1e-9 ? 20.0 * std::log10(lin) : -180.0;
}

struct Analysis {
	uint64_t frames = 0;
	bool any_audible = false;
	uint64_t first_audible = 0;
	uint64_t last_audible = 0;
	double peak = 0.0;
	double gated_rms = 0.0;
	double plain_rms = 0.0;
};

bool analyze(WavReader &rd, float audible_threshold, const std::atomic<bool> &cancel, Analysis &a)
{
	const WavInfo &info = rd.info();
	const uint16_t ch = info.channels;
	std::vector<float> buf(k_block_frames * ch);

	// Loudness is measured over 100 ms windows; windows below the gate are
	// ignored so long silent stretches do not inflate the normalisation gain.
	const uint64_t win_frames = std::max<uint64_t>(1, info.sample_rate / 10);
	const double gate_ms = std::pow(10.0, k_gate_dbfs / 10.0);
	double win_sum = 0.0;
	uint64_t win_n = 0;
	long double gated_sum = 0.0L, total_sum = 0.0L;
	uint64_t gated_n = 0, total_n = 0;

	uint64_t frame = 0;
	for (;;) {
		if (cancel.load(std::memory_order_relaxed))
			return false;
		const size_t got = rd.read(buf.data(), k_block_frames);
		if (got == 0)
			break;
		for (size_t i = 0; i < got; i++, frame++) {
			float fpeak = 0.0f;
			double fsum = 0.0;
			const float *s = &buf[i * ch];
			for (uint16_t c = 0; c < ch; c++) {
				const float v = std::fabs(s[c]);
				fpeak = v > fpeak ? v : fpeak;
				fsum += (double)s[c] * (double)s[c];
			}
			if (fpeak >= audible_threshold) {
				if (!a.any_audible)
					a.first_audible = frame;
				a.any_audible = true;
				a.last_audible = frame;
			}
			if (fpeak > a.peak)
				a.peak = fpeak;

			win_sum += fsum;
			win_n += ch;
			if (win_n >= win_frames * ch) {
				if (win_sum / (double)win_n >= gate_ms) {
					gated_sum += win_sum;
					gated_n += win_n;
				}
				total_sum += win_sum;
				total_n += win_n;
				win_sum = 0.0;
				win_n = 0;
			}
		}
	}
	if (win_n > 0) {
		if (win_sum / (double)win_n >= gate_ms) {
			gated_sum += win_sum;
			gated_n += win_n;
		}
		total_sum += win_sum;
		total_n += win_n;
	}
	a.frames = frame;
	a.gated_rms = gated_n ? std::sqrt((double)(gated_sum / (long double)gated_n)) : 0.0;
	a.plain_rms = total_n ? std::sqrt((double)(total_sum / (long double)total_n)) : 0.0;
	return true;
}

} // namespace

bool analyze_wav(const std::string &path, const std::atomic<bool> &cancel, PostprocessResult &result)
{
	WavReader rd;
	if (!rd.open(path))
		return false;
	Analysis a;
	if (!analyze(rd, 1e-4f, cancel, a))
		return false;
	result.frames = a.frames;
	result.silent = !a.any_audible;
	result.peak_dbfs = to_db(a.peak);
	result.rms_dbfs = to_db(a.gated_rms > 0.0 ? a.gated_rms : a.plain_rms);
	return true;
}

bool postprocess_wav(const std::string &path, const PostprocessOptions &opt, const std::atomic<bool> &cancel,
		     PostprocessResult &result)
{
	result = PostprocessResult{};

	WavReader rd;
	if (!rd.open(path))
		return false;
	const WavInfo info = rd.info();
	const uint16_t ch = info.channels;

	const float threshold = (float)std::pow(10.0, std::min(-1.0f, opt.trim_threshold_dbfs) / 20.0);
	Analysis a;
	if (!analyze(rd, opt.trim ? threshold : 1e-4f, cancel, a))
		return false;

	result.frames = a.frames;
	result.peak_dbfs = to_db(a.peak);
	result.rms_dbfs = to_db(a.gated_rms > 0.0 ? a.gated_rms : a.plain_rms);
	result.silent = !a.any_audible || a.peak <= 0.0;
	if (a.frames == 0 || result.silent)
		return true; // nothing to trim or normalise; leave the (silent) stem untouched

	uint64_t start = 0, end = a.frames;
	if (opt.trim) {
		const uint64_t lead = (uint64_t)std::max(0, opt.trim_lead_ms) * info.sample_rate / 1000u;
		const uint64_t trail = (uint64_t)std::max(0, opt.trim_trail_ms) * info.sample_rate / 1000u;
		start = a.first_audible > lead ? a.first_audible - lead : 0;
		end = std::min<uint64_t>(a.frames, a.last_audible + 1 + trail);
	}

	double gain = 1.0;
	if (opt.normalize && a.gated_rms > 0.0) {
		gain = std::pow(10.0, (double)opt.normalize_target_dbfs / 20.0) / a.gated_rms;
		if (opt.normalize_limiter && a.peak * gain > k_limiter_ceiling)
			gain = k_limiter_ceiling / a.peak;
	}

	const bool gain_changes = std::fabs(to_db(gain)) >= 0.01;
	if (start == 0 && end == a.frames && !gain_changes)
		return true;

	fs::path tmp = u8path(path);
	tmp += ".processing.tmp";
	const std::string tmp_s = to_u8(tmp);

	bool ok = true;
	{
		WavWriter w;
		if (!w.open(tmp_s, info.sample_rate, ch, info.sample_format(), info.channel_mask))
			return false;
		if (!rd.seek_frame(start)) {
			w.close();
			ok = false;
		}
		std::vector<float> buf(k_block_frames * ch);
		const float g = (float)gain;
		uint64_t remaining = end - start;
		while (ok && remaining > 0) {
			if (cancel.load(std::memory_order_relaxed)) {
				ok = false;
				break;
			}
			const size_t want = (size_t)std::min<uint64_t>(remaining, k_block_frames);
			const size_t got = rd.read(buf.data(), want);
			if (got == 0) {
				ok = false;
				break;
			}
			if (gain_changes) {
				for (size_t i = 0; i < got * ch; i++)
					buf[i] *= g;
			}
			if (!w.write(buf.data(), got)) {
				ok = false;
				break;
			}
			remaining -= got;
		}
		ok = w.close() && ok;
	}
	rd.close();

	std::error_code ec;
	if (!ok) {
		fs::remove(tmp, ec);
		return false;
	}
	fs::rename(tmp, u8path(path), ec);
	if (ec) {
		fs::remove(tmp, ec);
		return false;
	}

	result.changed = true;
	result.trimmed_start_frames = start;
	result.frames = end - start;
	result.gain_db = gain_changes ? to_db(gain) : 0.0;
	return true;
}

} // namespace stems
