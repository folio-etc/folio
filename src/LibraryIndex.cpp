#include "LibraryIndex.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include "stores/progress/ProgressStore.h"
#include "util/PathHash.h"

namespace {
constexpr char LOG_TAG[] = "LIB";

constexpr char CACHE_DIR[] = "/.crosspoint";
constexpr char BOOKS_ROOT[] = "/Books";
constexpr char LIBRARY_FILE[] = "/.crosspoint/library.bin";
constexpr char LIBRARY_FILE_TMP[] = "/.crosspoint/library.bin.tmp";
// Paths live in their own append-only file so they never accumulate in RAM
// during indexing (streamed per book, freed immediately). `pathOffset` in each
// record is the absolute offset of that book's [len][bytes] record here.
constexpr char LIBRARY_PATHS_FILE[] = "/.crosspoint/library_paths.bin";

// 'XPLB' (CrossPoint Library) in little-endian byte order on the wire.
constexpr uint32_t LIBRARY_FILE_MAGIC = 0x424C5058u;
// v6: compact records + a RAM string arena for title/author/series/genre, with
// `path` moved to a separate on-SD file (read via getPath). Old caches (<=v5)
// fail the version check and trigger a clean rescan.
constexpr uint8_t LIBRARY_FILE_VERSION = 6;

// library.bin layout: [header][fixed entry table][arena blob].
// Header: magic u32, version u8, reserved[3], bookCount u32.
constexpr uint32_t HEADER_SIZE = 12;
// Fixed record (so a single field can be patched in place by byte offset):
//   pathHash u32 @0, openSequence u32 @4, pathOffset u32 @8,
//   progressSpineIndex u16 @12, spineCount u16 @14, seriesIndex u16 @16,
//   titleLen u16 @18, authorLen u16 @20, seriesLen u16 @22, genreLen u16 @24.
//   pathOffset addresses library_paths.bin, not library.bin.
constexpr uint32_t ENTRY_RECORD_SIZE = 26;
constexpr uint32_t REC_OPENSEQ_OFFSET = 4;
constexpr uint32_t REC_PROGRESS_OFFSET = 12;

constexpr size_t ARENA_BLOCK = 4096;

// Bound directory recursion. /Books/Author/Series/Title.epub is plenty.
constexpr int MAX_TRAVERSAL_DEPTH = 4;

constexpr int THUMB_HEIGHT = LibraryIndex::THUMB_HEIGHT;

// Soft cap. With compact records + arena (~110 B/book) this is ~55 KB resident
// — fits under the heap ceiling even with a heavy SD theme loaded.
constexpr int MAX_LIBRARY_BOOKS = 500;

struct DirFrame {
  HalFile dir;
  std::string path;
};

std::string epubLabel(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
  size_t end = path.size();
  if (end - start >= 5 && path.compare(end - 5, 5, ".epub") == 0) end -= 5;
  return path.substr(start, end - start);
}

// const char* -> string_view (the arena guarantees null-termination).
inline std::string_view sv(const char* s) { return std::string_view{s}; }

}  // namespace

LibraryIndex LibraryIndex::instance;

// ---- Arena ------------------------------------------------------------------

void LibraryIndex::arenaReset() {
  arenaBlocks.clear();
  arenaUsed = 0;
  arenaCap = 0;
}

const char* LibraryIndex::arenaPush(std::string_view s) {
  if (s.empty()) return "";
  const size_t need = s.size() + 1;  // + NUL
  if (arenaBlocks.empty() || arenaUsed + need > arenaCap) {
    const size_t cap = std::max(ARENA_BLOCK, need);
    auto block = makeUniqueNoThrow<char[]>(cap);
    if (!block) {
      LOG_ERR(LOG_TAG, "OOM: arena block %u", static_cast<unsigned>(cap));
      return "";
    }
    arenaBlocks.push_back(std::move(block));
    arenaUsed = 0;
    arenaCap = cap;
  }
  char* dst = arenaBlocks.back().get() + arenaUsed;
  std::memcpy(dst, s.data(), s.size());
  dst[s.size()] = '\0';
  arenaUsed += need;
  return dst;
}

// ---- Load / save ------------------------------------------------------------

