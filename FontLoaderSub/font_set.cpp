#include "font_set.h"

#include "font_db_generated.h"
#include "ttf_parser.h"
#include "util.h"
#include "utf.h"

#include "absl/strings/string_view.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <zstd.h>
#include <xxhash.h>

#include <tlx/sort/parallel_mergesort.hpp>

static size_t fs_background_sort_threads() {
  const unsigned int hw = std::thread::hardware_concurrency();
  if (hw <= 2u)
    return 1u;
  return (hw - 1u < 4u) ? hw - 1u : 4u;
}

typedef struct {
  std::string ver;
  uint16_t ver_lang_id;
  std::unordered_set<std::string> faces;
} FS_FontAccum;

typedef struct {
  std::string tag;
  std::string face;
  std::string ver;
  FS_Format format;
  uint64_t hash_low64;
  uint64_t hash_high64;
} FS_Entry;

struct FS_FontParseResult {
  FS_Format format;
  std::unordered_map<uint32_t, FS_FontAccum> fonts;
  uint32_t count_face;
  uint64_t hash_low64;
  uint64_t hash_high64;
};

struct _FS_Set {
  allocator_t *alloc;
  std::vector<FS_Entry> entries;
  std::vector<std::string> blacklist;
  std::vector<FS_Index> index_vec;
  FS_Index *index;
  FS_Stat stat;
};

typedef struct {
  FS_Set *set;
  std::unordered_map<uint32_t, FS_FontAccum> fonts;
  uint32_t count_face;
} FS_ParseCtx;

static int fs_tolower_ascii(int ch) {
  if ('A' <= ch && ch <= 'Z')
    return ch - 'A' + 'a';
  return ch;
}

static int fs_stricmp_ascii(const char *a, const char *b) {
  if (a == nullptr)
    return b ? -1 : 0;
  if (b == nullptr)
    return 1;
  while (*a && *b) {
    int da = fs_tolower_ascii(*a);
    int db = fs_tolower_ascii(*b);
    if (da != db)
      return da - db;
    ++a;
    ++b;
  }
  return fs_tolower_ascii(*a) - fs_tolower_ascii(*b);
}

static int fs_strncasecmp_ascii(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int ca = fs_tolower_ascii(a[i]);
    int cb = fs_tolower_ascii(b[i]);
    if (ca != cb)
      return ca - cb;
    if (a[i] == 0 || b[i] == 0)
      return ca - cb;
  }
  return 0;
}

static size_t fs_prefix_len_ci(const char *a, const char *b) {
  size_t i = 0;
  for (; a[i] && b[i]; i++) {
    int ca = fs_tolower_ascii(a[i]);
    int cb = fs_tolower_ascii(b[i]);
    if (ca != cb)
      break;
  }
  return i;
}

static int fs_is_digit(int ch) {
  return ('0' <= ch && ch <= '9') ? (ch - '0') : -1;
}

static int fs_version_cmp_utf8(const char *a, const char *b) {
  const char *ptr_a = a;
  const char *ptr_b = b;
  int cmp = 0;

  if (b == nullptr)
    return 1;
  if (a == nullptr)
    return -1;

  while (*ptr_a && *ptr_b && cmp == 0) {
    if (fs_is_digit(*ptr_a) >= 0 && fs_is_digit(*ptr_b) >= 0) {
      const char *start_a = ptr_a, *start_b = ptr_b;
      while (fs_is_digit(*ptr_a) >= 0)
        ptr_a++;
      while (fs_is_digit(*ptr_b) >= 0)
        ptr_b++;
      const char *dig_a = ptr_a, *dig_b = ptr_b;
      while (dig_a != start_a && dig_b != start_b) {
        dig_a--;
        dig_b--;
        cmp = *dig_a - *dig_b;
      }
      if (cmp == 0) {
        cmp = (int)((ptr_a - start_a) - (ptr_b - start_b));
      }
    } else {
      cmp = *ptr_a - *ptr_b;
      ptr_a++;
      ptr_b++;
    }
  }

  if (cmp == 0)
    cmp = *ptr_a - *ptr_b;
  return cmp;
}

