#pragma once

#include <string>
#include <utility>
#include <vector>

namespace stems {

struct Settings {
	bool trigger_recording = true;
	bool trigger_streaming = true;
	std::string output_dir;

	// Post-processing
	bool trim_silence = true;
	float trim_threshold_dbfs = -45.0f; // dBFS
	int trim_lead_ms = 150;            // keep a bit before first audio
	int trim_trail_ms = 350;           // keep a bit after last audio

	bool normalize_audio = true;
	float normalize_target_dbfs = -16.0f; // RMS target (practical editing default)
	bool normalize_limiter = true;        // prevent clipping

	// Metadata
	bool write_sidecar_json = true;
	bool record_scene_markers = true;

	// Naming
	bool use_source_aliases = false;
	// Stored as pairs: uuid, alias
	std::vector<std::pair<std::string, std::string>> source_aliases;

	// Stable source identifiers (UUIDs)
	std::vector<std::string> selected_source_uuids;
};

Settings load_settings();
void save_settings(const Settings &s);

} // namespace stems