bool LibraryIndex::loadFromFile() {
  loaded = false;
  entries.clear();
  arenaReset();

  if (!Storage.exists(LIBRARY_FILE)) {
    return false;
  }

  HalFile f;
  if (!Storage.openFileForRead(LOG_TAG, LIBRARY_FILE, f)) {
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t reserved[3] = {0, 0, 0};
  uint32_t bookCount = 0;
  serialization::readPod(f, magic);
  serialization::readPod(f, version);
  serialization::readPod(f, reserved);
  serialization::readPod(f, bookCount);

  if (magic != LIBRARY_FILE_MAGIC) {
    LOG_ERR(LOG_TAG, "library.bin: bad magic 0x%08X", magic);
    return false;
  }
  if (version != LIBRARY_FILE_VERSION) {
    LOG_DBG(LOG_TAG, "library.bin: version %u, will rebuild", version);
    return false;
  }
  if (bookCount > static_cast<uint32_t>(MAX_LIBRARY_BOOKS)) {
    LOG_ERR(LOG_TAG, "library.bin: bookCount %u exceeds cap %d", bookCount, MAX_LIBRARY_BOOKS);
    return false;
  }

  // Pass 1: read the fixed entry table (string lengths come with it).
  struct Lens {
    uint16_t title, author, series, genre;
  };
  std::vector<Lens> lens;
  entries.reserve(bookCount);
  lens.reserve(bookCount);
  uint16_t maxLen = 0;
  for (uint32_t i = 0; i < bookCount; ++i) {
    LibraryEntry e;
    Lens l;
    serialization::readPod(f, e.pathHash);
    serialization::readPod(f, e.openSequence);
    serialization::readPod(f, e.pathOffset);
    serialization::readPod(f, e.progressSpineIndex);
    serialization::readPod(f, e.spineCount);
    serialization::readPod(f, e.seriesIndex);
    serialization::readPod(f, l.title);
    serialization::readPod(f, l.author);
    serialization::readPod(f, l.series);
    serialization::readPod(f, l.genre);
    e.diskSlot = HEADER_SIZE + i * ENTRY_RECORD_SIZE;
    entries.push_back(e);
    lens.push_back(l);
    maxLen = std::max({maxLen, l.title, l.author, l.series, l.genre});
  }

  // Pass 2: stream the arena blob (immediately follows the table) into the arena.
  auto tmp = makeUniqueNoThrow<char[]>(static_cast<size_t>(maxLen) + 1);
  if (!tmp && maxLen > 0) {
    LOG_ERR(LOG_TAG, "OOM: load temp %u", maxLen + 1);
    entries.clear();
    return false;
  }
  auto readStr = [&](uint16_t len) -> const char* {
    if (len == 0) return "";
    f.read(reinterpret_cast<uint8_t*>(tmp.get()), len);
    return arenaPush(std::string_view{tmp.get(), len});
  };
  for (uint32_t i = 0; i < bookCount; ++i) {
    entries[i].title = readStr(lens[i].title);
    entries[i].author = readStr(lens[i].author);
    entries[i].series = readStr(lens[i].series);
    entries[i].genre = readStr(lens[i].genre);
  }

  loaded = true;
  LOG_DBG(LOG_TAG, "library.bin loaded: %d books", static_cast<int>(entries.size()));
  return true;
}

bool LibraryIndex::saveToFile() {
  Storage.mkdir(CACHE_DIR);

  const uint32_t bookCount = static_cast<uint32_t>(entries.size());
  for (uint32_t i = 0; i < bookCount; ++i) {
    entries[i].diskSlot = HEADER_SIZE + i * ENTRY_RECORD_SIZE;
  }

  HalFile f;
  if (!Storage.openFileForWrite(LOG_TAG, LIBRARY_FILE_TMP, f)) {
    LOG_ERR(LOG_TAG, "Cannot open %s for write", LIBRARY_FILE_TMP);
    return false;
  }

  const uint32_t magic = LIBRARY_FILE_MAGIC;
  const uint8_t version = LIBRARY_FILE_VERSION;
  const uint8_t reserved[3] = {0, 0, 0};
  serialization::writePod(f, magic);
  serialization::writePod(f, version);
  serialization::writePod(f, reserved);
  serialization::writePod(f, bookCount);

  // Entry table. pathOffset was assigned when the path was streamed to
  // library_paths.bin during refresh.
  for (const auto& e : entries) {
    const uint16_t titleLen = static_cast<uint16_t>(std::strlen(e.title));
    const uint16_t authorLen = static_cast<uint16_t>(std::strlen(e.author));
    const uint16_t seriesLen = static_cast<uint16_t>(std::strlen(e.series));
    const uint16_t genreLen = static_cast<uint16_t>(std::strlen(e.genre));
    serialization::writePod(f, e.pathHash);
    serialization::writePod(f, e.openSequence);
    serialization::writePod(f, e.pathOffset);
    serialization::writePod(f, e.progressSpineIndex);
    serialization::writePod(f, e.spineCount);
    serialization::writePod(f, e.seriesIndex);
    serialization::writePod(f, titleLen);
    serialization::writePod(f, authorLen);
    serialization::writePod(f, seriesLen);
    serialization::writePod(f, genreLen);
  }

  // Arena blob (raw chars, no NUL — lengths are in the table).
  for (const auto& e : entries) {
    f.write(reinterpret_cast<const uint8_t*>(e.title), std::strlen(e.title));
    f.write(reinterpret_cast<const uint8_t*>(e.author), std::strlen(e.author));
    f.write(reinterpret_cast<const uint8_t*>(e.series), std::strlen(e.series));
    f.write(reinterpret_cast<const uint8_t*>(e.genre), std::strlen(e.genre));
  }

  // Must close before rename — see CLAUDE.md DESTRUCTOR_CLOSES_FILE note.
  f.close();

  if (Storage.exists(LIBRARY_FILE)) {
    Storage.remove(LIBRARY_FILE);
  }
  if (!Storage.rename(LIBRARY_FILE_TMP, LIBRARY_FILE)) {
    LOG_ERR(LOG_TAG, "Failed to rename %s -> %s", LIBRARY_FILE_TMP, LIBRARY_FILE);
    return false;
  }
  return true;
}

bool LibraryIndex::patchAt(uint32_t at, const void* data, size_t size) const {
  HalFile f = Storage.open(LIBRARY_FILE, O_RDWR);
  if (!f) {
    LOG_ERR(LOG_TAG, "patchAt: cannot open %s", LIBRARY_FILE);
    return false;
  }
  if (!f.seek(at)) {
    LOG_ERR(LOG_TAG, "patchAt: seek %u failed", at);
    return false;
  }
  f.write(reinterpret_cast<const uint8_t*>(data), size);
  return true;
}

// ---- Refresh ----------------------------------------------------------------

bool LibraryIndex::refreshFromSdCard(const IndexProgressFn& onProgress) {
  if (!Storage.exists(BOOKS_ROOT)) {
    LOG_DBG(LOG_TAG, "%s not present — empty library", BOOKS_ROOT);
    const bool changed = !entries.empty();
    entries.clear();
    arenaReset();
    loaded = true;
    if (changed) {
      saveToFile();
      Storage.remove(LIBRARY_PATHS_FILE);
    }
    return true;
  }

  // Move current entries into a hash-keyed lookup so we can reuse arena strings
  // for already-indexed EPUBs without re-parsing them. The arena is NOT reset:
  // reused entries keep their existing (still-valid) string pointers; strings
  // for removed books are orphaned until the next loadFromFile compacts them.
  std::unordered_map<uint32_t, LibraryEntry> existingByHash;
  existingByHash.reserve(entries.size() + 16);
  for (auto& e : entries) {
    existingByHash.emplace(e.pathHash, std::move(e));
  }
  const size_t previousCount = existingByHash.size();
  entries.clear();
  entries.reserve(previousCount + 16);

  HalFile root = Storage.open(BOOKS_ROOT);
  if (!root || !root.isDirectory()) {
    LOG_ERR(LOG_TAG, "%s is not a directory", BOOKS_ROOT);
    return false;
  }
  root.rewindDirectory();

  std::vector<DirFrame> stack;
  stack.reserve(MAX_TRAVERSAL_DEPTH);
  stack.push_back({std::move(root), BOOKS_ROOT});

  // --- Pass 1: gather candidate EPUB paths in one cheap directory walk. ---
  std::vector<std::string> epubPaths;
  epubPaths.reserve(previousCount + 16);

  while (!stack.empty()) {
    DirFrame& frame = stack.back();
    HalFile entry = frame.dir.openNextFile();
    if (!entry) {
      frame.dir.close();
      stack.pop_back();
      continue;
    }

    char nameBuf[256];
    entry.getName(nameBuf, sizeof(nameBuf));

    if (nameBuf[0] == '.' || std::strcmp(nameBuf, "System Volume Information") == 0) {
      continue;
    }

    if (entry.isDirectory()) {
      if (stack.size() < static_cast<size_t>(MAX_TRAVERSAL_DEPTH)) {
        std::string subpath = frame.path + "/" + nameBuf;
        entry.rewindDirectory();
        stack.push_back({std::move(entry), std::move(subpath)});
      }
      continue;
    }

    if (!FsHelpers::hasEpubExtension(std::string_view{nameBuf})) {
      continue;
    }

    if (epubPaths.size() >= static_cast<size_t>(MAX_LIBRARY_BOOKS)) {
      LOG_ERR(LOG_TAG, "Hit MAX_LIBRARY_BOOKS=%d; skipping remaining EPUBs", MAX_LIBRARY_BOOKS);
      break;
    }

    epubPaths.push_back(frame.path + "/" + nameBuf);
  }

  // Now the exact count is known: size the entry vector once (the small 18 B
  // records are contiguous, but reserve avoids geometric-growth reallocations).
  entries.reserve(epubPaths.size());

  // --- Partition: reuse cached entries for known books, collect the rest.
  // Reused entries keep their existing pathOffset (into library_paths.bin), so
  // they cost no path I/O here. ---
  std::vector<std::string> newPaths;
  bool changed = false;
  for (auto& path : epubPaths) {
    const uint32_t h = hashPath(path);
    auto it = existingByHash.find(h);
    if (it != existingByHash.end()) {
      LibraryEntry e = std::move(it->second);
      existingByHash.erase(it);
      const uint16_t oldProgress = e.progressSpineIndex;
      const BookProgress* prog = PROGRESS_STORE.find(h);
      e.progressSpineIndex = prog ? prog->spineIndex : 0;
      if (e.progressSpineIndex != oldProgress) {
        changed = true;
      }
      entries.push_back(e);
    } else {
      newPaths.push_back(std::move(path));
    }
  }

  // Stream new books' paths to library_paths.bin so they never accumulate in
  // RAM. A fresh index (no reused books) truncates; an incremental refresh
  // appends, leaving reused books' offsets valid.
  const int total = static_cast<int>(newPaths.size());
  HalFile pathsFile;
  uint32_t pathFilePos = 0;
  if (total > 0) {
    if (previousCount > 0) {
      pathsFile = Storage.open(LIBRARY_PATHS_FILE, O_RDWR);
      if (pathsFile) {
        pathFilePos = static_cast<uint32_t>(pathsFile.available());  // size at pos 0
        pathsFile.seek(pathFilePos);
      }
    }
    if (!pathsFile) {  // fresh index, or no existing file: (re)create
      if (!Storage.openFileForWrite(LOG_TAG, LIBRARY_PATHS_FILE, pathsFile)) {
        LOG_ERR(LOG_TAG, "Cannot open %s for write", LIBRARY_PATHS_FILE);
        return false;
      }
      pathFilePos = 0;
    }
  }

  // --- Pass 2: index the new books, reporting progress. ---
  for (int i = 0; i < total; ++i) {
    std::string& path = newPaths[i];
    if (onProgress) onProgress(i, total, epubLabel(path).c_str());

    const uint32_t h = hashPath(path);
    Epub epub(path, CACHE_DIR);
    if (!epub.load(true, true, /*metadataOnly=*/true)) {
      LOG_ERR(LOG_TAG, "Epub load failed: %s", path.c_str());
      continue;
    }

    LibraryEntry e;
    e.pathHash = h;
    e.title = arenaPush(epub.getTitle());
    e.author = arenaPush(epub.getAuthor());
    e.series = arenaPush(epub.getSeries());
    e.genre = arenaPush(epub.getGenre());
    e.seriesIndex = epub.getSeriesIndex();
    e.spineCount = static_cast<uint16_t>(epub.getSpineItemsCount());
    const BookProgress* prog = PROGRESS_STORE.find(h);
    e.progressSpineIndex = prog ? prog->spineIndex : 0;

    // Stream the path to SD, then free its RAM immediately — path is not resident.
    const uint32_t len = static_cast<uint32_t>(path.size());
    e.pathOffset = pathFilePos;
    serialization::writePod(pathsFile, len);
    pathsFile.write(reinterpret_cast<const uint8_t*>(path.data()), len);
    pathFilePos += 4 + len;
    std::string().swap(path);  // release the path buffer now, not at function exit

    epub.generateThumbBmp(LibraryIndex::THUMB_MAX_WIDTH, THUMB_HEIGHT);

    entries.push_back(e);
    changed = true;
  }
  if (total > 0) pathsFile.close();  // flush before library.bin references it
  if (onProgress && total > 0) onProgress(total, total, "");

  if (!existingByHash.empty()) {
    changed = true;
  }

  loaded = true;

  if (changed) {
    saveToFile();
  }

  LOG_DBG(LOG_TAG, "Library refresh: %d books (%d new)", static_cast<int>(entries.size()), total);
  return true;
}

// ---- Accessors --------------------------------------------------------------

int LibraryIndex::totalPages(int perPage) const {
  if (perPage <= 0) return 0;
  if (entries.empty()) return 0;
  return (static_cast<int>(entries.size()) + perPage - 1) / perPage;
}

BookView LibraryIndex::getAt(int index) const {
  if (!isValidIndex(index)) return BookView{};
  const LibraryEntry& e = entries[index];
  BookView v;
  v.title = sv(e.title);
  v.author = sv(e.author);
  v.series = sv(e.series);
  v.genre = sv(e.genre);
  v.pathHash = e.pathHash;
  v.openSequence = e.openSequence;
  v.progressSpineIndex = e.progressSpineIndex;
  v.spineCount = e.spineCount;
  v.seriesIndex = e.seriesIndex;
  return v;
}

std::string LibraryIndex::getPath(int index) const {
  if (!isValidIndex(index)) return {};
  HalFile f;
  if (!Storage.openFileForRead(LOG_TAG, LIBRARY_PATHS_FILE, f)) return {};
  if (!f.seek(entries[index].pathOffset)) return {};
  uint32_t len = 0;
  serialization::readPod(f, len);
  if (len == 0 || len > 1024) return {};  // sanity bound on a path length
  std::string out;
  out.resize(len);
  f.read(reinterpret_cast<uint8_t*>(&out[0]), len);
  return out;
}

void LibraryIndex::unload() {
  entries.clear();
  entries.shrink_to_fit();
  arenaReset();
  loaded = false;
}

int LibraryIndex::findByPath(const std::string& path) const {
  const uint32_t h = hashPath(path);
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].pathHash == h) return static_cast<int>(i);
  }
  return -1;
}

