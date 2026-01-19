#pragma once

#include <cstdint>
#include <string>

namespace stems {

// Trims leading/trailing silence in-place (via temp file swap) for 16-bit PCM WAV.
// Returns true on success; if no trimming needed, returns true.
bool trim_silence_wav(const std::string &wav_path, uint16_t channels, uint32_t sample_rate,
			      float threshold_dbfs, int lead_ms, int trail_ms);

// Normalizes RMS level (dBFS) in-place (via temp file swap) for 16-bit PCM WAV.
// If limiter_enabled, gain will be reduced to avoid clipping.
bool normalize_wav_rms(const std::string &wav_path, uint16_t channels, uint32_t sample_rate,
			       float target_dbfs, bool limiter_enabled);

} // namespace stems
