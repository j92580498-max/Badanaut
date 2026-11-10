#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// The SDL audio-device lock protects this queue. Storage is allocated during
// Prepare, never from the real-time callback or WriteBuffer. End markers retain
// one completion per guest buffer even when samples wrap around the ring.
class PcmRing {
  public:
    void prepare(size_t frames, int channels) {
        channels_ = channels;
        samples_.resize(frames * channels);
        ends_.resize(frames);
        clear();
    }
    void clear() { head_ = queued_ = 0; }
    bool empty() const { return !queued_; }
    size_t size() const { return queued_; }
    size_t capacity() const { return ends_.size(); }

    template<class Copy>
    bool append(size_t frames, Copy copy) {
        if (!frames || frames > capacity() - queued_) return false;
        const size_t tail = (head_ + queued_) % capacity();
        const size_t first = std::min(frames, capacity() - tail);
        copy(samples_.data() + tail * channels_, 0, first * channels_);
        if (first < frames) copy(samples_.data(), first * channels_, (frames - first) * channels_);
        std::memset(ends_.data() + tail, 0, first);
        if (first < frames) std::memset(ends_.data(), 0, frames - first);
        ends_[(tail + frames - 1) % capacity()] = 1;
        queued_ += frames;
        return true;
    }
    int16_t sample(int channel, bool next = false) const {
        const size_t frame = (head_ + (next && queued_ > 1 ? 1 : 0)) % capacity();
        return samples_[frame * channels_ + (channels_ == 1 ? 0 : channel)];
    }
    bool consume() {
        if (!queued_) return false;
        const bool completed = ends_[head_] != 0;
        head_ = (head_ + 1) % capacity();
        --queued_;
        return completed;
    }
  private:
    std::vector<int16_t> samples_;
    std::vector<uint8_t> ends_;
    size_t head_ = 0, queued_ = 0;
    int channels_ = 1;
};