// ---- Search -----------------------------------------------------------------

namespace {
bool containsCI(std::string_view haystack, std::string_view needle) {
  const auto eq = [](char a, char b) {
    return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
  };
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(), eq) !=
         haystack.end();
}
}  // namespace

std::vector<uint32_t> LibraryIndex::search(std::string_view query) const {
  const auto first = query.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  const auto last = query.find_last_not_of(" \t");
  query = query.substr(first, last - first + 1);

  std::vector<uint32_t> matches;
  for (size_t i = 0; i < entries.size(); ++i) {
    const LibraryEntry& e = entries[i];
    if (containsCI(sv(e.title), query) || containsCI(sv(e.author), query) ||
        containsCI(sv(e.series), query) || containsCI(sv(e.genre), query)) {
      matches.push_back(static_cast<uint32_t>(i));
    }
  }
  return matches;
}

// ---- Progress / open tracking (in-place field patches) ----------------------

void LibraryIndex::refreshProgress(const std::string& path) {
  const int idx = findByPath(path);
  if (idx < 0) return;

  const uint16_t old = entries[idx].progressSpineIndex;
  const BookProgress* prog = PROGRESS_STORE.find(entries[idx].pathHash);
  const uint16_t next = prog ? prog->spineIndex : 0;
  if (next == old) return;
  entries[idx].progressSpineIndex = next;
  patchAt(entries[idx].diskSlot + REC_PROGRESS_OFFSET, &next, sizeof(next));
}