static size_t fs_strnlen(const char *str, size_t n) {
  for (size_t i = 0; i != n; i++) {
    if (str[i] == 0)
      return i;
  }
  return n;
}

static bool
fs_utf16be_to_utf8(const wchar_t *str, size_t cch, std::string *out) {
  if (out == nullptr)
    return false;
  out->clear();
  if (cch == 0)
    return true;

  std::wstring tmp;
  tmp.resize(cch);
  const uint16_t *src = (const uint16_t *)str;
  for (size_t i = 0; i != cch; i++) {
    tmp[i] = (wchar_t)be16(src[i]);
  }
  return Utf16ToUtf8(tmp, out);
}

static int fs_parser_name_cb(
    uint32_t font_id,
    OTF_NameRecord *r,
    const wchar_t *str,
    void *arg) {
  FS_ParseCtx *c = (FS_ParseCtx *)arg;
  const uint32_t cch = be16(r->length) / sizeof str[0];
  if (cch == 0)
    return FL_OK;

  FS_FontAccum &font = c->fonts[font_id];
  if (r->name_id == be16(5)) {
    if (font.ver_lang_id == 0 || r->lang_id == be16(0x0409)) {
      std::string ver;
      if (!fs_utf16be_to_utf8(str, cch, &ver))
        return FL_OUT_OF_MEMORY;
      font.ver = ver;
      font.ver_lang_id = r->lang_id;
    }
  } else {
    std::string face;
    if (!fs_utf16be_to_utf8(str, cch, &face))
      return FL_OUT_OF_MEMORY;
    if (!face.empty()) {
      if (font.faces.insert(face).second) {
        c->count_face++;
      }
    }
  }

  return FL_OK;
}

static int fs_idx_comp(const FS_Index &a, const FS_Index &b) {
  int cmp = fs_stricmp_ascii(a.face, b.face);
  if (cmp == 0) {
    cmp = 0 - (int)(a.format - b.format);
    if (cmp == 0) {
      cmp = 0 - fs_version_cmp_utf8(a.ver, b.ver);
    }
  }
  return cmp;
}

int fs_create(allocator_t *alloc, FS_Set **out) {
  if (out == nullptr)
    return FL_OUT_OF_MEMORY;
  void *mem = alloc->alloc(nullptr, sizeof(FS_Set), alloc->arg);
  if (!mem) {
    *out = nullptr;
    return FL_OUT_OF_MEMORY;
  }
  FS_Set *s = new (mem) FS_Set();
  s->alloc = alloc;
  s->index = nullptr;
  s->stat = {};
  *out = s;
  return FL_OK;
}

int fs_free(FS_Set *s) {
  if (s) {
    allocator_t *alloc = s->alloc;
    s->~FS_Set();
    alloc->alloc(s, 0, alloc->arg);
  }
  return FL_OK;
}

int fs_stat(FS_Set *s, FS_Stat *stat) {
  if (s && stat) {
    *stat = s->stat;
  }
  return 0;
}

FS_FontParseResult *fs_parse_font_data(const uint8_t *buf, size_t size) {
  if (buf == nullptr || size == 0)
    return nullptr;

  int r = FL_OK;
  int ok = 0;
  FS_Format fmt = FS_FmtNone;
  FS_ParseCtx ctx = {};

  do {
    r = ttc_parse(buf, size, fs_parser_name_cb, &ctx);
    if (r == FL_OK && ctx.count_face > 0) {
      fmt = FS_FmtTTC;
      ok = 1;
      break;
    }

    ctx.fonts.clear();
    ctx.count_face = 0;
    fmt = (buf[0] == 'O') ? FS_FmtOTF : FS_FmtTTF;
    r = otf_parse(buf, size, fs_parser_name_cb, &ctx);
    if (r == FL_OK && ctx.count_face > 0) {
      ok = 1;
      break;
    }
  } while (0);

  if (!ok)
    return nullptr;

  FS_FontParseResult *res = new (std::nothrow) FS_FontParseResult();
  if (res == nullptr)
    return nullptr;
  res->format = fmt;
  res->fonts = std::move(ctx.fonts);
  res->count_face = ctx.count_face;
  const XXH128_hash_t hash = XXH3_128bits(buf, size);
  res->hash_low64 = hash.low64;
  res->hash_high64 = hash.high64;
  return res;
}

