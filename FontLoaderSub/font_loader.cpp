#include "font_loader.h"

#include <Windows.h>

#include <chrono>
#include <climits>
#include <cstring>
#include <string>
#include <new>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ass_string.h"
#include "ass_parser.h"
#include "path.h"
#include "log.h"
#include "mock_config.h"
#include "util.h"
#include "utf.h"

#include <tlx/sort/parallel_mergesort.hpp>
#include <blockingconcurrentqueue.h>
#include <concurrentqueue.h>
#include <xxhash.h>

static std::string fl_utf16_to_utf8_safe(const wchar_t *path) {
  if (path == nullptr)
    return std::string();
  std::string out;
  if (!Utf16ToUtf8(path, &out))
    return std::string();
  return out;
}

static size_t fl_background_sort_threads() {
  const unsigned int hw = std::thread::hardware_concurrency();
  if (hw <= 2u)
    return 1u;
  return (hw - 1u < 4u) ? hw - 1u : 4u;
}

static std::string fl_font_key(const char *font, size_t cch) {
  if (font == nullptr || cch == 0)
    return std::string();
  std::wstring wide;
  if (!Utf8ToUtf16(font, cch, &wide))
    return std::string();
  if (!wide.empty()) {
    CharLowerBuffW(&wide[0], (DWORD)wide.size());
  }
  std::string out;
  if (!Utf16ToUtf8(wide, &out))
    return std::string();
  return out;
}

int fl_init(FL_LoaderCtx *c, allocator_t *alloc) {
  int r = FL_OK;
  c->alloc = alloc;
  c->sub_fonts.clear();
  c->sub_font_set.clear();
  c->loaded_sub_files.clear();
  c->font_path.clear();
  c->walk_path.clear();
  c->font_set = nullptr;
  c->num_sub.store(0, std::memory_order_relaxed);
  c->num_sub_font.store(0, std::memory_order_relaxed);
  c->num_font_loaded.store(0, std::memory_order_relaxed);
  c->num_font_failed.store(0, std::memory_order_relaxed);
  c->num_font_unmatched.store(0, std::memory_order_relaxed);
  c->num_scan_file.store(0, std::memory_order_relaxed);
  c->num_scan_face.store(0, std::memory_order_relaxed);
  c->loaded_font.clear();
  c->duplicate_font_files.clear();
  c->duplicate_font_versions.clear();
  c->event_cancel = nullptr;
  c->event_cancel = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!c->event_cancel)
    r = FL_OS_ERROR;

  if (r != FL_OK) {
    SPDLOG_ERROR("fl_init failed, err={}", r);
    fl_free(c);
  } else {
    SPDLOG_INFO("fl_init ok");
  }
  return r;
}

int fl_free(FL_LoaderCtx *c) {
  if (c->event_cancel) {
    CloseHandle(c->event_cancel);
    c->event_cancel = nullptr;
  }
  c->loaded_font.clear();
  c->duplicate_font_files.clear();
  c->duplicate_font_versions.clear();
  c->sub_fonts.clear();
  c->sub_font_set.clear();
  c->loaded_sub_files.clear();
  c->font_path.clear();
  c->walk_path.clear();
  fs_free(c->font_set);
  c->font_set = nullptr;

  SPDLOG_INFO("fl_free done");
  return FL_OK;
}

static bool fl_path_to_lower_utf8(const wchar_t *path, std::string *out) {
  if (path == nullptr || out == nullptr)
    return false;
  std::wstring tmp(path);
  if (!tmp.empty()) {
    CharLowerBuffW(&tmp[0], (DWORD)tmp.size());
  }
  return Utf16ToUtf8(tmp, out);
}

static int fl_is_sub_file_loaded(FL_LoaderCtx *c, const wchar_t *filePath) {
  std::string needle;
  if (!fl_path_to_lower_utf8(filePath, &needle))
    return 0;
  return c->loaded_sub_files.find(needle) != c->loaded_sub_files.end();
}

static int fl_add_sub_file_loaded(FL_LoaderCtx *c, const wchar_t *filePath) {
  std::string value;
  if (!fl_path_to_lower_utf8(filePath, &value))
    return 0;
  return c->loaded_sub_files.insert(std::move(value)).second ? 1 : 0;
}

int fl_cancel(FL_LoaderCtx *c) {
  return SetEvent(c->event_cancel) ? FL_OK : FL_OS_ERROR;
}

static int fl_check_cancel(FL_LoaderCtx *c) {
  if (WaitForSingleObject(c->event_cancel, 0) != WAIT_TIMEOUT)
    return FL_OS_ERROR;
  return FL_OK;
}

