#pragma once

#include <string>

namespace stems {

// Returns true on success. On failure, WAV is left intact.
bool wav_to_mp3(const std::string &ffmpeg_path_or_empty, const std::string &wav_path,
			const std::string &mp3_path, int bitrate_kbps);

} // namespace stems