void LibraryIndex::noteBookOpened(const std::string& path) {
  const int idx = findByPath(path);
  if (idx < 0) return;  // Not indexed (e.g. opened from file browser).

  uint32_t maxSeq = 0;
  for (const auto& e : entries) {
    if (e.openSequence > maxSeq) maxSeq = e.openSequence;
  }
  const uint32_t next = maxSeq + 1;
  entries[idx].openSequence = next;
  patchAt(entries[idx].diskSlot + REC_OPENSEQ_OFFSET, &next, sizeof(next));
}

// ---- Sort -------------------------------------------------------------------

namespace {

std::string_view titleSortKey(std::string_view v) {
  auto startsWithCi = [&](std::string_view prefix) -> bool {
    if (v.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
      char a = v[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != prefix[i]) return false;
    }
    return true;
  };
  if (startsWithCi("the "))
    v.remove_prefix(4);
  else if (startsWithCi("an "))
    v.remove_prefix(3);
  else if (startsWithCi("a "))
    v.remove_prefix(2);
  return v;
}

std::string_view authorSurname(std::string_view v) {
  const size_t sp = v.find_last_of(" \t");
  if (sp == std::string_view::npos) return v;
  return v.substr(sp + 1);
}

int ciCompare(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return (ca < cb) ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return (a.size() < b.size()) ? -1 : 1;
}

}  // namespace