void fs_parse_font_free(FS_FontParseResult *result) {
  delete result;
}

uint32_t fs_parse_result_face_count(const FS_FontParseResult *result) {
  if (!result) {
    return 0;
  }
  return result->count_face;
}

int fs_add_parsed_font(
    FS_Set *s,
    const char *tag,
    const FS_FontParseResult *result) {
  if (s == nullptr || tag == nullptr)
    return FL_UNRECOGNIZED;

  s->stat.num_file++;
  if (result == nullptr || result->count_face == 0)
    return FL_UNRECOGNIZED;

  for (const auto &item : result->fonts) {
    const FS_FontAccum &font = item.second;
    for (const auto &face : font.faces) {
      FS_Entry e;
      e.tag = tag;
      e.face = face;
      e.ver = font.ver;
      e.format = result->format;
      e.hash_low64 = result->hash_low64;
      e.hash_high64 = result->hash_high64;
      s->entries.push_back(e);
    }
  }
  s->stat.num_face += result->count_face;
  return FL_OK;
}

int fs_add_font(FS_Set *s, const char *tag, void *buf, size_t size) {
  if (s == nullptr || tag == nullptr || buf == nullptr || size == 0)
    return FL_UNRECOGNIZED;

  FS_FontParseResult *parsed = fs_parse_font_data((const uint8_t *)buf, size);
  const int r = fs_add_parsed_font(s, tag, parsed);
  fs_parse_font_free(parsed);
  return r;
}

int fs_build_index(FS_Set *s) {
  if (s == nullptr)
    return FL_OUT_OF_MEMORY;

  s->stat.num_face = (uint32_t)s->entries.size();
  s->index_vec.clear();
  s->index_vec.reserve(s->entries.size());
  for (const auto &e : s->entries) {
    FS_Index idx = {};
    idx.tag = e.tag.c_str();
    idx.face = e.face.c_str();
    idx.ver = e.ver.empty() ? nullptr : e.ver.c_str();
    idx.format = e.format;
    idx.hash_low64 = e.hash_low64;
    idx.hash_high64 = e.hash_high64;
    s->index_vec.push_back(idx);
  }

  if (s->index_vec.size() > 1) {
    FS_Index *data = s->index_vec.data();
    tlx::parallel_mergesort(
        data, data + s->index_vec.size(),
        [](const FS_Index &a, const FS_Index &b) {
          return fs_idx_comp(a, b) < 0;
        },
        fs_background_sort_threads());
  }

  s->index = s->index_vec.empty() ? nullptr : s->index_vec.data();
  return FL_OK;
}

int fs_iter_new(FS_Set *s, const char *face, FS_Iter *it) {
  if (s == nullptr || s->index == nullptr || it == nullptr)
    return 0;
  int a = 0, b = (int)s->stat.num_face - 1;
  int m = 0;
  if (s->index != nullptr && s->stat.num_face != 0) {
    while (a <= b) {
      m = a + (b - a) / 2;
      const char *got = s->index[m].face;
      const int t = fs_stricmp_ascii(face, got);
      if (t == 0) {
        a = b = m;
        break;
      }
      if (t > 0) {
        a = m + 1;
      } else {
        b = m - 1;
      }
    }
  }

  do {
    if (!(a == b && a == m)) {
      break;
    }
    while (m > 0 && fs_stricmp_ascii(face, s->index[m - 1].face) == 0) {
      m--;
    }
    while (m != (int)s->stat.num_face &&
           fs_blacklist_match(s, s->index[m].tag)) {
      m++;
    }
    if (m == (int)s->stat.num_face) {
      break;
    }
    it->set = s;
    it->query_id = m;
    it->index_id = m;
    it->info = s->index[m];
    return 1;
  } while (0);

  it->set = nullptr;
  it->query_id = 0;
  it->index_id = 0;
  return 0;
}

