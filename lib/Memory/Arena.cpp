#include "Arena.h"

#include "Memory.h"  // makeUniqueNoThrow

#include <cstring>

namespace {

// Per-block prefix. 8 bytes keeps the payload that follows 4-byte aligned
// (RISC-V faults on unaligned wide loads; our payloads are byte rasters but
// staying aligned is free and safe). `size` is the payload size in bytes,
// excluding this header. `free` is 1 when the block is available.
struct BlockHeader {
  uint32_t size;
  uint32_t free;
};

constexpr std::size_t kHeader = sizeof(BlockHeader);  // 8
constexpr std::size_t kAlign = 4;
// Don't split off a remainder smaller than this — a sub-header sliver is
// unusable and just complicates the free list.
constexpr std::size_t kMinSplitPayload = 8;

inline std::size_t alignUp(std::size_t n) { return (n + (kAlign - 1)) & ~(kAlign - 1); }

}  // namespace

bool Arena::init(std::size_t capacity) {
  buf_.reset();
  capacity_ = 0;
  used_ = 0;

  // Round so the region holds a whole number of aligned slots.
  const std::size_t cap = alignUp(capacity);
  if (cap <= kHeader) return false;

  auto block = makeUniqueNoThrow<uint8_t[]>(cap);
  if (!block) return false;

  buf_ = std::move(block);
  capacity_ = cap;
  reset();
  return true;
}

void Arena::reset() {
  if (!buf_) return;
  auto* head = reinterpret_cast<BlockHeader*>(buf_.get());
  head->size = static_cast<uint32_t>(capacity_ - kHeader);
  head->free = 1;
  used_ = 0;
}

void* Arena::allocate(std::size_t bytes) {
  if (!buf_ || bytes == 0) return nullptr;
  const std::size_t need = alignUp(bytes);

  std::size_t off = 0;
  while (off < capacity_) {
    auto* h = reinterpret_cast<BlockHeader*>(buf_.get() + off);
    if (h->free && h->size >= need) {
      const std::size_t remainder = h->size - need;
      if (remainder >= kHeader + kMinSplitPayload) {
        // Split: shrink this block to `need`, place a free block after it.
        auto* next = reinterpret_cast<BlockHeader*>(buf_.get() + off + kHeader + need);
        next->size = static_cast<uint32_t>(remainder - kHeader);
        next->free = 1;
        h->size = static_cast<uint32_t>(need);
      }
      h->free = 0;
      used_ += h->size;
      return buf_.get() + off + kHeader;
    }
    off += kHeader + h->size;
  }
  return nullptr;
}

void Arena::deallocate(void* p) {
  if (p == nullptr || !buf_) return;
  auto* h = reinterpret_cast<BlockHeader*>(static_cast<uint8_t*>(p) - kHeader);
  h->free = 1;
  used_ -= h->size;

  // Single left-to-right pass: merge each free block with the free block
  // immediately after it. Extending `cur` and re-checking absorbs runs.
  std::size_t off = 0;
  while (off < capacity_) {
    auto* cur = reinterpret_cast<BlockHeader*>(buf_.get() + off);
    const std::size_t nextOff = off + kHeader + cur->size;
    if (cur->free && nextOff < capacity_) {
      auto* next = reinterpret_cast<BlockHeader*>(buf_.get() + nextOff);
      if (next->free) {
        cur->size += static_cast<uint32_t>(kHeader + next->size);
        continue;  // re-check against the new neighbor
      }
    }
    off = nextOff;
  }
}

std::size_t Arena::largestFree() const {
  if (!buf_) return 0;
  std::size_t best = 0;
  std::size_t off = 0;
  while (off < capacity_) {
    auto* h = reinterpret_cast<BlockHeader*>(buf_.get() + off);
    if (h->free && h->size > best) best = h->size;
    off += kHeader + h->size;
  }
  return best;
}
