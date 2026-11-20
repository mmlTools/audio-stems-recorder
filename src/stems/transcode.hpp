#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace stems {

enum class OutputFormat {
	Wav,
	Flac,
	Mp3,
};

OutputFormat output_format_from_string(const std::string &s);
const char *output_format_to_string(OutputFormat f);
const char *output_format_extension(OutputFormat f);

// Locates an ffmpeg executable: the configured path if valid, otherwise the
// PATH. Returns an empty string if none is found.
std::string find_ffmpeg(const std::string &configured);

// Encodes `input_wav` into `output_path` using an external ffmpeg process.
// Runs without a console window, at reduced priority, and is killed promptly
// when `cancel` becomes true. The output is written to a temporary file and
// only renamed into place on success.
bool export_audio(const std::string &ffmpeg, const std::string &input_wav, const std::string &output_path,
		  OutputFormat format, int mp3_bitrate_kbps, const std::atomic<bool> &cancel);

} // namespace stems