int fs_iter_next(FS_Iter *it) {
  FS_Set *s = it->set;
  if (s == nullptr)
    return 0;
  if (it->index_id == s->stat.num_face)
    return 0;
  it->index_id++;
  const char *face = s->index[it->query_id].face;
  const char *ver = s->index[it->query_id].ver;
  FS_Format fmt = s->index[it->query_id].format;

  for (; it->index_id != s->stat.num_face; it->index_id++) {
    const char *got_face = s->index[it->index_id].face;
    const char *got_ver = s->index[it->index_id].ver;
    FS_Format got_fmt = s->index[it->index_id].format;

    const size_t df = fs_prefix_len_ci(face, got_face);
    if (face[df] != 0) {
      break;
    }

    if (fmt != got_fmt) {
      continue;
    }

    if (ver == nullptr) {
      if (got_ver != nullptr)
        continue;
    } else {
      if (got_ver == nullptr)
        continue;
      if (absl::string_view(ver) != got_ver)
        continue;
    }

    if (fs_blacklist_match(s, s->index[it->index_id].tag)) {
      continue;
    }

    it->info = s->index[it->index_id];
    return 1;
  }

  it->set = nullptr;
  return 0;
}

int fs_walk_index(FS_Set *s, FS_WalkIndexCallback cb, void *param) {
  if (s == nullptr || cb == nullptr)
    return FL_OS_ERROR;
  for (const auto &info : s->index_vec) {
    const int r = cb(&info, param);
    if (r != FL_OK)
      return r;
  }
  return FL_OK;
}

int fs_cache_load(const wchar_t *path, allocator_t *alloc, FS_Set **out) {
  if (out == nullptr)
    return FL_OUT_OF_MEMORY;
  *out = nullptr;

  memmap_t map = {nullptr};
  int r = FlMemMap(path, &map);
  if (map.data == nullptr)
    return FL_OS_ERROR;

  const unsigned long long content_size =
      ZSTD_getFrameContentSize(map.data, map.size);
  if (content_size == ZSTD_CONTENTSIZE_ERROR ||
      content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
    FlMemUnmap(&map);
    return FL_UNRECOGNIZED;
  }
  if (content_size > static_cast<unsigned long long>(SIZE_MAX)) {
    FlMemUnmap(&map);
    return FL_OUT_OF_MEMORY;
  }

  std::vector<uint8_t> decompressed;
  decompressed.resize((size_t)content_size);
  size_t decompressed_size = ZSTD_decompress(
      decompressed.data(), decompressed.size(), map.data, map.size);
  FlMemUnmap(&map);
  if (ZSTD_isError(decompressed_size) ||
      decompressed_size != decompressed.size()) {
    return FL_CORRUPTED;
  }

  flatbuffers::Verifier verifier(decompressed.data(), decompressed.size());
  if (!fontloader::VerifyFontDbBuffer(verifier)) {
    return FL_UNRECOGNIZED;
  }

  const fontloader::FontDb *db = fontloader::GetFontDb(decompressed.data());
  if (db == nullptr) {
    return FL_CORRUPTED;
  }
  if (db->version() != 2) {
    return FL_UNRECOGNIZED;
  }

  FS_Set *s = nullptr;
  r = fs_create(alloc, &s);
  if (r != FL_OK) {
    return r;
  }

  const auto *entries = db->entries();
  if (entries) {
    s->entries.reserve(entries->size());
    for (uint32_t i = 0; i != entries->size(); i++) {
      const auto *entry = entries->Get(i);
      if (entry == nullptr || entry->tag() == nullptr ||
          entry->face() == nullptr) {
        fs_free(s);
        return FL_CORRUPTED;
      }
      FS_Entry e;
      e.tag.assign(entry->tag()->c_str(), entry->tag()->size());
      e.face.assign(entry->face()->c_str(), entry->face()->size());
      if (entry->ver())
        e.ver.assign(entry->ver()->c_str(), entry->ver()->size());
      e.format = (FS_Format)entry->format();
      e.hash_low64 = entry->hash_low64();
      e.hash_high64 = entry->hash_high64();
      s->entries.push_back(e);
    }
  }

  s->stat.num_face = (uint32_t)s->entries.size();
  s->stat.num_file = db->num_file();
  if (s->stat.num_file == 0) {
    std::unordered_set<std::string> tags;
    tags.reserve(s->entries.size());
    for (const auto &e : s->entries)
      tags.insert(e.tag);
    s->stat.num_file = (uint32_t)tags.size();
  }

  r = fs_build_index(s);
  if (r != FL_OK) {
    fs_free(s);
    return r;
  }
  *out = s;
  return FL_OK;
}

