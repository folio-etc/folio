#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A user-created manual collection: a named set of books, referenced by the
// same path hash the rest of the cache uses (std::hash<path>) so moving a file
// keeps it in its collections. Persisted to /.crosspoint/collections.bin.
//
// Membership editing (adding/removing books) is not wired up yet — that arrives
// with the Book Details work. memberPathHashes is still read/written so the file
// format is stable once membership editing lands.
struct Collection {
  uint32_t id = 0;
  std::string name;
  std::vector<uint32_t> memberPathHashes;

  int memberCount() const { return static_cast<int>(memberPathHashes.size()); }
};

class CollectionStore {
  static CollectionStore instance;
  std::vector<Collection> collections;
  uint32_t nextId = 1;
  bool loaded = false;

  CollectionStore() = default;
  bool saveToFile() const;

 public:
  static CollectionStore& getInstance() { return instance; }

  // Loads /.crosspoint/collections.bin if present. Safe to call repeatedly;
  // re-reads from disk every time. Returns true if anything was loaded.
  bool loadFromFile();
  bool isLoaded() const { return loaded; }

  const std::vector<Collection>& getCollections() const { return collections; }
  bool isEmpty() const { return collections.empty(); }

  // Look up a collection by id. Returns nullptr when not found.
  const Collection* findById(uint32_t id) const;

  // Create a new (empty) collection with the given name and persist. Returns
  // the new collection id, or 0 on failure (e.g. empty name).
  uint32_t createCollection(const std::string& name);

  // Remove a collection by id and persist. No-op if the id is unknown.
  void removeCollection(uint32_t id);
};

#define COLLECTION_STORE CollectionStore::getInstance()
