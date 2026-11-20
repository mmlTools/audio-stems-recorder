#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace stems {

// Wait-free single-producer / single-consumer ring buffer.
//
// The producer is the OBS audio thread (or a source's own audio thread, but
// OBS serialises capture callbacks per source), the consumer is the session's
// disk writer thread. The producer never blocks, never allocates and never
// takes a lock, so a slow disk can never stall OBS audio: when the ring is full
// the producer simply reports failure and the caller counts a drop.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // padding from alignas: intentional (avoids false sharing)
#endif
template<typename T> class SpscRing {
public:
	SpscRing() = default;
	SpscRing(const SpscRing &) = delete;
	SpscRing &operator=(const SpscRing &) = delete;

	// Not thread safe; call before the ring is shared.
	void init(size_t min_capacity)
	{
		size_t cap = 1;
		while (cap < min_capacity)
			cap <<= 1;
		buf_.reset(new T[cap]());
		cap_ = cap;
		mask_ = cap - 1;
		head_.store(0, std::memory_order_relaxed);
		tail_.store(0, std::memory_order_relaxed);
	}

	size_t capacity() const { return cap_; }

	// ---- producer side ----
	size_t write_available() const
	{
		const size_t h = head_.load(std::memory_order_relaxed);
		const size_t t = tail_.load(std::memory_order_acquire);
		return cap_ - (h - t);
	}

	// Direct slot access for in-place writes: fetch write_pos() once, fill
	// at(write_pos() + i), then commit_write().
	size_t write_pos() const { return head_.load(std::memory_order_relaxed); }
	T &at(size_t abs_index) { return buf_[abs_index & mask_]; }
	T &write_slot(size_t offset) { return at(write_pos() + offset); }

	void commit_write(size_t count) { head_.store(head_.load(std::memory_order_relaxed) + count, std::memory_order_release); }

	bool push(const T &v)
	{
		if (write_available() < 1)
			return false;
		write_slot(0) = v;
		commit_write(1);
		return true;
	}

	// ---- consumer side ----
	size_t read_available() const
	{
		const size_t t = tail_.load(std::memory_order_relaxed);
		const size_t h = head_.load(std::memory_order_acquire);
		return h - t;
	}

	const T &read_slot(size_t offset) const { return buf_[(tail_.load(std::memory_order_relaxed) + offset) & mask_]; }

	void commit_read(size_t count) { tail_.store(tail_.load(std::memory_order_relaxed) + count, std::memory_order_release); }

	// Copies `count` items into dst (caller ensures read_available() >= count).
	void read(T *dst, size_t count)
	{
		const size_t t = tail_.load(std::memory_order_relaxed);
		const size_t first = t & mask_;
		const size_t n1 = (count < cap_ - first) ? count : cap_ - first;
		for (size_t i = 0; i < n1; i++)
			dst[i] = buf_[first + i];
		for (size_t i = n1; i < count; i++)
			dst[i] = buf_[i - n1];
		commit_read(count);
	}

private:
	std::unique_ptr<T[]> buf_;
	size_t cap_ = 0;
	size_t mask_ = 0;
	alignas(64) std::atomic<size_t> head_{0};
	alignas(64) std::atomic<size_t> tail_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace stems
