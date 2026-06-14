#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Fixed-block free-list allocator. Reserves one contiguous heap block up front
// and sub-allocates variable-sized chunks from it, so a churning working set
// (e.g. the library cover cache) never fragments the GENERAL heap — all of its
// alloc/free traffic stays inside this region, which is released as a single
// block when the Arena is destroyed.
//
// Layout: blocks sit contiguously, each prefixed by an 8-byte header (payload
// size + free flag). allocate() is first-fit with splitting; deallocate()
// marks free and coalesces adjacent free blocks in a single left-to-right pass.
// Block count is small (tens), so the O(n) walks are negligible.
//
// NOT internally synchronized. The caller must serialize all allocate /
// deallocate / reset calls (the library cache does so under its cache mutex).
class Arena {
 public:
  Arena() = default;

  // Reserve `capacity` bytes from the heap. Returns false on OOM. Safe to call
  // once; a second call frees the previous region first.
  bool init(std::size_t capacity);

  // Carve `bytes` (4-byte aligned) from the region. Returns nullptr when no
  // free block fits — callers degrade gracefully (skip the cached item).
  void* allocate(std::size_t bytes);

  // Return a pointer previously handed out by allocate() to the free list and
  // coalesce neighbors. No-op for nullptr.
  void deallocate(void* p);

  // Drop every allocation at once (single free block spanning the region).
  void reset();

  std::size_t capacity() const { return capacity_; }
  std::size_t used() const { return used_; }
  std::size_t largestFree() const;

 private:
  std::unique_ptr<uint8_t[]> buf_;
  std::size_t capacity_ = 0;
  std::size_t used_ = 0;  // sum of payload bytes currently handed out
};
