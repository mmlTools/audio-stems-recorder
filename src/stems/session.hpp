#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <vector>

#include <obs-module.h>

#include "settings.hpp"
#include "stem_recorder.hpp"

namespace stems {

enum class SessionKind {
	Recording,
	Streaming,
};

struct StemOutput {
	std::unique_ptr<StemRecorder> recorder;
	std::string wav_path;
	std::string source_uuid;
	std::string source_name;
};

class Session {
public:
	Session(SessionKind kind, const Settings &settings);
	~Session();

	bool start();
	void stop();

	void on_scene_changed(const std::string &scene_name);

	SessionKind kind() const { return kind_; }
	bool is_running() const { return running_; }

private:
	void write_sidecar_json(const std::vector<StemOutput> &finished) const;
	void postprocess_stems(const std::vector<StemOutput> &finished);
	void mark_inprogress(bool inprogress);
	SessionKind kind_;
	Settings settings_;
	std::string session_dir_;
	std::vector<StemOutput> stems_;
	uint32_t sample_rate_ = 48000;
	uint16_t channels_ = 2;
	uint64_t start_ns_ = 0;

	struct Marker {
		uint64_t offset_ns = 0;
		std::string type;
		std::string value;
	};
	std::vector<Marker> markers_;
	bool running_ = false;
};

} // namespace stems
