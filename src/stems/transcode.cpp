#include "transcode.hpp"

#include <obs-module.h>

#include <cstdlib>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace stems {

static std::string shell_quote(const std::string &s)
{
#if defined(_WIN32)
	// Best-effort quoting for CreateProcess commandline: wrap in quotes and escape internal quotes.
	std::string out = "\"";
	for (char c : s) {
		if (c == '"')
			out += "\\\"";
		else
			out += c;
	}
	out += "\"";
	return out;
#else
	std::string out = "'";
	for (char c : s) {
		if (c == '\'')
			out += "'\\''"; // close, escape, reopen
		else
			out += c;
	}
	out += "'";
	return out;
#endif
}

bool wav_to_mp3(const std::string &ffmpeg_path_or_empty, const std::string &wav_path,
			const std::string &mp3_path, int bitrate_kbps)
{
	if (bitrate_kbps < 64)
		bitrate_kbps = 64;
	if (bitrate_kbps > 320)
		bitrate_kbps = 320;

	const std::string ff = ffmpeg_path_or_empty.empty() ? "ffmpeg" : ffmpeg_path_or_empty;

	// -y overwrite, -hide_banner quieter, -loglevel error minimal
	std::stringstream cmd;
	cmd << shell_quote(ff) << " -y -hide_banner -loglevel error";
	cmd << " -i " << shell_quote(wav_path);
	cmd << " -vn -acodec libmp3lame -b:a " << bitrate_kbps << "k";
	cmd << " " << shell_quote(mp3_path);

	int rc = std::system(cmd.str().c_str());
	if (rc != 0) {
		blog(LOG_ERROR, "Audio Stems: ffmpeg transcode failed (rc=%d)", rc);
		return false;
	}
	return true;
}

} // namespace stems
