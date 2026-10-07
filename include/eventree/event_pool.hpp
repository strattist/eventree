#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <memory>
#include <vector>

namespace eventree {

// Append-only storage that grows in fixed-size chunks. Growing adds a chunk and never moves existing
// elements; `reset` forgets the elements but keeps every chunk for reuse.
//
// One pool serves one stream and is single-threaded: no reader may run while a writer appends.
template <class T>
class EventPool {
 public:
  static constexpr std::size_t default_chunk_size = 1 << 16;

  // `chunk_size` is rounded up to a power of two.
  explicit EventPool(std::size_t chunk_size = default_chunk_size)
      : shift_(std::countr_zero(std::bit_ceil(chunk_size ? chunk_size : 1))), chunk_size_(std::size_t{1} << shift_) {}

  void push_back(const T& value) {
    if (free_ == 0) [[unlikely]] enter_next_chunk();
    *cursor_++ = value;
    --free_;
    ++size_;
  }

  T& operator[](std::size_t i) {
    assert(i < size_);
    return chunks_[i >> shift_][i & (chunk_size_ - 1)];
  }
  const T& operator[](std::size_t i) const {
    assert(i < size_);
    return chunks_[i >> shift_][i & (chunk_size_ - 1)];
  }

  T& back() { return (*this)[size_ - 1]; }
  const T& back() const { return (*this)[size_ - 1]; }

  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // Allocates enough chunks to hold `count` elements in total, so appending up to that many allocates nothing.
  void reserve(std::size_t count) {
    const std::size_t needed = (count + chunk_size_ - 1) >> shift_;
    if (needed <= chunks_.size()) return;
    chunks_.reserve(needed);
    while (chunks_.size() < needed) chunks_.push_back(std::make_unique_for_overwrite<T[]>(chunk_size_));
  }

  void reset() {
    size_ = 0;
    cursor_ = nullptr;
    free_ = 0;
  }

  // Bytes held by the pool, including chunks that are allocated but not yet filled.
  std::size_t memory_use() const {
    return chunks_.size() * chunk_size_ * sizeof(T) + chunks_.capacity() * sizeof(std::unique_ptr<T[]>);
  }

 private:
  // Points the write cursor at the chunk holding element `size_`, allocating it if it does not exist yet.
  void enter_next_chunk() {
    const std::size_t index = size_ >> shift_;
    if (index == chunks_.size()) chunks_.push_back(std::make_unique_for_overwrite<T[]>(chunk_size_));
    cursor_ = chunks_[index].get();
    free_ = chunk_size_;
  }

  unsigned shift_;
  std::size_t chunk_size_;
  std::size_t size_ = 0;
  T* cursor_ = nullptr;      // next slot to write in the current chunk
  std::size_t free_ = 0;     // slots left in the current chunk; 0 sends the next push to `enter_next_chunk`
  std::vector<std::unique_ptr<T[]>> chunks_;
};

}  // namespace eventree
