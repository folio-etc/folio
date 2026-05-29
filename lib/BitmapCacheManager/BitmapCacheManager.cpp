#include "BitmapCacheManager.h"

#include <utility>

BitmapCacheManager::Entry* BitmapCacheManager::get(const char* path) {
  if (path == nullptr || path[0] == '\0') return nullptr;
  for (auto& slot : slots_) {
    if (!slot.path.empty() && slot.path == path) {
      return &slot;
    }
  }
  return nullptr;
}

BitmapCacheManager::Entry* BitmapCacheManager::set(Entry entry) {
  if (slots_.empty()) return nullptr;

  for (auto& slot : slots_) {
    if (!slot.path.empty() && slot.path == entry.path) {
      slot = std::move(entry);
      return &slot;
    }
  }

  for (auto& slot : slots_) {
    if (slot.path.empty()) {
      slot = std::move(entry);
      return &slot;
    }
  }

  auto& victim = slots_[nextOverwrite_];
  nextOverwrite_ = (nextOverwrite_ + 1) % slots_.size();
  victim = std::move(entry);
  return &victim;
}

void BitmapCacheManager::clear() {
  for (auto& slot : slots_) {
    slot = Entry{};
  }
  nextOverwrite_ = 0;
}