static int fl_sub_font_callback(const char *font, size_t cch, void *arg) {
  FL_LoaderCtx *c = (FL_LoaderCtx *)arg;
  if (cch != 0) {
    if (font[0] == '@') {
      // skip prefix '@'
      font++;
      cch--;
    }
    if (cch == 0)
      return FL_OK;

    std::string name(font, cch);
    std::string key = fl_font_key(font, cch);
    if (key.empty())
      key = name;
    if (c->sub_font_set.insert(key).second) {
      c->sub_fonts.push_back(std::move(name));
      c->num_sub_font.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return FL_OK;
}

static int
fl_walk_sub_callback(const wchar_t *path, WIN32_FIND_DATA *data, void *arg) {
  FL_LoaderCtx *c = (FL_LoaderCtx *)arg;
  // NOTE: don't name this variable `r`.
  // Some Windows/CRT environments may macro-substitute short identifiers,
  // which breaks assignment and triggers C2166/C2106 on MSVC.
  const int fl_rc = fl_check_cancel(c);
  if (fl_rc != FL_OK)
    return fl_rc;

  // Deduplicate per subtitle file. This is required for correct incremental
  // counting when a folder drop contains already-processed files.
  if (fl_is_sub_file_loaded(c, path))
    return FL_OK;

  const size_t len = ass_strlen(path);
  const wchar_t *ext = path + len - 4;
  const int match_attr =
      !(data->dwFileAttributes &
        (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_DIRECTORY));
  const int match_size =
      (data->nFileSizeHigh == 0 && data->nFileSizeLow <= 64 * 1024 * 1024);
  const int match_ext = (len > 4) && (ass_strncasecmp(ext, L".ass", 4) == 0 ||
                                      ass_strncasecmp(ext, L".ssa", 4) == 0);
  if (!(match_attr && match_size && match_ext))
    return FL_OK;

  const std::string path_u8 = fl_utf16_to_utf8_safe(path);
  if (!path_u8.empty()) {
    SPDLOG_INFO("ASS file detected: {}", path_u8);
  } else {
    SPDLOG_INFO("ASS file detected (utf16)");
  }

  memmap_t map;
  char *content = nullptr;
  size_t content_len = 0;

  FlMemMap(path, &map);
  if (!map.data) {
    if (!path_u8.empty()) {
      SPDLOG_WARN("ASS file map failed: {}", path_u8);
    } else {
      SPDLOG_WARN("ASS file map failed (utf16)");
    }
    // ignore error
    return FL_OK;
  }

  content =
      FlTextDecode((const uint8_t *)map.data, map.size, &content_len, c->alloc);
  if (content == nullptr) {
    // ignore error
    FlMemUnmap(&map);
    if (!path_u8.empty()) {
      SPDLOG_WARN("ASS file decode failed: {}", path_u8);
    } else {
      SPDLOG_WARN("ASS file decode failed (utf16)");
    }
    return FL_OK;
  }

  const int added = fl_add_sub_file_loaded(c, path);
  if (!added) {
    if (!path_u8.empty()) {
      SPDLOG_WARN(
          "ASS file dedup tracking skipped (utf8 conversion failed): {}",
          path_u8);
    } else {
      SPDLOG_WARN("ASS file dedup tracking skipped (utf16 conversion failed)");
    }
  }

  const size_t before_fonts = c->sub_font_set.size();
  c->num_sub.fetch_add(1, std::memory_order_relaxed);
  ass_process_data(content, content_len, fl_sub_font_callback, c);
  const size_t after_fonts = c->sub_font_set.size();
  const size_t new_fonts =
      (after_fonts >= before_fonts) ? (after_fonts - before_fonts) : 0;
  if (!path_u8.empty()) {
    SPDLOG_INFO(
        "ASS parsed: {} new_fonts={} total_fonts={}", path_u8, new_fonts,
        after_fonts);
  } else {
    SPDLOG_INFO(
        "ASS parsed (utf16) new_fonts={} total_fonts={}", new_fonts,
        after_fonts);
  }
  if (MOCK_DELAY_SUB)
    Sleep(MOCK_DELAY_SUB);

  FlMemUnmap(&map);
  c->alloc->alloc(content, 0, c->alloc->arg);
  return FL_OK;
}

int fl_add_subs(FL_LoaderCtx *c, const wchar_t *path) {
  int r;
  const std::string path_u8 = fl_utf16_to_utf8_safe(path);
  if (!path_u8.empty()) {
    SPDLOG_INFO("fl_add_subs start: {}", path_u8);
  } else {
    SPDLOG_INFO("fl_add_subs start (utf16)");
  }
  do {
    c->walk_path.clear();
    r = FlResolvePath(path, &c->walk_path);
    if (r == FL_OS_ERROR) {
      // ignore error
      r = FL_OK;
      break;
    } else if (r != FL_OK) {
      break;
    }

    r = FlWalkDir(c->walk_path.c_str(), fl_walk_sub_callback, c);
    if (r != FL_OK)
      break;
  } while (0);
  if (!path_u8.empty()) {
    SPDLOG_INFO(
        "fl_add_subs done: {} result={} sub_count={} font_count={}", path_u8, r,
        c->num_sub.load(std::memory_order_relaxed),
        c->num_sub_font.load(std::memory_order_relaxed));
  } else {
    SPDLOG_INFO(
        "fl_add_subs done (utf16) result={} sub_count={} font_count={}", r,
        c->num_sub.load(std::memory_order_relaxed),
        c->num_sub_font.load(std::memory_order_relaxed));
  }
  return r;
}

typedef struct {
  std::wstring path;
  std::string tag;
} FL_FontScanItem;

typedef struct {
  std::string tag;
  FS_FontParseResult *parsed;
} FL_FontScanResult;

typedef struct {
  FL_LoaderCtx *loader;
  size_t base_len;
  moodycamel::BlockingConcurrentQueue<FL_FontScanItem> *queue;
  std::atomic<int> *error;
  std::atomic<bool> *cancel;
} FL_FontScanCtx;

static int
fl_walk_font_enqueue(const wchar_t *path, WIN32_FIND_DATA *data, void *arg) {
  FL_FontScanCtx *ctx = (FL_FontScanCtx *)arg;
  if (ctx->cancel->load()) {
    const int r = ctx->error->load();
    return r == FL_OK ? FL_OS_ERROR : r;
  }

  const int r = fl_check_cancel(ctx->loader);
  if (r != FL_OK) {
    ctx->error->store(r);
    ctx->cancel->store(true);
    return r;
  }

  const size_t len = ass_strlen(path);
  const wchar_t *ext = path + len - 4;
  const int match_attr =
      !(data->dwFileAttributes &
        (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_DIRECTORY));
  const int match_ext = (len > 4) && (ass_strncasecmp(ext, L".ttc", 4) == 0 ||
                                      ass_strncasecmp(ext, L".otf", 4) == 0 ||
                                      ass_strncasecmp(ext, L".ttf", 4) == 0);
  if (!(match_attr && match_ext))
    return FL_OK;

  const wchar_t *tag = path + ctx->base_len + 1;
  std::string tag_u8;
  if (!Utf16ToUtf8(tag, &tag_u8))
    return FL_OK;

  FL_FontScanItem item;
  item.path = path;
  item.tag = std::move(tag_u8);
  ctx->queue->enqueue(std::move(item));
  ctx->loader->num_scan_file.fetch_add(1, std::memory_order_relaxed);
  return FL_OK;
}

static int fl_scan_fonts_mt(FL_LoaderCtx *c) {
  SPDLOG_INFO("fl_scan_fonts_mt start");
  moodycamel::BlockingConcurrentQueue<FL_FontScanItem> work_queue;
  moodycamel::ConcurrentQueue<FL_FontScanResult> result_queue;
  std::atomic<bool> cancel(false);
  std::atomic<bool> done(false);
  std::atomic<int> error(FL_OK);

  const unsigned int hw = std::thread::hardware_concurrency();
  unsigned int worker_count = (hw > 1u) ? hw - 1u : 1u;
  if (worker_count > 8u)
    worker_count = 8u;
  SPDLOG_INFO("fl_scan_fonts_mt workers={}", worker_count);

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned int i = 0; i < worker_count; i++) {
    workers.emplace_back([&]() {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      FL_FontScanItem item;
      while (!cancel.load()) {
        const int cancel_check = fl_check_cancel(c);
        if (cancel_check != FL_OK) {
          error.store(cancel_check);
          cancel.store(true);
          break;
        }
        if (!work_queue.wait_dequeue_timed(
                item, std::chrono::milliseconds(25))) {
          if (done.load())
            break;
          continue;
        }

        memmap_t map;
        FlMemMap(item.path.c_str(), &map);
        if (!map.data)
          continue;

        FS_FontParseResult *parsed =
            fs_parse_font_data((const uint8_t *)map.data, map.size);
        FlMemUnmap(&map);

        if (parsed) {
          const uint32_t faces = fs_parse_result_face_count(parsed);
          c->num_scan_face.fetch_add(faces, std::memory_order_relaxed);
        }

        FL_FontScanResult res;
        res.tag = std::move(item.tag);
        res.parsed = parsed;
        result_queue.enqueue(std::move(res));
      }
    });
  }

  FL_FontScanCtx ctx = {};
  ctx.loader = c;
  ctx.base_len = c->font_path.size();
  ctx.queue = &work_queue;
  ctx.error = &error;
  ctx.cancel = &cancel;

  int r = FlWalkDir(c->walk_path.c_str(), fl_walk_font_enqueue, &ctx);
  if (r != FL_OK) {
    error.store(r);
    cancel.store(true);
  }
  done.store(true);

  for (auto &t : workers) {
    if (t.joinable())
      t.join();
  }

  FL_FontScanResult res;
  while (result_queue.try_dequeue(res)) {
    fs_add_parsed_font(c->font_set, res.tag.c_str(), res.parsed);
    fs_parse_font_free(res.parsed);
  }

  const int scan_err = error.load();
  SPDLOG_INFO(
      "fl_scan_fonts_mt done result={}", scan_err == FL_OK ? r : scan_err);
  return scan_err == FL_OK ? r : scan_err;
}

static void fl_blacklist_parse(FL_LoaderCtx *c, const char *data, size_t len) {
  const char *p = data;
  const char *eos = data + len;
  while (p != eos) {
    // skip blank lines
    while (p != eos && ass_is_eol(*p))
      ++p;
    // find end of the line
    const char *q = p;
    while (q != eos && !ass_is_eol(*q))
      ++q;

    if (q > p) {
      fs_blacklist_add(c->font_set, p, (size_t)(q - p));
    }
    p = q;
  }
}

static void fl_blacklist_load(FL_LoaderCtx *c, const wchar_t *filename) {
  fs_blacklist_clear(c->font_set);
  if (!filename) {
    return;
  }
  c->walk_path = c->font_path;
  if (!c->walk_path.empty() && c->walk_path.back() != L'\\') {
    c->walk_path += L'\\';
  }
  c->walk_path += filename;
  memmap_t map;
  char *content = nullptr;
  size_t content_len = 0;
  do {
    FlMemMap(c->walk_path.c_str(), &map);
    if (!map.data)
      break;
    content = FlTextDecode(
        (const uint8_t *)map.data, map.size, &content_len, c->alloc);
    if (content == nullptr)
      break;
    fl_blacklist_parse(c, content, content_len);

  } while ((0));

  FlMemUnmap(&map);
  c->alloc->alloc(content, 0, c->alloc->arg);
}

struct FL_Hash128Key {
  uint64_t low;
  uint64_t high;

  bool operator==(const FL_Hash128Key &other) const {
    return low == other.low && high == other.high;
  }
};

struct FL_Hash128KeyHasher {
  size_t operator()(const FL_Hash128Key &key) const {
    return static_cast<size_t>(
        key.low ^ (key.high + 0x9e3779b97f4a7c15ULL + (key.low << 6) +
                   (key.low >> 2)));
  }
};

typedef struct {
  FL_Hash128Key hash;
  std::string path;
} FL_IndexedFile;

typedef struct {
  std::string name;
  std::vector<FL_FontVersionPath> fonts;
} FL_IndexedName;

typedef struct {
  std::unordered_map<std::string, FL_IndexedFile> files;
  std::unordered_map<std::string, FL_IndexedName> names;
} FL_FontAnalysisCtx;

static int
fl_collect_font_index(const FS_Index *info, void *param) {
  auto ctx = static_cast<FL_FontAnalysisCtx *>(param);
  if (info == nullptr || info->tag == nullptr || info->face == nullptr)
    return FL_OK;
  try {
    ctx->files.emplace(
        info->tag,
        FL_IndexedFile{
            {info->hash_low64, info->hash_high64}, info->tag});

    const std::string name_key =
        fl_font_key(info->face, strlen(info->face));
    if (name_key.empty())
      return FL_OK;
    auto &name = ctx->names[name_key];
    if (name.name.empty())
      name.name = info->face;
    const std::string version = info->ver ? info->ver : "";
    for (const auto &font : name.fonts) {
      if (font.version == version && font.path == info->tag)
        return FL_OK;
    }
    name.fonts.push_back({version, info->tag});
  } catch (const std::bad_alloc &) {
    return FL_OUT_OF_MEMORY;
  }
  return FL_OK;
}

static void fl_analyze_font_index(FL_LoaderCtx *c) {
  c->duplicate_font_files.clear();
  c->duplicate_font_versions.clear();
  if (c->font_set == nullptr)
    return;

  try {
    FL_FontAnalysisCtx index;
    if (fs_walk_index(c->font_set, fl_collect_font_index, &index) != FL_OK)
      return;

    std::unordered_map<
        FL_Hash128Key,
        std::vector<std::string>,
        FL_Hash128KeyHasher>
        hash_groups;
    hash_groups.reserve(index.files.size());
    for (const auto &item : index.files)
      hash_groups[item.second.hash].push_back(item.second.path);

    for (auto &item : hash_groups) {
      auto &paths = item.second;
      if (paths.size() < 2)
        continue;
      std::sort(paths.begin(), paths.end());
      c->duplicate_font_files.insert(
          c->duplicate_font_files.end(), paths.begin() + 1, paths.end());
    }
    std::sort(
        c->duplicate_font_files.begin(), c->duplicate_font_files.end());

    for (auto &item : index.names) {
      auto &name = item.second;
      std::unordered_set<std::string> versions;
      for (const auto &font : name.fonts)
        versions.insert(font.version);
      if (versions.size() < 2)
        continue;
      std::sort(
          name.fonts.begin(), name.fonts.end(),
          [](const FL_FontVersionPath &a, const FL_FontVersionPath &b) {
            if (a.version != b.version)
              return a.version < b.version;
            return a.path < b.path;
          });
      c->duplicate_font_versions.push_back(
          {std::move(name.name), std::move(name.fonts)});
    }
    std::sort(
        c->duplicate_font_versions.begin(),
        c->duplicate_font_versions.end(),
        [](const FL_FontVersionGroup &a, const FL_FontVersionGroup &b) {
          return a.name < b.name;
        });
  } catch (const std::bad_alloc &) {
    c->duplicate_font_files.clear();
    c->duplicate_font_versions.clear();
    SPDLOG_WARN("Failed to analyze duplicate fonts");
  }

  SPDLOG_INFO(
      "Font duplicate analysis: identical_files={} versioned_names={}",
      c->duplicate_font_files.size(), c->duplicate_font_versions.size());
}

int fl_scan_fonts(
    FL_LoaderCtx *c,
    const wchar_t *path,
    const wchar_t *cache,
    const wchar_t *black) {
  // caller: fl_unload_fonts

  // free previous font set
  fs_free(c->font_set);
  c->font_set = nullptr;

  const std::string path_u8 = fl_utf16_to_utf8_safe(path);
  const std::string cache_u8 = fl_utf16_to_utf8_safe(cache);
  const std::string black_u8 = fl_utf16_to_utf8_safe(black);
  if (!path_u8.empty()) {
    SPDLOG_INFO(
        "fl_scan_fonts start: path={} cache={} black={}", path_u8,
        cache_u8.empty() ? "<none>" : cache_u8,
        black_u8.empty() ? "<none>" : black_u8);
  } else {
    SPDLOG_INFO("fl_scan_fonts start (utf16)");
  }

  c->num_scan_file.store(0, std::memory_order_relaxed);
  c->num_scan_face.store(0, std::memory_order_relaxed);

  int r = FlResolvePath(path, &c->font_path);
  // if path points to a file, find its parent directory
  if (1) {
    HANDLE test = CreateFile(
        c->font_path.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (test != INVALID_HANDLE_VALUE) {
      FlPathParent(&c->font_path);
      CloseHandle(test);
    }
  }

  if (cache) {
    if (r == FL_OK) {
      // load from cache
      c->walk_path = c->font_path;
      if (!c->walk_path.empty() && c->walk_path.back() != L'\\')
        c->walk_path += L'\\';
      c->walk_path += cache;
    }
    if (r == FL_OK) {
      r = fs_cache_load(c->walk_path.c_str(), c->alloc, &c->font_set);
    }
  } else {
    // search font files
    if (r == FL_OK) {
      c->walk_path = c->font_path;
    }
    if (r == FL_OK) {
      r = fs_create(c->alloc, &c->font_set);
    }
    if (r == FL_OK) {
      r = fl_scan_fonts_mt(c);
    }
  }
  if (r == FL_OK) {
    r = fs_build_index(c->font_set);
    fl_blacklist_load(c, black);
    fl_analyze_font_index(c);
  }

  // failed
  if (r != FL_OK) {
    fs_free(c->font_set);
    c->font_set = nullptr;
  }

  if (r == FL_OK && c->font_set) {
    FS_Stat stat = {0};
    fs_stat(c->font_set, &stat);
    c->num_scan_file.store(stat.num_file, std::memory_order_release);
    c->num_scan_face.store(stat.num_face, std::memory_order_release);
    SPDLOG_INFO(
        "fl_scan_fonts done: files={} faces={} r={}", stat.num_file,
        stat.num_face, r);
  } else {
    SPDLOG_WARN("fl_scan_fonts failed: r={}", r);
  }

  return r;
}

int fl_save_cache(FL_LoaderCtx *c, const wchar_t *cache) {
  int r = FL_OK;
  c->walk_path = c->font_path;
  if (!c->walk_path.empty() && c->walk_path.back() != L'\\')
    c->walk_path += L'\\';
  c->walk_path += cache;

  r = fs_cache_dump(c->font_set, c->walk_path.c_str());
  if (r == FL_OK) {
    const std::string cache_u8 = fl_utf16_to_utf8_safe(c->walk_path.c_str());
    if (!cache_u8.empty()) {
      SPDLOG_INFO("Cache saved: {}", cache_u8);
    } else {
      SPDLOG_INFO("Cache saved (utf16)");
    }
  } else {
    SPDLOG_WARN("Cache save failed: r={}", r);
  }
  return r;
}

static int CALLBACK enum_fonts(
    const LOGFONTW *lfp,
    const TEXTMETRICW *tmp,
    DWORD fontType,
    LPARAM lParam) {
  int *r = (int *)lParam;
  *r = 1;
  return 1;  // continue
}

static bool fl_utf8_to_utf16(const char *input, std::wstring *output) {
  if (input == nullptr || output == nullptr)
    return false;
  return Utf8ToUtf16(input, output);
}

static int IsFontInstalled(const char *face) {
  if (MOCK_NO_SYS)
    return 0;
  std::wstring face_w;
  if (!fl_utf8_to_utf16(face, &face_w))
    return 0;
  int found = 0;
  HDC dc = GetDC(nullptr);
  EnumFontFamiliesW(dc, face_w.c_str(), enum_fonts, (LPARAM)&found);
  ReleaseDC(nullptr, dc);
  return found;
}

static int fl_utf8_casecmp(const char *a, const char *b);

static int fl_face_loaded(FL_LoaderCtx *c, const char *face) {
  if (face == nullptr)
    return 0;
  for (const auto &m : c->loaded_font) {
    if (fl_utf8_casecmp(m.face.c_str(), face) == 0)
      return 1;
  }
  return 0;
}

static int fl_file_loaded(FL_LoaderCtx *c, const char *file) {
  for (size_t i = 0; i != c->loaded_font.size(); i++) {
    if (c->loaded_font[i].filename == file)
      return (i > static_cast<size_t>(INT_MAX)) ? -1 : (int)i;
  }
  return -1;
}

static int fl_utf8_casecmp(const char *a, const char *b) {
  if (a == nullptr)
    return b ? -1 : 0;
  if (b == nullptr)
    return 1;
  std::wstring wa;
  std::wstring wb;
  if (!Utf8ToUtf16(a, &wa))
    return -1;
  if (!Utf8ToUtf16(b, &wb))
    return 1;
  return FlStrCmpIW(wa.c_str(), wb.c_str());
}

static int fl_hash_loaded(FL_LoaderCtx *c, XXH128_hash_t hash) {
  for (size_t i = 0; i != c->loaded_font.size(); i++) {
    FL_FontMatch *m = &c->loaded_font[i];
    if ((m->flag & FL_LOAD_OK) && m->hash_low64 == hash.low64 &&
        m->hash_high64 == hash.high64)
      return (i > static_cast<size_t>(INT_MAX)) ? -1 : (int)i;
  }
  return -1;
}

static bool fl_try_push_loaded(FL_LoaderCtx *c, const FL_FontMatch &m) {
  try {
    c->loaded_font.push_back(m);
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

int fl_find_missing_fonts(
    FL_LoaderCtx *c,
    size_t first_font,
    std::vector<std::string> *missing) {
  if (c == nullptr || missing == nullptr || c->font_set == nullptr)
    return FL_OS_ERROR;

  missing->clear();
  if (first_font > c->sub_fonts.size())
    first_font = c->sub_fonts.size();

  try {
    missing->reserve(c->sub_fonts.size() - first_font);
    for (size_t i = first_font; i < c->sub_fonts.size(); i++) {
      const int r = fl_check_cancel(c);
      if (r != FL_OK)
        return r;

      const std::string &face = c->sub_fonts[i];
      if (fl_face_loaded(c, face.c_str()) || IsFontInstalled(face.c_str()))
        continue;

      FS_Iter it;
      if (!fs_iter_new(c->font_set, face.c_str(), &it)) {
        missing->push_back(face);
      }
    }
  } catch (const std::bad_alloc &) {
    missing->clear();
    return FL_OUT_OF_MEMORY;
  }

  SPDLOG_INFO(
      "fl_find_missing_fonts done: first={} total={} missing={}", first_font,
      c->sub_fonts.size(), missing->size());
  return FL_OK;
}

static int
fl_load_file(FL_LoaderCtx *c, const char *face, const char *file, int *dup) {
  int r = FL_OK;
  int candidate;
  memmap_t map = {nullptr};
  XXH128_hash_t hash = {};

  do {
    try {
      c->loaded_font.reserve(c->loaded_font.size() + 1);
    } catch (const std::bad_alloc &) {
      r = FL_OUT_OF_MEMORY;
      break;
    }

    // check 1: if file pointer is loaded
    candidate = fl_file_loaded(c, file);
    if (candidate != -1) {
      *dup = candidate;
      r = FL_DUP;
      break;
    }

    // check 2: hash
    std::wstring file_w;
    if (!Utf8ToUtf16(file, &file_w)) {
      r = FL_OUT_OF_MEMORY;
      break;
    }
    c->walk_path = c->font_path;
    if (!c->walk_path.empty() && c->walk_path.back() != L'\\')
      c->walk_path += L'\\';
    c->walk_path += file_w;

    const wchar_t *full_path = c->walk_path.c_str();
    FlMemMap(full_path, &map);
    if (map.data == nullptr) {
      r = FL_OS_ERROR;
      break;
    }

    hash = XXH3_128bits(map.data, map.size);

    candidate = fl_hash_loaded(c, hash);
    if (candidate != -1) {
      *dup = candidate;
      r = FL_DUP;
      break;
    }

    if (MOCK_FAKE_LOAD) {
      if (MOCK_DELAY_FONT) {
        Sleep(MOCK_DELAY_FONT);
      }
    } else if (AddFontResource(full_path) == 0) {
      r = FL_OS_ERROR;
      break;
    }
  } while (0);

  FlMemUnmap(&map);
  if (r != FL_OUT_OF_MEMORY && r != FL_DUP) {
    FL_FontMatch m;
    if (r == FL_OK) {
      m.flag = FL_LOAD_OK;
      c->num_font_loaded.fetch_add(1, std::memory_order_relaxed);
    } else {
      m.flag = FL_LOAD_ERR;
      c->num_font_failed.fetch_add(1, std::memory_order_relaxed);
    }
    if (face)
      m.face = face;
    else
      m.face.clear();
    if (file)
      m.filename = file;
    else
      m.filename.clear();
    m.hash_low64 = hash.low64;
    m.hash_high64 = hash.high64;
    try {
      c->loaded_font.push_back(m);
    } catch (const std::bad_alloc &) {
      if (m.flag == FL_LOAD_OK)
        c->num_font_loaded.fetch_sub(1, std::memory_order_relaxed);
      else
        c->num_font_failed.fetch_sub(1, std::memory_order_relaxed);
      r = FL_OUT_OF_MEMORY;
    }
  }
  if (r == FL_OS_ERROR) {
    SPDLOG_WARN(
        "Font load failed: face={} file={}", face ? face : "<null>",
        file ? file : "<null>");
  } else if (r == FL_DUP) {
    SPDLOG_INFO(
        "Font load duplicate: face={} file={}", face ? face : "<null>",
        file ? file : "<null>");
  }
  return r;
}

int fl_load_rec_sort(const void *ptr_a, const void *ptr_b, void *arg) {
  const FL_FontMatch *a = (const FL_FontMatch *)ptr_a;
  const FL_FontMatch *b = (const FL_FontMatch *)ptr_b;

  // sort flag in descent order
  const int flag_mask = 16 | 8;  // FL_LOAD_ERR | FL_LOAD_MISS
  const int df = (b->flag & flag_mask) - (a->flag & flag_mask);
  if (df != 0)
    return df;

  // sort in filename
  if (a->filename.empty())
    return -1;
  if (b->filename.empty())
    return 1;
  const int ds = fl_utf8_casecmp(a->filename.c_str(), b->filename.c_str());
  if (ds != 0)
    return ds;

  // dup comes late
  if (b->flag & FL_LOAD_DUP)
    return -1;
  if (a->flag & FL_LOAD_DUP)
    return 1;

  // last resort
  return fl_utf8_casecmp(a->face.c_str(), b->face.c_str());
}

int fl_load_fonts(FL_LoaderCtx *c) {
  // caller: fl_unload_fonts

  int r = FL_OK;
  c->num_font_failed.store(0, std::memory_order_relaxed);
  c->num_font_loaded.store(0, std::memory_order_relaxed);
  c->num_font_unmatched.store(0, std::memory_order_relaxed);

  SPDLOG_INFO("fl_load_fonts start: sub_fonts={}", c->sub_fonts.size());

  // pass 1: scan for existing fonts
  for (const auto &face : c->sub_fonts) {
    if ((r = fl_check_cancel(c)) != FL_OK)
      return r;

    if (IsFontInstalled(face.c_str())) {
      FL_FontMatch m;
      m.flag = FL_OS_LOADED;
      m.face = face;
      m.filename.clear();
      if (!fl_try_push_loaded(c, m))
        return FL_OUT_OF_MEMORY;
    }
  }

  // pass 2: load the missing font
  for (const auto &face : c->sub_fonts) {
    if (r == FL_OUT_OF_MEMORY)
      break;
    if (fl_face_loaded(c, face.c_str()))
      continue;

    FS_Iter it;
    if (!fs_iter_new(c->font_set, face.c_str(), &it)) {
      FL_FontMatch m;
      m.flag = FL_LOAD_MISS;
      m.face = face;
      m.filename.clear();
      if (!fl_try_push_loaded(c, m))
        return FL_OUT_OF_MEMORY;
      c->num_font_unmatched.fetch_add(1, std::memory_order_relaxed);
      SPDLOG_INFO("Font missing: {}", face);
    } else {
      int num_loaded = 0;
      int num_dup = 0;
      int num_total = 0;
      int dup_candidate = 0;
      do {
        if ((r = fl_check_cancel(c)) != FL_OK)
          return r;

        r = fl_load_file(c, face.c_str(), it.info.tag, &dup_candidate);
        num_total++;
        if (r == FL_DUP)
          num_dup++;
        if (r == FL_OK)
          num_loaded++;
      } while (r != FL_OUT_OF_MEMORY && num_loaded <= 16 && fs_iter_next(&it));
      SPDLOG_INFO(
          "Font load summary: face={} loaded={} dup={} total={}", face,
          num_loaded, num_dup, num_total);
      if (num_dup == num_total && dup_candidate >= 0 &&
          dup_candidate < (int)c->loaded_font.size()) {
        FL_FontMatch m;
        const FL_FontMatch &ref = c->loaded_font[dup_candidate];
        m.flag = (FL_MatchFlag)(FL_LOAD_DUP | ref.flag);
        m.face = face;
        m.filename = ref.filename;
        if (!fl_try_push_loaded(c, m))
          return FL_OUT_OF_MEMORY;
      }
    }
  }
  if (c->loaded_font.size() > 1) {
    FL_FontMatch *data = c->loaded_font.data();
    tlx::parallel_mergesort(
        data, data + c->loaded_font.size(),
        [](const FL_FontMatch &a, const FL_FontMatch &b) {
          return fl_load_rec_sort(&a, &b, nullptr) < 0;
        },
        fl_background_sort_threads());
  }

  SPDLOG_INFO(
      "fl_load_fonts done: loaded={} failed={} unmatched={} total_records={}",
      c->num_font_loaded.load(std::memory_order_relaxed),
      c->num_font_failed.load(std::memory_order_relaxed),
      c->num_font_unmatched.load(std::memory_order_relaxed),
      c->loaded_font.size());
  return FL_OK;
}

int fl_load_fonts_incremental(FL_LoaderCtx *c) {
  // Incremental font loading: don't reset counters, only load new fonts
  // This is used when adding new subtitle files after initial load

  int r = FL_OK;
  // NOTE: Don't reset counters here, unlike fl_load_fonts

  SPDLOG_INFO(
      "fl_load_fonts_incremental start: sub_fonts={}", c->sub_fonts.size());

  // pass 1: scan for existing fonts (skip already loaded ones)
  for (const auto &face : c->sub_fonts) {
    if ((r = fl_check_cancel(c)) != FL_OK)
      return r;

    if (fl_face_loaded(c, face.c_str()))
      continue;

    if (IsFontInstalled(face.c_str())) {
      FL_FontMatch m;
      m.flag = FL_OS_LOADED;
      m.face = face;
      m.filename.clear();
      if (!fl_try_push_loaded(c, m))
        return FL_OUT_OF_MEMORY;
    }
  }

  // pass 2: load the missing font
  for (const auto &face : c->sub_fonts) {
    if (r == FL_OUT_OF_MEMORY)
      break;
    if (fl_face_loaded(c, face.c_str()))
      continue;

    FS_Iter it;
    if (!fs_iter_new(c->font_set, face.c_str(), &it)) {
      FL_FontMatch m;
      m.flag = FL_LOAD_MISS;
      m.face = face;
      m.filename.clear();
      if (!fl_try_push_loaded(c, m))
        return FL_OUT_OF_MEMORY;
      c->num_font_unmatched.fetch_add(1, std::memory_order_relaxed);
      SPDLOG_INFO("Font missing: {}", face);
    } else {
      int num_loaded = 0;
      int num_dup = 0;
      int num_total = 0;
      int dup_candidate = 0;
      do {
        if ((r = fl_check_cancel(c)) != FL_OK)
          return r;

        r = fl_load_file(c, face.c_str(), it.info.tag, &dup_candidate);
        num_total++;
        if (r == FL_DUP)
          num_dup++;
        if (r == FL_OK)
          num_loaded++;
      } while (r != FL_OUT_OF_MEMORY && num_loaded <= 16 && fs_iter_next(&it));
      SPDLOG_INFO(
          "Font load summary: face={} loaded={} dup={} total={}", face,
          num_loaded, num_dup, num_total);
      if (num_dup == num_total && dup_candidate >= 0 &&
          dup_candidate < (int)c->loaded_font.size()) {
        FL_FontMatch m;
        const FL_FontMatch &ref = c->loaded_font[dup_candidate];
        m.flag = (FL_MatchFlag)(FL_LOAD_DUP | ref.flag);
        m.face = face;
        m.filename = ref.filename;
        if (!fl_try_push_loaded(c, m))
          return FL_OUT_OF_MEMORY;
      }
    }
  }
  if (c->loaded_font.size() > 1) {
    FL_FontMatch *data = c->loaded_font.data();
    tlx::parallel_mergesort(
        data, data + c->loaded_font.size(),
        [](const FL_FontMatch &a, const FL_FontMatch &b) {
          return fl_load_rec_sort(&a, &b, nullptr) < 0;
        },
        fl_background_sort_threads());
  }

  SPDLOG_INFO(
      "fl_load_fonts_incremental done: loaded={} failed={} unmatched={} "
      "total_records={}",
      c->num_font_loaded.load(std::memory_order_relaxed),
      c->num_font_failed.load(std::memory_order_relaxed),
      c->num_font_unmatched.load(std::memory_order_relaxed),
      c->loaded_font.size());
  return FL_OK;
}

int fl_walk_loaded_fonts(FL_LoaderCtx *c, WalkLoadedCallback cb, void *param) {
  if (c->font_path.empty())
    return FL_OK;

  std::wstring base = c->font_path;
  if (!base.empty() && base.back() != L'\\')
    base += L'\\';

  std::wstring path_buf;
  for (size_t i = 0; i != c->loaded_font.size(); i++) {
    const wchar_t *path = nullptr;
    const FL_FontMatch &m = c->loaded_font[i];
    if (!m.filename.empty()) {
      std::wstring file_w;
      if (Utf8ToUtf16(m.filename.c_str(), &file_w)) {
        path_buf = base + file_w;
        path = path_buf.c_str();
      }
    }
    const int ret = cb(c, i, path, param);
    if (ret != FL_OK)
      return ret;
  }

  return FL_OK;
}

static int
fl_unload_cb(FL_LoaderCtx *c, size_t i, const wchar_t *path, void *param) {
  FL_FontMatch *m = &c->loaded_font[i];
  if (!(m->flag & FL_LOAD_DUP)) {
    if (m->flag & FL_LOAD_OK) {
      c->num_font_loaded.fetch_sub(1, std::memory_order_relaxed);
    }
    RemoveFontResource(path);
    if (MOCK_DELAY_FONT) {
      Sleep(MOCK_DELAY_FONT);
    }
  }
  return 0;
}

int fl_unload_fonts(FL_LoaderCtx *c) {
  SPDLOG_INFO("fl_unload_fonts start: total_records={}", c->loaded_font.size());
  fl_walk_loaded_fonts(c, fl_unload_cb, nullptr);
  c->loaded_font.clear();
  c->num_font_loaded.store(0, std::memory_order_relaxed);
  c->num_font_failed.store(0, std::memory_order_relaxed);
  c->num_font_unmatched.store(0, std::memory_order_relaxed);

  SPDLOG_INFO("fl_unload_fonts done");
  return FL_OK;
}

static int
fl_cache_cb(FL_LoaderCtx *c, size_t i, const wchar_t *path, void *param) {
  FL_FontMatch *m = &c->loaded_font[i];
  if (m->flag & FL_LOAD_DUP) {
    return FL_OK;
  }

  HANDLE evt_cancel = *(HANDLE *)param;
  if (WaitForSingleObject(evt_cancel, 0) != WAIT_TIMEOUT) {
    return FL_OS_ERROR;
  }

  // read the file
  memmap_t mmap;
  DWORD tick = 0;
  FlMemMap(path, &mmap);
  const size_t step = 4 * 1024;
  volatile char chksum = 0;
  volatile const char *bytes = (const char *)mmap.data;
  for (size_t pos = 0; pos < mmap.size; pos += step) {
    const DWORD now = GetTickCount();
    if (now - tick > 10) {
      tick = now;
      if (WaitForSingleObject(evt_cancel, 0) != WAIT_TIMEOUT) {
        // loop will be terminated on next file
        break;
      }
    }
    chksum ^= bytes[pos];
  }
  FlMemUnmap(&mmap);
  return FL_OK;
}

int fl_cache_fonts(FL_LoaderCtx *c, HANDLE evt_cancel) {
  SPDLOG_INFO("fl_cache_fonts start");
  fl_walk_loaded_fonts(c, fl_cache_cb, &evt_cancel);
  SPDLOG_INFO("fl_cache_fonts done");
  return FL_OK;
}
