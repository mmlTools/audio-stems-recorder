#pragma once

#include <cstdint>
#include <string>

namespace stems {



bool trim_silence_wav(const std::string &wav_path, uint16_t channels, uint32_t sample_rate,
			      float threshold_dbfs, int lead_ms, int trail_ms);



bool normalize_wav_rms(const std::string &wav_path, uint16_t channels, uint32_t sample_rate,
			       float target_dbfs, bool limiter_enabled);

} 
