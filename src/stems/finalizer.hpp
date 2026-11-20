#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace stems {

// One low-priority background thread that performs everything slow after a
// session ends (trim, normalise, FFmpeg export, sidecar JSON, crash repair),
// so none of it can ever block the OBS UI or audio threads.
class Finalizer {
public:
	using Job = std::function<void(const std::atomic<bool> &cancel)>;

	Finalizer() = default;
	~Finalizer();
	Finalizer(const Finalizer &) = delete;
	Finalizer &operator=(const Finalizer &) = delete;

	void start();
	// Cancels the running job (it is expected to bail out quickly and leave
	// valid raw WAV files behind), runs remaining jobs in cancelled mode so
	// they still write metadata, then joins.
	void shutdown();

	void enqueue(Job job);
	size_t pending() const;

private:
	void thread_main();

	mutable std::mutex mtx_;
	std::condition_variable cv_;
	std::condition_variable idle_cv_;
	static constexpr int k_grace_seconds = 10;
	std::deque<Job> jobs_;
	bool stop_ = false;
	bool running_job_ = false;
	std::atomic<bool> cancel_{false};
	std::thread thread_;
};

} // namespace stems
