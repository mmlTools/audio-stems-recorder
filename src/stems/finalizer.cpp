#include "finalizer.hpp"

#include <obs-module.h>
#include <util/threading.h>

#include <chrono>
#include <exception>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace stems {

Finalizer::~Finalizer()
{
	shutdown();
}

void Finalizer::start()
{
	std::lock_guard<std::mutex> lock(mtx_);
	if (thread_.joinable())
		return;
	stop_ = false;
	cancel_.store(false);
	thread_ = std::thread(&Finalizer::thread_main, this);
}

void Finalizer::shutdown()
{
	{
		std::unique_lock<std::mutex> lock(mtx_);
		if (!thread_.joinable())
			return;
		stop_ = true;
		cv_.notify_all();
		// Give pending work a short grace period, then cancel: the remaining
		// jobs still run in cancelled mode (raw WAV kept, metadata written).
		const bool idle = idle_cv_.wait_for(lock, std::chrono::seconds(k_grace_seconds),
						    [this] { return jobs_.empty() && !running_job_; });
		if (!idle) {
			blog(LOG_INFO, "[Audio Stems] OBS is closing: cancelling stem post-processing "
				       "(raw WAV files are kept)");
			cancel_.store(true);
		}
	}
	cv_.notify_all();
	thread_.join();
}

void Finalizer::enqueue(Job job)
{
	{
		std::lock_guard<std::mutex> lock(mtx_);
		jobs_.push_back(std::move(job));
	}
	cv_.notify_one();
}

size_t Finalizer::pending() const
{
	std::lock_guard<std::mutex> lock(mtx_);
	return jobs_.size() + (running_job_ ? 1 : 0);
}

void Finalizer::thread_main()
{
	os_set_thread_name("audio-stems-finalizer");
#if defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif

	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mtx_);
			cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
			if (jobs_.empty())
				return; // stop requested and nothing left
			job = std::move(jobs_.front());
			jobs_.pop_front();
			running_job_ = true;
		}

		try {
			job(cancel_);
		} catch (const std::exception &e) {
			blog(LOG_ERROR, "[Audio Stems] post-processing error: %s", e.what());
		} catch (...) {
			blog(LOG_ERROR, "[Audio Stems] unknown post-processing error");
		}

		{
			std::lock_guard<std::mutex> lock(mtx_);
			running_job_ = false;
		}
		idle_cv_.notify_all();
	}
}

} // namespace stems
