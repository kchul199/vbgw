#pragma once
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstring>

class RingBuffer {
public:
    RingBuffer(size_t size) : buffer_(size), head_(0), tail_(0), count_(0) {}

    void write(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < len; ++i) {
            if (count_ == buffer_.size()) {
                // 버퍼 오버플로우 시 가장 오래된 데이터 버림
                head_ = (head_ + 1) % buffer_.size();
                count_--;
            }
            buffer_[tail_] = data[i];
            tail_ = (tail_ + 1) % buffer_.size();
            count_++;
        }
    }

    size_t read(uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t read_bytes = 0;
        for (size_t i = 0; i < len; ++i) {
            if (count_ == 0) break; // 언더플로우 방지
            data[i] = buffer_[head_];
            head_ = (head_ + 1) % buffer_.size();
            count_--;
            read_bytes++;
        }
        return read_bytes;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

private:
    std::vector<uint8_t> buffer_;
    size_t head_;
    size_t tail_;
    size_t count_;
    std::mutex mutex_;
};
