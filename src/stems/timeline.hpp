#pragma once

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace stems {

// Maps OBS audio timestamps (os_gettime_ns clock) onto a session timeline in
// sample frames, excluding time spent paused. Shared by every stem in a
// session so all stems stay sample-aligned with each other.
class Timeline {
public:
	struct Pos {
		bool valid = false;
		bool paused = false;
		int64_t frames = 0;
	};

	void start(uint64_t anchor_ns, uint32_t sample_rate)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		anchor_ns_ = anchor_ns;
		rate_ = sample_rate;
		pauses_.clear();
	}

	bool pause(uint64_t ns)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (!pauses_.empty() && pauses_.back().second == k_open)
			return false;
		pauses_.emplace_back(ns, k_open);
		return true;
	}

	bool resume(uint64_t ns)
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (pauses_.empty() || pauses_.back().second != k_open)
			return false;
		pauses_.back().second = ns > pauses_.back().first ? ns : pauses_.back().first;
		return true;
	}

	bool is_paused() const
	{
		std::lock_guard<std::mutex> lock(mtx_);
		return !pauses_.empty() && pauses_.back().second == k_open;
	}

	uint32_t sample_rate() const { return rate_; }
	uint64_t anchor_ns() const { return anchor_ns_; }

	Pos map(uint64_t ts) const
	{
		Pos p;
		if (ts == 0)
			return p;
		std::lock_guard<std::mutex> lock(mtx_);
		uint64_t paused_before = 0;
		for (const auto &iv : pauses_) {
			if (ts >= iv.first && ts < iv.second) {
				p.valid = true;
				p.paused = true;
				return p;
			}
			if (ts >= iv.second)
				paused_before += iv.second - iv.first;
		}
		const int64_t rel_ns = (int64_t)ts - (int64_t)anchor_ns_ - (int64_t)paused_before;
		// microsecond precision keeps the multiplication far from overflow
		p.frames = (rel_ns / 1000) * (int64_t)rate_ / 1000000;
		p.valid = true;
		return p;
	}

	// Elapsed session time in nanoseconds, excluding pauses.
	uint64_t active_ns(uint64_t now) const
	{
		std::lock_guard<std::mutex> lock(mtx_);
		if (now <= anchor_ns_)
			return 0;
		uint64_t paused = 0;
		for (const auto &iv : pauses_) {
			const uint64_t end = iv.second == k_open ? now : iv.second;
			if (end > iv.first && now > iv.first)
				paused += (end < now ? end : now) - iv.first;
		}
		const uint64_t total = now - anchor_ns_;
		return total > paused ? total - paused : 0;
	}

private:
	static constexpr uint64_t k_open = ~0ull;
	mutable std::mutex mtx_;
	uint64_t anchor_ns_ = 0;
	uint32_t rate_ = 48000;
	std::vector<std::pair<uint64_t, uint64_t>> pauses_;
};

} // namespace stems
