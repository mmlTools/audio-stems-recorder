#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace stems {

struct SelectedSource {
	std::string uuid;
	std::string name; // fallback match when UUIDs change (other scene collection / machine)
};

struct Settings {
	// Triggers
	bool trigger_recording = true;
	bool trigger_streaming = true;

	// Location
	bool save_next_to_recordings = false; // use OBS' recording folder instead of output_dir
	std::string output_dir;

	// Format
	std::string output_format = "wav"; // wav | flac | mp3
	int wav_bit_depth = 24;            // 16, 24, 32 (= 32-bit float)
	int mp3_bitrate_kbps = 320;
	bool keep_wav_after_export = false;
	std::string ffmpeg_path; // optional; PATH is searched otherwise

	// What to capture
	std::vector<SelectedSource> selected_sources;
	bool post_fader = false;   // apply each source's OBS volume fader / mute
	uint32_t track_mask = 0;   // bit i => record OBS mixer track i+1

	// File names
	bool use_source_aliases = false;
	std::vector<std::pair<std::string, std::string>> source_aliases; // uuid -> alias

	// Post-processing (both off by default: stems stay sample-aligned with the video)
	bool trim_silence = false;
	float trim_threshold_dbfs = -45.0f;
	int trim_lead_ms = 150;
	int trim_trail_ms = 350;

	bool normalize_audio = false;
	float normalize_target_dbfs = -16.0f;
	bool normalize_limiter = true;

	// Metadata
	bool write_sidecar_json = true;
	bool record_scene_markers = true;
};

Settings load_settings();
void save_settings(const Settings &s);
std::string default_output_dir();

} // namespace stems
