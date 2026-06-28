#include "CollectionStore.h"

#include <Logging.h>

#include <algorithm>
#include <unordered_set>

#include "LibraryIndex.h"

namespace {
constexpr char LOG_TAG[] = "COL";
}  // namespace

CollectionStore CollectionStore::instance;

void CollectionStore::onLoaded() {
  // nextId is derived state: one past the highest collection id seen on disk.
  nextId = 1;
  for (const auto& c : data_.collections) {
    if (c.id >= nextId) {
      nextId = c.id + 1;
    }
  }
  LOG_DBG(LOG_TAG, "collections.json loaded: %d collection(s)", static_cast<int>(data_.collections.size()));
}

const Collection* CollectionStore::findById(uint32_t id) const {
  for (const auto& c : data_.collections) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

uint32_t CollectionStore::createCollection(const std::string& name) {
  if (name.empty()) {
    return 0;
  }

  Collection c;
  c.id = nextId++;
  c.name = name;
  const uint32_t newId = c.id;
  data_.collections.push_back(std::move(c));

  if (!saveToFile()) {
    // Roll back the in-memory add so state matches disk.
    data_.collections.pop_back();
    nextId = newId;
    return 0;
  }
  return newId;
}

void CollectionStore::removeCollection(uint32_t id) {
  auto& collections = data_.collections;

  int removed = std::erase_if(collections, [id](const Collection& c) { return c.id == id; });

  if (removed != 0) {
    saveToFile();
  }
}

void CollectionStore::addBookToCollection(uint32_t bookHash, uint32_t collectionId) {
  auto collectionIt = std::find_if(data_.collections.begin(), data_.collections.end(), [collectionId](const Collection& c) { return c.id == collectionId; });
  if (collectionIt == data_.collections.end()) {
    // collection not found
    return;
  }
  auto& collection = *collectionIt;

  if (collection.hasBook(bookHash)) {
    // book is already in collection; nothing to do
    return;
  }

  collection.members.push_back(bookHash);
  saveToFile();
}

void CollectionStore::removeBookFromCollection(uint32_t bookHash, uint32_t collectionId) {
  auto collectionIt = std::find_if(data_.collections.begin(), data_.collections.end(), [collectionId](const Collection& c) { return c.id == collectionId; });
  if (collectionIt == data_.collections.end()) {
    // collection not found
    return;
  }
  auto& collection = *collectionIt;

  const auto removed = std::erase(collection.members, bookHash);
  if (removed != 0) {
    saveToFile();
  }
}

CollectionStore::LibraryAxes CollectionStore::pruneMissing(const LibraryIndex& index) {
  LibraryAxes axes;
  std::unordered_set<uint32_t> live;
  const int count = index.getBookCount();
  live.reserve(count);
  for (int i = 0; i < count; ++i) {
    const BookView b = index.getAt(i);
    live.insert(b.pathHash);
    axes.hasSeries = axes.hasSeries || !b.series.empty();
    axes.hasAuthor = axes.hasAuthor || !b.author.empty();
    axes.hasGenre = axes.hasGenre || !b.genre.empty();
  }

  size_t pruned = 0;
  for (auto& c : data_.collections) {
    pruned += std::erase_if(c.members, [&live](uint32_t h) { return live.count(h) == 0; });
  }
  if (pruned != 0) {
    LOG_DBG(LOG_TAG, "pruned %d stale member(s)", static_cast<int>(pruned));
    saveToFile();
  }
  return axes;
}
