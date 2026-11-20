#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "finalizer.hpp"
#include "settings.hpp"
#include "stem_recorder.hpp"
#include "timeline.hpp"

namespace stems {

enum class SessionKind {
	Recording,
	Streaming,
	Manual,
};

const char *session_kind_label(SessionKind k);

struct SessionStatus {
	bool running = false;
	bool paused = false;
	size_t stems = 0;
	uint64_t elapsed_ns = 0;
	uint64_t dropped_frames = 0;
	size_t failed_stems = 0;
	std::string session_dir;
};

// One capture session (a recording, a stream, or a manual capture).
// All public methods must be called from the OBS UI thread.
class Session {
public:
	Session(SessionKind kind, const Settings &settings, Finalizer &finalizer);
	~Session();
	Session(const Session &) = delete;
	Session &operator=(const Session &) = delete;

	bool start(const std::string &recording_path = {});
	// Fast: detaches capture, flushes and closes files, then hands the slow
	// work (post-processing / export / metadata) to the Finalizer.
	void stop();

	void pause();
	void resume();
	void add_marker(const std::string &type, const std::string &value);

	SessionKind kind() const { return kind_; }
	bool is_running() const { return running_; }
	SessionStatus status() const;

private:
	struct Marker {
		int64_t frames = 0;
		uint64_t wall_ns = 0;
		std::string type;
		std::string value;
	};

	void writer_main();
	std::string resolve_base_dir(const std::string &recording_path) const;
	void write_inprogress_marker();

	SessionKind kind_;
	Settings settings_;
	Finalizer &finalizer_;

	std::string session_dir_;
	std::string recording_path_;
	std::string started_at_;
	StemFormat fmt_;
	Timeline timeline_;
	std::vector<std::unique_ptr<StemRecorder>> recs_;

	std::thread writer_;
	std::atomic<bool> writer_stop_{false};

	mutable std::mutex markers_mtx_;
	std::vector<Marker> markers_;

	uint64_t start_ns_ = 0;
	uint64_t stop_ns_ = 0;
	bool running_ = false;
};

// Repairs WAV headers of sessions interrupted by a crash (runs on the Finalizer).
void repair_interrupted_sessions(const std::string &base_dir, const std::atomic<bool> &cancel);

} // namespace stems