void LibraryIndex::sortBy(SortField field, SortDirection direction) {
  const bool asc = (direction == SortDirection::Ascending);

  // Books with no usable key (unread / no author / never opened) always sort to
  // the END regardless of direction. Tiebreak on title then pathHash (path is
  // not resident); pathHash gives a stable, deterministic final order.
  std::sort(entries.begin(), entries.end(), [&](const LibraryEntry& a, const LibraryEntry& b) {
    auto tiebreak = [&]() {
      const int t = ciCompare(titleSortKey(sv(a.title)), titleSortKey(sv(b.title)));
      if (t != 0) return t < 0;
      return a.pathHash < b.pathHash;
    };

    switch (field) {
      case SortField::Recent: {
        const bool aHas = a.openSequence > 0;
        const bool bHas = b.openSequence > 0;
        if (aHas != bHas) return aHas;
        if (!aHas) return tiebreak();
        if (a.openSequence != b.openSequence) {
          return asc ? (a.openSequence < b.openSequence) : (a.openSequence > b.openSequence);
        }
        return tiebreak();
      }
      case SortField::Title: {
        const int c = ciCompare(titleSortKey(sv(a.title)), titleSortKey(sv(b.title)));
        if (c != 0) return asc ? (c < 0) : (c > 0);
        return a.pathHash < b.pathHash;
      }
      case SortField::Author: {
        const bool aHas = sv(a.author).size() > 0;
        const bool bHas = sv(b.author).size() > 0;
        if (aHas != bHas) return aHas;
        if (!aHas) return tiebreak();
        const int c = ciCompare(authorSurname(sv(a.author)), authorSurname(sv(b.author)));
        if (c != 0) return asc ? (c < 0) : (c > 0);
        const int c2 = ciCompare(sv(a.author), sv(b.author));
        if (c2 != 0) return asc ? (c2 < 0) : (c2 > 0);
        return tiebreak();
      }
      case SortField::Progress: {
        const bool aHas = a.progressSpineIndex > 0 && a.spineCount > 0;
        const bool bHas = b.progressSpineIndex > 0 && b.spineCount > 0;
        if (aHas != bHas) return aHas;
        if (!aHas) return tiebreak();
        const uint32_t pa = static_cast<uint32_t>(a.progressSpineIndex) * 100u / a.spineCount;
        const uint32_t pb = static_cast<uint32_t>(b.progressSpineIndex) * 100u / b.spineCount;
        if (pa != pb) return asc ? (pa < pb) : (pa > pb);
        return tiebreak();
      }
    }
    return tiebreak();
  });
}
