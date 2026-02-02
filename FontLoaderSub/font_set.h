#pragma once

#include <stdint.h>
#include "util.h"

typedef struct _FS_Set FS_Set;

typedef struct FS_FontParseResult FS_FontParseResult;

typedef struct {
  uint32_t num_file;
  uint32_t num_face;
} FS_Stat;

// feel free to change its order (except for the last one)
typedef enum {
  FS_FmtNone,  // least preferred
  FS_FmtOTF,
  FS_FmtTTF,
  FS_FmtTTC,  // most preferred
  FS_FmtMax   // number of formats
} FS_Format;

typedef struct {
  const char *tag;
  const char *face;
  const char *ver;
  FS_Format format;
} FS_Index;

typedef struct {
  // private:
  FS_Set *set;
  uint32_t query_id;
  uint32_t index_id;
  // public:
  FS_Index info;
} FS_Iter;

int fs_create(allocator_t *alloc, FS_Set **out);

int fs_free(FS_Set *s);

int fs_stat(FS_Set *s, FS_Stat *stat);

int fs_add_font(FS_Set *s, const char *tag, void *buf, size_t size);

FS_FontParseResult *fs_parse_font_data(const uint8_t *buf, size_t size);

void fs_parse_font_free(FS_FontParseResult *result);

uint32_t fs_parse_result_face_count(const FS_FontParseResult *result);

int fs_add_parsed_font(
    FS_Set *s,
    const char *tag,
    const FS_FontParseResult *result);

int fs_build_index(FS_Set *s);

int fs_iter_new(FS_Set *s, const char *face, FS_Iter *it);

int fs_iter_next(FS_Iter *it);

int fs_cache_load(const wchar_t *path, allocator_t *alloc, FS_Set **out);

int fs_cache_dump(FS_Set *s, const wchar_t *path);

int fs_blacklist_clear(FS_Set *s);

int fs_blacklist_add(FS_Set *s, const char *path, size_t cch);

int fs_blacklist_match(FS_Set *S, const char *path);
