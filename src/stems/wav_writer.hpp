#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace stems {

class WavWriter {
public:
	WavWriter() = default;
	~WavWriter();
	WavWriter(const WavWriter &) = delete;
	WavWriter &operator=(const WavWriter &) = delete;

	bool open(const std::string &path, uint32_t sample_rate, uint16_t channels);
	bool write_samples(const int16_t *interleaved, size_t frames);
	void close();

	const std::string &path() const { return path_; }
	uint32_t sample_rate() const { return sample_rate_; }
	uint16_t channels() const { return channels_; }
	uint64_t frames_written() const { return frames_written_; }

	// Repair header sizes for a WAV file produced by this writer, based on file length.
	static bool repair_header(const std::string &path);

private:
	bool write_header_placeholder();
	bool finalize_header();

	std::FILE *fp_ = nullptr;
	std::string path_;
	uint32_t sample_rate_ = 48000;
	uint16_t channels_ = 2;
	uint64_t frames_written_ = 0;
};

} // namespace stems
