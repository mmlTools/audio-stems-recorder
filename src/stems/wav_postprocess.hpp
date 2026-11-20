#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace stems {

struct PostprocessOptions {
	bool trim = false;
	float trim_threshold_dbfs = -45.0f;
	int trim_lead_ms = 150;
	int trim_trail_ms = 350;

	bool normalize = false;
	float normalize_target_dbfs = -16.0f;
	bool normalize_limiter = true;
};

struct PostprocessResult {
	bool changed = false;
	bool silent = false;
	uint64_t trimmed_start_frames = 0; // offset of the first kept frame in the original timeline
	uint64_t frames = 0;               // resulting length
	double gain_db = 0.0;
	double peak_dbfs = -144.0;
	double rms_dbfs = -144.0; // gated RMS of the original
};

// Two-pass, constant-memory trim/normalise of a WAV file in place.
// Memory use is independent of the recording length (a few hundred KB), so
// multi-hour stems can be processed without risking an out-of-memory crash.
bool postprocess_wav(const std::string &path, const PostprocessOptions &opt, const std::atomic<bool> &cancel,
		     PostprocessResult &result);

// Analysis only (fills peak/rms/frames/silent).
bool analyze_wav(const std::string &path, const std::atomic<bool> &cancel, PostprocessResult &result);

} // namespace stems
