#pragma once

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

#include "util.h"
#include "font_set.h"

typedef enum {
  FL_OS_LOADED = 1,
  FL_LOAD_OK = 2,
  FL_LOAD_ERR = 16,
  FL_LOAD_DUP = 4,
  FL_LOAD_MISS = 8
} FL_MatchFlag;

typedef struct {
  FL_MatchFlag flag;
  std::string face;
  std::string filename;
  uint8_t hash[32];
} FL_FontMatch;

typedef struct {
  allocator_t *alloc;
  std::vector<std::string> sub_fonts;
  std::unordered_set<std::string> sub_font_set;
  // Lowercase UTF-8 absolute paths to avoid re-processing.
  std::unordered_set<std::string> loaded_sub_files;
  std::wstring font_path;
  std::wstring walk_path;
  FS_Set *font_set;

  std::atomic<uint32_t> num_sub;
  std::atomic<uint32_t> num_sub_font;
  std::atomic<uint32_t> num_font_loaded;
  std::atomic<uint32_t> num_font_failed;
  std::atomic<uint32_t> num_font_unmatched;

  void *event_cancel;
  void *hash_alg;
  std::vector<FL_FontMatch> loaded_font;
} FL_LoaderCtx;

int fl_init(FL_LoaderCtx *c, allocator_t *alloc);

int fl_free(FL_LoaderCtx *c);

int fl_cancel(FL_LoaderCtx *c);

int fl_add_subs(FL_LoaderCtx *c, const wchar_t *path);

int fl_scan_fonts(
    FL_LoaderCtx *c,
    const wchar_t *path,
    const wchar_t *cache,
    const wchar_t *black);

int fl_save_cache(FL_LoaderCtx *c, const wchar_t *cache);

int fl_load_fonts(FL_LoaderCtx *c);

int fl_load_fonts_incremental(FL_LoaderCtx *c);

int fl_unload_fonts(FL_LoaderCtx *c);

int fl_cache_fonts(FL_LoaderCtx *c, HANDLE evt_cancel);

typedef int (*WalkLoadedCallback)(
    FL_LoaderCtx *c,
    size_t i,
    const wchar_t *path,
    void *param);

int fl_walk_loaded_fonts(FL_LoaderCtx *c, WalkLoadedCallback cb, void *param);
