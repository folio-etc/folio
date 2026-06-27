#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class GfxRenderer;

// A book's genre is stored as its EPUB <dc:subject> tags joined by '\n'.
// Invokes fn(std::string_view) once per non-empty subject. Templated on the
// callback to avoid std::function heap/binary overhead (see CLAUDE.md).
template <typename Fn>
inline void forEachGenre(std::string_view genre, Fn&& fn) {
  size_t start = 0;
  while (start < genre.size()) {
    size_t nl = genre.find('\n', start);
    if (nl == std::string_view::npos) nl = genre.size();
    if (nl > start) fn(genre.substr(start, nl - start));
    start = nl + 1;
  }
}

// Resident per-book record. The four display strings live in the RAM arena
// (the `const char*` below point into it, are null-terminated, and stay valid
// for the arena's lifetime — i.e. until the next load/refresh/unload). `path`
// is NOT resident: it lives on SD and is fetched on demand via getPath() (only
// needed when a book is opened). This keeps ~500 books under the heap ceiling
// even with a heavy custom theme loaded. See docs and LibraryIndex.cpp.
struct LibraryEntry {
  const char* title = "";
  const char* author = "";
  const char* series = "";
  const char* genre = "";
  uint32_t pathHash = 0;
  uint32_t openSequence = 0;
  // Absolute byte offset of this book's path record in library.bin (len-prefixed).
  uint32_t pathOffset = 0;
  // Byte position of this entry's fixed record in library.bin, assigned on the
  // last full save. Lets refreshProgress()/noteBookOpened() patch a single field
  // in place instead of rewriting the whole file (no paths in RAM to rewrite).
  uint32_t diskSlot = 0;
  uint16_t progressSpineIndex = 0;
  uint16_t spineCount = 0;
  uint16_t seriesIndex = 0;
};

// Lightweight read accessor returned by getAt(). The string_views point into the
// arena and are null-terminated, so .data() is safe to pass to C draw APIs.
// Valid until the next load/refresh/unload (the arena does not move on sort, so
// views even survive a re-sort).
struct BookView {
  std::string_view title;
  std::string_view author;
  std::string_view series;
  std::string_view genre;
  uint32_t pathHash = 0;
  uint32_t openSequence = 0;
  uint16_t progressSpineIndex = 0;
  uint16_t spineCount = 0;
  uint16_t seriesIndex = 0;

  bool hasProgress() const { return progressSpineIndex > 0 && spineCount > 0; }
  bool hasBeenOpened() const { return openSequence > 0; }

  // 0..100. Returns 0 when unread/unknown.
  uint8_t progressPercent() const {
    if (!hasProgress()) return 0;
    const uint32_t pct = static_cast<uint32_t>(progressSpineIndex) * 100u / spineCount;
    return pct > 100u ? uint8_t{100} : static_cast<uint8_t>(pct);
  }
};

class LibraryIndex {
  static LibraryIndex instance;

  // Resident records (small + contiguous: ~30 B each). sortBy() reorders these
  // in place; diskSlot travels with each entry so in-place field patches still
  // find the right byte on disk.
  std::vector<LibraryEntry> entries;

  // String arena: title/author/series/genre for all books, packed
  // null-terminated into a chain of fixed blocks. Blocks never move, so the
  // const char* in each entry is stable for the arena's lifetime.
  std::vector<std::unique_ptr<char[]>> arenaBlocks;
  size_t arenaUsed = 0;  // bytes used in the last (current) block
  size_t arenaCap = 0;   // capacity of the last (current) block

  bool loaded = false;

 public:
  static constexpr int THUMB_HEIGHT = 120;
  static constexpr int THUMB_MAX_WIDTH = 120;

  enum class SortField : uint8_t {
    Recent = 0,
    Title = 1,
    Author = 2,
    Progress = 3,
  };
  enum class SortDirection : uint8_t {
    Descending = 0,
    Ascending = 1,
  };

  static LibraryIndex& getInstance() { return instance; }

  // Loads /.crosspoint/library.bin if present. Returns true if anything loaded.
  bool loadFromFile();

  using IndexProgressFn = std::function<void(int done, int total, const char* label)>;

  // Walks /Books (recursive, depth-limited), reuses cached metadata for known
  // books, indexes the rest (reporting via onProgress), and persists library.bin
  // if anything changed. Returns true on success (empty library is a success).
  bool refreshFromSdCard(const IndexProgressFn& onProgress = {});

  // Entry indices (in current order) whose title/author/series/genre contains
  // `query` (case-insensitive substring). A blank query returns empty.
  std::vector<uint32_t> search(std::string_view query) const;

  int getBookCount() const { return static_cast<int>(entries.size()); }
  bool isEmpty() const { return entries.empty(); }
  bool isLoaded() const { return loaded; }
  bool isValidIndex(int index) const {
    return index >= 0 && index < static_cast<int>(entries.size());
  }

  int totalPages(int perPage) const;

  // A BookView for the entry at `index`. Returns an empty view when out of range
  // (callers that can be OOB should guard with isValidIndex first).
  BookView getAt(int index) const;

  // Reads the book's path from SD (one seek+read). Empty string on OOB/error.
  // Only needed when opening a book.
  std::string getPath(int index) const;

  // Refresh a single book's progress from ProgressStore and patch it in place.
  void refreshProgress(const std::string& path);

  // Bump the matching book's openSequence to max+1 and patch it in place.
  void noteBookOpened(const std::string& path);

  // Reorder `entries` in place per the requested field and direction.
  void sortBy(SortField field, SortDirection direction);

  // Drop the in-memory records + arena. The on-disk library.bin is preserved.
  void unload();

 private:
  LibraryIndex() = default;
  // Writes library.bin (header + entry table + arena blob). Paths are NOT here —
  // they live in library_paths.bin, streamed during refresh (see refreshFromSdCard).
  bool saveToFile();

  // Copy `s` + a NUL into the arena; return a stable pointer to the copied
  // chars (null-terminated). Empty input returns "".
  const char* arenaPush(std::string_view s);
  void arenaReset();

  // Overwrite `size` bytes at absolute offset `at` in library.bin (O_RDWR, no
  // truncate). Used for single-field progress/openSequence patches.
  bool patchAt(uint32_t at, const void* data, size_t size) const;

  // Index of the entry matching `path`'s hash, or -1. Matches on pathHash only
  // (path is on SD); collision odds at <=500 books are negligible.
  // ponytail: hash-only match; add a getPath() verify if collisions ever bite.
  int findByPath(const std::string& path) const;
};

#define LIBRARY_INDEX LibraryIndex::getInstance()
