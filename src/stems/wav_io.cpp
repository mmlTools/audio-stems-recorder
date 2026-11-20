#include "wav_io.hpp"

#include "paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace stems {

namespace {

constexpr uint64_t k_u32_max = 0xFFFFFFFFull;
constexpr uint64_t k_ds64_slot_offset = 12; // JUNK/ds64 chunk directly after "RIFF....WAVE"
constexpr uint32_t k_ds64_body_size = 28;

constexpr uint16_t k_tag_pcm = 1;
constexpr uint16_t k_tag_float = 3;
constexpr uint16_t k_tag_extensible = 0xFFFE;

void put_u16(std::vector<uint8_t> &b, uint16_t v)
{
	b.push_back((uint8_t)(v & 0xFF));
	b.push_back((uint8_t)(v >> 8));
}

void put_u32(std::vector<uint8_t> &b, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

void put_u64(std::vector<uint8_t> &b, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		b.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

void put_tag(std::vector<uint8_t> &b, const char *tag)
{
	b.insert(b.end(), tag, tag + 4);
}

uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t get_u64(const uint8_t *p)
{
	return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

bool write_at(std::FILE *f, uint64_t offset, const std::vector<uint8_t> &bytes)
{
	if (fseek64(f, (int64_t)offset, SEEK_SET) != 0)
		return false;
	return std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
}

int64_t file_size_of(std::FILE *f)
{
	const int64_t cur = ftell64(f);
	if (fseek64(f, 0, SEEK_END) != 0)
		return -1;
	const int64_t size = ftell64(f);
	fseek64(f, cur, SEEK_SET);
	return size;
}

// Writes RIFF/RF64 size fields. `has_ds64_slot` tells whether a 28 byte
// JUNK/ds64 chunk exists at offset 12 that may be turned into ds64.
bool write_size_fields(std::FILE *f, uint64_t data_offset, uint64_t data_bytes, uint64_t frames,
		       uint64_t riff_total_minus8, bool has_ds64_slot)
{
	const bool needs_rf64 = riff_total_minus8 > k_u32_max || data_bytes > k_u32_max;
	std::vector<uint8_t> b;

	if (needs_rf64 && has_ds64_slot) {
		b.clear();
		put_tag(b, "RF64");
		put_u32(b, 0xFFFFFFFFu);
		if (!write_at(f, 0, b))
			return false;

		b.clear();
		put_tag(b, "ds64");
		put_u32(b, k_ds64_body_size);
		put_u64(b, riff_total_minus8);
		put_u64(b, data_bytes);
		put_u64(b, frames);
		put_u32(b, 0); // table length
		if (!write_at(f, k_ds64_slot_offset, b))
			return false;

		b.clear();
		put_u32(b, 0xFFFFFFFFu);
		return write_at(f, data_offset - 4, b);
	}

	// Classic RIFF (clamped if a legacy header cannot be upgraded).
	b.clear();
	put_tag(b, "RIFF");
	put_u32(b, (uint32_t)std::min<uint64_t>(riff_total_minus8, k_u32_max));
	if (!write_at(f, 0, b))
		return false;

	if (has_ds64_slot) {
		b.clear();
		put_tag(b, "JUNK");
		put_u32(b, k_ds64_body_size);
		b.resize(b.size() + k_ds64_body_size, 0);
		if (!write_at(f, k_ds64_slot_offset, b))
			return false;
	}

	b.clear();
	put_u32(b, (uint32_t)std::min<uint64_t>(data_bytes, k_u32_max));
	return write_at(f, data_offset - 4, b);
}

bool has_ds64_slot(std::FILE *f)
{
	uint8_t hdr[8] = {};
	if (fseek64(f, (int64_t)k_ds64_slot_offset, SEEK_SET) != 0)
		return false;
	if (std::fread(hdr, 1, 8, f) != 8)
		return false;
	const bool tag_ok = std::memcmp(hdr, "JUNK", 4) == 0 || std::memcmp(hdr, "ds64", 4) == 0;
	return tag_ok && get_u32(hdr + 4) == k_ds64_body_size;
}

inline float clamp_unit(float v)
{
	if (!(v == v)) // NaN guard
		return 0.0f;
	return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

} // namespace

SampleFormat sample_format_from_bits(int bits)
{
	switch (bits) {
	case 24:
		return SampleFormat::S24;
	case 32:
		return SampleFormat::F32;
	default:
		return SampleFormat::S16;
	}
}

int sample_format_bits(SampleFormat f)
{
	switch (f) {
	case SampleFormat::S24:
		return 24;
	case SampleFormat::F32:
		return 32;
	default:
		return 16;
	}
}

const char *sample_format_name(SampleFormat f)
{
	switch (f) {
	case SampleFormat::S24:
		return "pcm_s24le";
	case SampleFormat::F32:
		return "pcm_f32le";
	default:
		return "pcm_s16le";
	}
}

uint32_t default_channel_mask(uint16_t channels)
{
	// Matches libobs speaker layouts.
	switch (channels) {
	case 1:
		return 0x4; // FC
	case 2:
		return 0x3; // FL FR
	case 3:
		return 0xB; // FL FR LFE
	case 4:
		return 0x107; // FL FR FC BC
	case 5:
		return 0x10F; // FL FR FC LFE BC
	case 6:
		return 0x3F; // FL FR FC LFE BL BR
	case 8:
		return 0x63F; // FL FR FC LFE BL BR SL SR
	default:
		return 0;
	}
}

SampleFormat WavInfo::sample_format() const
{
	if (is_float)
		return SampleFormat::F32;
	return bits == 24 ? SampleFormat::S24 : SampleFormat::S16;
}

// ---------------------------------------------------------------------------
// WavWriter
// ---------------------------------------------------------------------------

WavWriter::~WavWriter()
{
	close();
}

bool WavWriter::open(const std::string &path, uint32_t sample_rate, uint16_t channels, SampleFormat fmt,
		     uint32_t channel_mask)
{
	close();
	path_ = path;
	sample_rate_ = sample_rate ? sample_rate : 48000;
	channels_ = channels ? channels : 2;
	fmt_ = fmt;
	block_align_ = (uint32_t)channels_ * (uint32_t)(sample_format_bits(fmt_) / 8);
	frames_written_ = 0;
	failed_ = false;

	fp_ = fopen_u8(path, "wb");
	if (!fp_)
		return false;
	// Large stdio buffer: fewer syscalls, the writer thread hands us ~10-20 ms
	// of audio per call.
	std::setvbuf(fp_, nullptr, _IOFBF, 256 * 1024);

	if (!write_header(channel_mask ? channel_mask : default_channel_mask(channels_))) {
		std::fclose(fp_);
		fp_ = nullptr;
		return false;
	}
	return true;
}

bool WavWriter::write_header(uint32_t channel_mask)
{
	const uint16_t bits = (uint16_t)sample_format_bits(fmt_);
	const bool is_float = fmt_ == SampleFormat::F32;
	const bool extensible = channels_ > 2;
	const uint16_t tag = extensible ? k_tag_extensible : (is_float ? k_tag_float : k_tag_pcm);

	std::vector<uint8_t> fmt;
	put_u16(fmt, tag);
	put_u16(fmt, channels_);
	put_u32(fmt, sample_rate_);
	put_u32(fmt, sample_rate_ * block_align_);
	put_u16(fmt, (uint16_t)block_align_);
	put_u16(fmt, bits);
	if (extensible) {
		put_u16(fmt, 22);
		put_u16(fmt, bits);
		put_u32(fmt, channel_mask);
		put_u32(fmt, is_float ? k_tag_float : k_tag_pcm);
		put_u16(fmt, 0x0000);
		put_u16(fmt, 0x0010);
		const uint8_t tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
		fmt.insert(fmt.end(), tail, tail + 8);
	} else if (is_float) {
		put_u16(fmt, 0);
	}

	std::vector<uint8_t> h;
	put_tag(h, "RIFF");
	put_u32(h, 0);
	put_tag(h, "WAVE");
	put_tag(h, "JUNK");
	put_u32(h, k_ds64_body_size);
	h.resize(h.size() + k_ds64_body_size, 0);
	put_tag(h, "fmt ");
	put_u32(h, (uint32_t)fmt.size());
	h.insert(h.end(), fmt.begin(), fmt.end());
	put_tag(h, "data");
	put_u32(h, 0);

	data_offset_ = h.size();
	data_size_field_offset_ = data_offset_ - 4;
	if (!write_raw(h.data(), h.size()))
		return false;
	// Make the empty-but-valid header visible immediately.
	return write_sizes(false);
}

bool WavWriter::write_raw(const void *data, size_t bytes)
{
	if (!fp_ || failed_)
		return false;
	if (bytes && std::fwrite(data, 1, bytes, fp_) != bytes) {
		failed_ = true;
		return false;
	}
	return true;
}

bool WavWriter::write(const float *in, size_t frames)
{
	if (!fp_ || failed_)
		return false;
	if (!in || frames == 0)
		return true;

	const size_t samples = frames * channels_;
	scratch_.resize(frames * block_align_);
	uint8_t *o = scratch_.data();

	switch (fmt_) {
	case SampleFormat::S16:
		for (size_t i = 0; i < samples; i++) {
			const int32_t v = (int32_t)std::lrintf(clamp_unit(in[i]) * 32767.0f);
			o[0] = (uint8_t)(v & 0xFF);
			o[1] = (uint8_t)((v >> 8) & 0xFF);
			o += 2;
		}
		break;
	case SampleFormat::S24:
		for (size_t i = 0; i < samples; i++) {
			const int32_t v = (int32_t)std::lrint((double)clamp_unit(in[i]) * 8388607.0);
			o[0] = (uint8_t)(v & 0xFF);
			o[1] = (uint8_t)((v >> 8) & 0xFF);
			o[2] = (uint8_t)((v >> 16) & 0xFF);
			o += 3;
		}
		break;
	case SampleFormat::F32:
		for (size_t i = 0; i < samples; i++) {
			float v = in[i];
			if (!(v == v))
				v = 0.0f;
			std::memcpy(o, &v, 4); // all supported targets are little-endian
			o += 4;
		}
		break;
	}

	if (!write_raw(scratch_.data(), scratch_.size()))
		return false;
	frames_written_ += frames;
	return true;
}

bool WavWriter::write_silence(uint64_t frames)
{
	if (!fp_ || failed_)
		return false;
	constexpr size_t k_block = 4096;
	scratch_.assign(k_block * block_align_, 0);
	while (frames > 0) {
		const size_t n = (size_t)std::min<uint64_t>(frames, k_block);
		if (!write_raw(scratch_.data(), n * block_align_))
			return false;
		frames_written_ += n;
		frames -= n;
	}
	return true;
}

bool WavWriter::write_sizes(bool final)
{
	if (!fp_)
		return false;
	const int64_t end = ftell64(fp_);
	if (end < 0)
		return false;

	const uint64_t data_bytes = frames_written_ * block_align_;
	uint64_t total = data_offset_ + data_bytes;
	if (final && (data_bytes & 1))
		total += 1; // pad byte written by close()

	bool ok = write_size_fields(fp_, data_offset_, data_bytes, frames_written_, total - 8, true);
	ok = (fseek64(fp_, end, SEEK_SET) == 0) && ok;
	ok = (std::fflush(fp_) == 0) && ok;
	return ok;
}

bool WavWriter::refresh_header()
{
	if (!fp_ || failed_)
		return false;
	return write_sizes(false);
}

bool WavWriter::close()
{
	if (!fp_)
		return true;
	bool ok = !failed_;
	const uint64_t data_bytes = frames_written_ * block_align_;
	if (data_bytes & 1) {
		const uint8_t pad = 0;
		std::fwrite(&pad, 1, 1, fp_);
	}
	ok = write_sizes(true) && ok;
	ok = (std::fclose(fp_) == 0) && ok;
	fp_ = nullptr;
	scratch_.clear();
	scratch_.shrink_to_fit();
	return ok;
}

// ---------------------------------------------------------------------------
// Header parsing / reader
// ---------------------------------------------------------------------------

bool read_wav_info(std::FILE *f, uint64_t file_size, WavInfo &out, bool tolerate_unfinished)
{
	out = WavInfo{};
	uint8_t hdr[12];
	if (fseek64(f, 0, SEEK_SET) != 0 || std::fread(hdr, 1, 12, f) != 12)
		return false;
	const bool rf64 = std::memcmp(hdr, "RF64", 4) == 0;
	if ((!rf64 && std::memcmp(hdr, "RIFF", 4) != 0) || std::memcmp(hdr + 8, "WAVE", 4) != 0)
		return false;

	uint64_t ds64_data = 0;
	bool have_fmt = false;
	uint16_t tag = 0;
	uint16_t block_align = 0;
	uint64_t pos = 12;

	while (pos + 8 <= file_size) {
		uint8_t ch[8];
		if (fseek64(f, (int64_t)pos, SEEK_SET) != 0 || std::fread(ch, 1, 8, f) != 8)
			return false;
		const uint32_t size = get_u32(ch + 4);
		const uint64_t body = pos + 8;

		if (std::memcmp(ch, "ds64", 4) == 0 && size >= 24) {
			uint8_t d[24];
			if (std::fread(d, 1, 24, f) != 24)
				return false;
			ds64_data = get_u64(d + 8);
		} else if (std::memcmp(ch, "fmt ", 4) == 0 && size >= 16) {
			uint8_t d[40] = {};
			const size_t n = std::min<size_t>(size, sizeof(d));
			if (std::fread(d, 1, n, f) != n)
				return false;
			tag = get_u16(d);
			out.channels = get_u16(d + 2);
			out.sample_rate = get_u32(d + 4);
			block_align = get_u16(d + 12);
			out.bits = get_u16(d + 14);
			if (tag == k_tag_extensible && n >= 40) {
				out.channel_mask = get_u32(d + 20);
				tag = (uint16_t)get_u32(d + 24);
			}
			have_fmt = true;
		} else if (std::memcmp(ch, "data", 4) == 0) {
			if (!have_fmt)
				return false;
			out.data_offset = body;
			uint64_t bytes = size;
			if (rf64 && size == 0xFFFFFFFFu)
				bytes = ds64_data;
			const uint64_t avail = file_size > body ? file_size - body : 0;
			if (tolerate_unfinished && (bytes == 0 || size == 0xFFFFFFFFu || bytes < avail))
				bytes = avail;
			if (bytes > avail)
				bytes = avail; // truncated file
			out.data_bytes = bytes;
			break;
		}
		pos = body + size + (size & 1);
	}

	if (!have_fmt || out.data_offset == 0 || out.channels == 0 || out.sample_rate == 0)
		return false;
	out.is_float = tag == k_tag_float;
	if (out.is_float ? out.bits != 32 : (tag != k_tag_pcm || (out.bits != 16 && out.bits != 24 && out.bits != 32)))
		return false;
	const uint32_t expected_align = (uint32_t)out.channels * (out.bits / 8u);
	if (block_align != expected_align)
		return false;
	out.data_bytes -= out.data_bytes % expected_align;
	out.frames = out.data_bytes / expected_align;
	return true;
}

WavReader::~WavReader()
{
	close();
}

bool WavReader::open(const std::string &path)
{
	close();
	fp_ = fopen_u8(path, "rb");
	if (!fp_)
		return false;
	std::setvbuf(fp_, nullptr, _IOFBF, 256 * 1024);
	const int64_t size = file_size_of(fp_);
	if (size < 0 || !read_wav_info(fp_, (uint64_t)size, info_, false)) {
		close();
		return false;
	}
	return seek_frame(0);
}

void WavReader::close()
{
	if (fp_) {
		std::fclose(fp_);
		fp_ = nullptr;
	}
	scratch_.clear();
	scratch_.shrink_to_fit();
}

bool WavReader::seek_frame(uint64_t frame)
{
	if (!fp_)
		return false;
	if (frame > info_.frames)
		frame = info_.frames;
	const uint64_t align = (uint64_t)info_.channels * (info_.bits / 8u);
	if (fseek64(fp_, (int64_t)(info_.data_offset + frame * align), SEEK_SET) != 0)
		return false;
	pos_frames_ = frame;
	return true;
}

size_t WavReader::read(float *out, size_t frames)
{
	if (!fp_ || pos_frames_ >= info_.frames)
		return 0;
	frames = (size_t)std::min<uint64_t>(frames, info_.frames - pos_frames_);
	const size_t bps = info_.bits / 8u;
	const size_t align = info_.channels * bps;
	scratch_.resize(frames * align);
	const size_t got = std::fread(scratch_.data(), align, frames, fp_);
	const size_t samples = got * info_.channels;
	const uint8_t *p = scratch_.data();

	if (info_.is_float) {
		std::memcpy(out, p, samples * 4);
	} else if (bps == 2) {
		for (size_t i = 0; i < samples; i++, p += 2)
			out[i] = (float)(int16_t)get_u16(p) / 32768.0f;
	} else if (bps == 3) {
		for (size_t i = 0; i < samples; i++, p += 3) {
			int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
			out[i] = (float)v / 8388608.0f;
		}
	} else {
		for (size_t i = 0; i < samples; i++, p += 4)
			out[i] = (float)((double)(int32_t)get_u32(p) / 2147483648.0);
	}
	pos_frames_ += got;
	return got;
}

bool repair_wav_file(const std::string &path)
{
	std::FILE *f = fopen_u8(path, "r+b");
	if (!f)
		return false;
	bool ok = false;
	const int64_t size = file_size_of(f);
	WavInfo info;
	if (size > 0 && read_wav_info(f, (uint64_t)size, info, true)) {
		const bool slot = has_ds64_slot(f);
		const uint64_t total = (uint64_t)size;
		ok = write_size_fields(f, info.data_offset, info.data_bytes, info.frames, total - 8, slot);
	}
	ok = (std::fclose(f) == 0) && ok;
	return ok;
}

} // namespace stems
