#include "CollectionStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

namespace {
constexpr char LOG_TAG[] = "COL";

constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr char COLLECTIONS_FILE[] = "/.crosspoint/collections.bin";
constexpr char COLLECTIONS_FILE_TMP[] = "/.crosspoint/collections.bin.tmp";

// 'COLL' in little-endian byte order on the wire.
constexpr uint32_t COLLECTIONS_FILE_MAGIC = 0x4C4C4F43u;
constexpr uint8_t COLLECTIONS_FILE_VERSION = 1;
}  // namespace

CollectionStore CollectionStore::instance;

bool CollectionStore::loadFromFile() {
  collections.clear();
  nextId = 1;
  loaded = true;  // an absent file is a valid (empty) state

  if (!Storage.exists(COLLECTIONS_FILE)) {
    return false;
  }

  FsFile f;
  if (!Storage.openFileForRead(LOG_TAG, COLLECTIONS_FILE, f)) {
    LOG_ERR(LOG_TAG, "Cannot open %s for read", COLLECTIONS_FILE);
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t reserved[3] = {0, 0, 0};
  uint32_t count = 0;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  serialization::readPod(f, reserved);
  serialization::readPod(f, count);

  if (magic != COLLECTIONS_FILE_MAGIC) {
    LOG_ERR(LOG_TAG, "collections.bin: bad magic 0x%08X", magic);
    return false;
  }
  if (version != COLLECTIONS_FILE_VERSION) {
    LOG_DBG(LOG_TAG, "collections.bin: version %u, ignoring", version);
    return false;
  }

  collections.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Collection c;
    serialization::readPod(f, c.id);
    serialization::readString(f, c.name);
    uint32_t memberCount = 0;
    serialization::readPod(f, memberCount);
    c.memberPathHashes.reserve(memberCount);
    for (uint32_t j = 0; j < memberCount; ++j) {
      uint32_t hash = 0;
      serialization::readPod(f, hash);
      c.memberPathHashes.push_back(hash);
    }
    if (c.id >= nextId) {
      nextId = c.id + 1;
    }
    collections.push_back(std::move(c));
  }

  LOG_DBG(LOG_TAG, "collections.bin loaded: %d collections", static_cast<int>(collections.size()));
  return true;
}

bool CollectionStore::saveToFile() const {
  Storage.mkdir(CACHE_DIR);

  FsFile f;
  if (!Storage.openFileForWrite(LOG_TAG, COLLECTIONS_FILE_TMP, f)) {
    LOG_ERR(LOG_TAG, "Cannot open %s for write", COLLECTIONS_FILE_TMP);
    return false;
  }

  const uint32_t magic = COLLECTIONS_FILE_MAGIC;
  const uint8_t version = COLLECTIONS_FILE_VERSION;
  const uint8_t reserved[3] = {0, 0, 0};
  const uint32_t count = static_cast<uint32_t>(collections.size());
  serialization::writePod(f, magic);
  serialization::writePod(f, version);
  serialization::writePod(f, reserved);
  serialization::writePod(f, count);

  for (const auto& c : collections) {
    serialization::writePod(f, c.id);
    serialization::writeString(f, c.name);
    const uint32_t memberCount = static_cast<uint32_t>(c.memberPathHashes.size());
    serialization::writePod(f, memberCount);
    for (uint32_t hash : c.memberPathHashes) {
      serialization::writePod(f, hash);
    }
  }

  // Must close before rename — see CLAUDE.md DESTRUCTOR_CLOSES_FILE note.
  f.close();

  if (Storage.exists(COLLECTIONS_FILE)) {
    Storage.remove(COLLECTIONS_FILE);
  }
  if (!Storage.rename(COLLECTIONS_FILE_TMP, COLLECTIONS_FILE)) {
    LOG_ERR(LOG_TAG, "Failed to rename %s -> %s", COLLECTIONS_FILE_TMP, COLLECTIONS_FILE);
    return false;
  }
  return true;
}

const Collection* CollectionStore::findById(uint32_t id) const {
  for (const auto& c : collections) {
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
  collections.push_back(std::move(c));

  if (!saveToFile()) {
    // Roll back the in-memory add so state matches disk.
    collections.pop_back();
    nextId = newId;
    return 0;
  }
  return newId;
}

void CollectionStore::removeCollection(uint32_t id) {
  const auto it =
      std::remove_if(collections.begin(), collections.end(), [id](const Collection& c) { return c.id == id; });
  if (it == collections.end()) {
    return;  // unknown id, nothing removed
  }
  collections.erase(it, collections.end());
  saveToFile();
}
