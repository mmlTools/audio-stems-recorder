#include "transcode.hpp"

#include <obs-module.h>

#include <cstdlib>
#include <sstream>

namespace stems {

static std::string shell_quote(const std::string &s)
{
#if defined(_WIN32)
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
			out += "'\\''";
		else
			out += c;
	}
	out += "'";
	return out;
#endif
}

static const char *wav_codec_for_bit_depth(int wav_bit_depth)
{
	switch (wav_bit_depth) {
	case 24:
		return "pcm_s24le";
	case 32:
		return "pcm_s32le";
	default:
		return "pcm_s16le";
	}
}

bool export_audio(const std::string &ffmpeg_path_or_empty, const std::string &input_wav_path,
			 const std::string &output_path, OutputFormat format, int bitrate_kbps,
			 uint32_t sample_rate, uint16_t channels, int wav_bit_depth)
{
	if (bitrate_kbps < 64)
		bitrate_kbps = 64;
	if (bitrate_kbps > 320)
		bitrate_kbps = 320;
	if (sample_rate == 0)
		sample_rate = 48000;
	if (channels == 0)
		channels = 2;
	if (wav_bit_depth != 24 && wav_bit_depth != 32)
		wav_bit_depth = 16;

	const std::string ff = ffmpeg_path_or_empty.empty() ? "ffmpeg" : ffmpeg_path_or_empty;

	std::stringstream cmd;
	cmd << shell_quote(ff) << " -y -hide_banner -loglevel error";
	cmd << " -i " << shell_quote(input_wav_path);
	cmd << " -vn -ar " << sample_rate;
	cmd << " -ac " << static_cast<unsigned int>(channels);

	if (format == OutputFormat::Mp3) {
		cmd << " -acodec libmp3lame -b:a " << bitrate_kbps << "k";
	} else {
		cmd << " -acodec " << wav_codec_for_bit_depth(wav_bit_depth);
	}

	cmd << " " << shell_quote(output_path);

	int rc = std::system(cmd.str().c_str());
	if (rc != 0) {
		blog(LOG_ERROR, "Audio Stems: ffmpeg export failed (rc=%d)", rc);
		return false;
	}
	return true;
}

}
