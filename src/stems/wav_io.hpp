#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace stems {

enum class SampleFormat {
	S16,
	S24,
	F32,
};

SampleFormat sample_format_from_bits(int bits); // 16, 24, 32 (float)
int sample_format_bits(SampleFormat f);
const char *sample_format_name(SampleFormat f);

// Streaming WAV writer.
//
// * Accepts interleaved float input and converts to the target sample format.
// * Reserves a JUNK chunk so the file is transparently upgraded to RF64 when
//   it grows past 4 GiB (long streams at 24/32-bit or multichannel).
// * Can refresh its header periodically so a hard crash leaves a playable file.
class WavWriter {
public:
	WavWriter() = default;
	~WavWriter();
	WavWriter(const WavWriter &) = delete;
	WavWriter &operator=(const WavWriter &) = delete;

	bool open(const std::string &path, uint32_t sample_rate, uint16_t channels, SampleFormat fmt,
		  uint32_t channel_mask = 0);
	bool write(const float *interleaved, size_t frames);
	bool write_silence(uint64_t frames);
	// Rewrites the size fields in place and flushes stdio buffers to the OS.
	bool refresh_header();
	bool close();

	bool is_open() const { return fp_ != nullptr; }
	const std::string &path() const { return path_; }
	uint32_t sample_rate() const { return sample_rate_; }
	uint16_t channels() const { return channels_; }
	SampleFormat format() const { return fmt_; }
	uint64_t frames_written() const { return frames_written_; }

private:
	bool write_header(uint32_t channel_mask);
	bool write_sizes(bool final);
	bool write_raw(const void *data, size_t bytes);

	std::FILE *fp_ = nullptr;
	std::string path_;
	uint32_t sample_rate_ = 48000;
	uint16_t channels_ = 2;
	SampleFormat fmt_ = SampleFormat::S16;
	uint32_t block_align_ = 4;
	uint64_t data_offset_ = 0;
	uint64_t data_size_field_offset_ = 0;
	uint64_t frames_written_ = 0;
	bool failed_ = false;
	std::vector<uint8_t> scratch_;
};

struct WavInfo {
	uint32_t sample_rate = 0;
	uint16_t channels = 0;
	uint16_t bits = 0;
	bool is_float = false;
	uint32_t channel_mask = 0;
	uint64_t data_offset = 0;
	uint64_t data_bytes = 0;
	uint64_t frames = 0;

	SampleFormat sample_format() const;
};

// Streaming WAV / RF64 reader that yields interleaved float frames.
class WavReader {
public:
	WavReader() = default;
	~WavReader();
	WavReader(const WavReader &) = delete;
	WavReader &operator=(const WavReader &) = delete;

	bool open(const std::string &path);
	void close();
	const WavInfo &info() const { return info_; }
	bool seek_frame(uint64_t frame);
	// Returns number of frames read (0 at end of data or on error).
	size_t read(float *interleaved, size_t frames);

private:
	std::FILE *fp_ = nullptr;
	WavInfo info_;
	uint64_t pos_frames_ = 0;
	std::vector<uint8_t> scratch_;
};

// Parses a WAV header, optionally tolerating unfinished size fields.
bool read_wav_info(std::FILE *f, uint64_t file_size, WavInfo &out, bool tolerate_unfinished);

// Fixes the size fields of a WAV left behind by a crash. Handles files written
// by older plugin versions (plain 44-byte header) as well as RF64-capable ones.
bool repair_wav_file(const std::string &path);

uint32_t default_channel_mask(uint16_t channels);

} // namespace stems