int fs_cache_dump(FS_Set *s, const wchar_t *path) {
  if (s == nullptr)
    return FL_OS_ERROR;

  DWORD flags = FILE_ATTRIBUTE_NORMAL;
  if (s->stat.num_file == 0)
    flags |= FILE_FLAG_DELETE_ON_CLOSE;

  HANDLE h = CreateFile(
      path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, flags,
      nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return FL_OS_ERROR;

  flatbuffers::FlatBufferBuilder builder(1024);
  std::vector<flatbuffers::Offset<fontloader::FontEntry>> entries_vec;
  entries_vec.reserve(s->entries.size());
  for (const auto &e : s->entries) {
    auto tag = builder.CreateString(e.tag);
    auto face = builder.CreateString(e.face);
    flatbuffers::Offset<flatbuffers::String> ver = 0;
    if (!e.ver.empty())
      ver = builder.CreateString(e.ver);
    auto entry = fontloader::CreateFontEntry(
        builder, tag, face, ver, (fontloader::FontFormat)e.format,
        e.hash_low64, e.hash_high64);
    entries_vec.push_back(entry);
  }

  auto entries = builder.CreateVector(entries_vec);
  auto db = fontloader::CreateFontDb(
      builder, 2, s->stat.num_file, s->stat.num_face, entries);
  builder.Finish(db, fontloader::FontDbIdentifier());

  const size_t src_size = builder.GetSize();
  const size_t max_dst = ZSTD_compressBound(src_size);
  std::vector<uint8_t> compressed;
  compressed.resize(max_dst);
  size_t compressed_size = ZSTD_compress(
      compressed.data(), compressed.size(), builder.GetBufferPointer(),
      src_size, 6);
  if (ZSTD_isError(compressed_size)) {
    CloseHandle(h);
    return FL_OS_ERROR;
  }
  if (compressed_size > static_cast<size_t>(MAXDWORD)) {
    CloseHandle(h);
    return FL_OUT_OF_MEMORY;
  }

  DWORD written = 0;
  BOOL ok = WriteFile(
      h, compressed.data(), (DWORD)compressed_size, &written, nullptr);
  CloseHandle(h);
  return ok ? FL_OK : FL_OS_ERROR;
}

int fs_blacklist_clear(FS_Set *s) {
  if (s)
    s->blacklist.clear();
  return 0;
}

int fs_blacklist_add(FS_Set *s, const char *path, size_t cch) {
  if (s == nullptr || path == nullptr)
    return 1;
  const size_t len =
      cch ? fs_strnlen(path, cch) : absl::string_view(path).size();
  if (len == 0)
    return 0;
  s->blacklist.emplace_back(path, len);
  return 0;
}

int fs_blacklist_match(FS_Set *s, const char *path) {
  if (s == nullptr || path == nullptr)
    return 0;
  const size_t len_path = absl::string_view(path).size();
  for (const auto &suffix : s->blacklist) {
    const size_t len_suffix = suffix.size();
    if (len_suffix > len_path)
      continue;
    const char *path_sfx = path + len_path - len_suffix;
    if (fs_strncasecmp_ascii(path_sfx, suffix.c_str(), len_suffix) == 0) {
      if (len_path == len_suffix || path_sfx[-1] == '\\') {
        return 1;
      }
    }
  }
  return 0;
}
