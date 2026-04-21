#pragma once

#include <cstdint>
#include <string>

namespace stems {

enum class OutputFormat {
	Wav,
	Mp3,
};

bool export_audio(const std::string &ffmpeg_path_or_empty, const std::string &input_wav_path,
			 const std::string &output_path, OutputFormat format, int bitrate_kbps,
			 uint32_t sample_rate, uint16_t channels, int wav_bit_depth);

}
