#include "wav_writer.hpp"

#include <cstring>
#include <filesystem>

namespace stems {

static void write_u32_le(std::FILE *f, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu),
			 (uint8_t)((v >> 16) & 0xFFu), (uint8_t)((v >> 24) & 0xFFu) };
	std::fwrite(b, 1, 4, f);
}

static void write_u16_le(std::FILE *f, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v & 0xFFu), (uint8_t)((v >> 8) & 0xFFu) };
	std::fwrite(b, 1, 2, f);
}

WavWriter::~WavWriter()
{
	close();
}

bool WavWriter::open(const std::string &path, uint32_t sample_rate, uint16_t channels)
{
	close();
	path_ = path;
	sample_rate_ = sample_rate ? sample_rate : 48000;
	channels_ = channels ? channels : 2;
	frames_written_ = 0;

	fp_ = std::fopen(path.c_str(), "wb");
	if (!fp_)
		return false;
	return write_header_placeholder();
}

bool WavWriter::write_header_placeholder()
{
	if (!fp_)
		return false;

	// RIFF header (will be finalized on close)
	std::fwrite("RIFF", 1, 4, fp_);
	write_u32_le(fp_, 0); // chunk size placeholder
	std::fwrite("WAVE", 1, 4, fp_);

	// fmt chunk
	std::fwrite("fmt ", 1, 4, fp_);
	write_u32_le(fp_, 16);             // PCM fmt chunk size
	write_u16_le(fp_, 1);              // audio format: PCM
	write_u16_le(fp_, channels_);
	write_u32_le(fp_, sample_rate_);
	uint32_t byte_rate = sample_rate_ * (uint32_t)channels_ * 2u;
	write_u32_le(fp_, byte_rate);
	uint16_t block_align = (uint16_t)(channels_ * 2u);
	write_u16_le(fp_, block_align);
	write_u16_le(fp_, 16); // bits per sample

	// data chunk header
	std::fwrite("data", 1, 4, fp_);
	write_u32_le(fp_, 0); // data size placeholder

	return true;
}

bool WavWriter::write_samples(const int16_t *interleaved, size_t frames)
{
	if (!fp_ || !interleaved || frames == 0)
		return true;
	size_t samples = frames * (size_t)channels_;
	size_t written = std::fwrite(interleaved, sizeof(int16_t), samples, fp_);
	if (written != samples)
		return false;
	frames_written_ += (uint64_t)frames;
	return true;
}

bool WavWriter::finalize_header()
{
	if (!fp_)
		return false;

	uint64_t data_bytes = frames_written_ * (uint64_t)channels_ * 2ull;
	uint32_t data_size = (data_bytes > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)data_bytes;
	uint32_t riff_size = 36u + data_size;

	// Seek back to RIFF chunk size (offset 4)
	if (std::fseek(fp_, 4, SEEK_SET) != 0)
		return false;
	write_u32_le(fp_, riff_size);

	// Seek to data chunk size (offset 40)
	if (std::fseek(fp_, 40, SEEK_SET) != 0)
		return false;
	write_u32_le(fp_, data_size);

	return true;
}

void WavWriter::close()
{
	if (!fp_)
		return;
	finalize_header();
	std::fclose(fp_);
	fp_ = nullptr;
}

bool WavWriter::repair_header(const std::string &path)
{
	namespace fs = std::filesystem;
	std::error_code ec;
	auto sz = fs::file_size(fs::path(path), ec);
	if (ec || sz < 44)
		return false;
	uint64_t data_bytes = sz - 44;
	uint32_t data_size = (data_bytes > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)data_bytes;
	uint32_t riff_size = 36u + data_size;

	std::FILE *f = std::fopen(path.c_str(), "rb+");
	if (!f)
		return false;
	// RIFF chunk size
	if (std::fseek(f, 4, SEEK_SET) != 0) {
		std::fclose(f);
		return false;
	}
	write_u32_le(f, riff_size);
	// data chunk size
	if (std::fseek(f, 40, SEEK_SET) != 0) {
		std::fclose(f);
		return false;
	}
	write_u32_le(f, data_size);
	std::fclose(f);
	return true;
}

} // namespace stems
