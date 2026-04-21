#pragma once

#include <string>
#include <utility>
#include <vector>

namespace stems {

struct Settings {
	bool trigger_recording = true;
	bool trigger_streaming = true;
	std::string output_dir;
	std::string output_format = "wav";
	int wav_bit_depth = 16;

	bool trim_silence = true;
	float trim_threshold_dbfs = -45.0f; 
	int trim_lead_ms = 150;            
	int trim_trail_ms = 350;           

	bool normalize_audio = true;
	float normalize_target_dbfs = -16.0f; 
	bool normalize_limiter = true;        

	bool write_sidecar_json = true;
	bool record_scene_markers = true;

	bool use_source_aliases = false;
	
	std::vector<std::pair<std::string, std::string>> source_aliases;

	std::vector<std::string> selected_source_uuids;
};

Settings load_settings();
void save_settings(const Settings &s);

} 
