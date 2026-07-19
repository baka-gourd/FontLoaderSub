#pragma once

#ifndef UNICODE
#define UNICODE
#endif

#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef CINTERFACE
#define CINTERFACE
#endif

#include <Windows.h>
#include <tchar.h>
#include <CommCtrl.h>
#include <Shobjidl.h>

#include <atomic>
#include <string>
#include <unordered_set>
#include <vector>

#include "res/resource.h"
#include "font_loader.h"
#include "shortcut.h"

typedef enum {
  APP_LOAD_SUB = IDS_WORK_SUBTITLE,
  APP_LOAD_CACHE = IDS_WORK_CACHE,
  APP_SCAN_FONT = IDS_WORK_FONT,
  APP_CHECK_FONT = IDS_WORK_CHECK,
  APP_LOAD_FONT = IDS_WORK_LOAD,
  APP_UNLOAD_FONT = IDS_WORK_UNLOAD,
  APP_DONE = IDS_WORK_DONE,
  APP_CANCELLED,
  APP_CONFIRM_MISSING = 1000
} FL_AppState;

typedef struct {
  HINSTANCE hInst;
  allocator_t *alloc;
  int argc;
  LPWSTR *argv;

  std::atomic<int> cancelled;
  int error;
  std::atomic<int> req_exit;
  FL_LoaderCtx loader;
  std::atomic<FL_AppState> app_state;
  wchar_t status_txt[256];  // should be sufficient
  wchar_t work_status_txt[256];
  LPARAM work_caption_id;
  int work_status_initialized;
  std::wstring log;
  std::wstring font_path;
  std::wstring full_exe_path;

  HWND work_hwnd;
  HWND hwnd_message;  // hidden message-only window for drag-drop
  HANDLE thread_load;
  HANDLE thread_cache;
  HANDLE evt_stop_cache;

  TASKDIALOGCONFIG dlg_work;
  TASKDIALOGCONFIG dlg_done;
  TASKDIALOGCONFIG dlg_missing;
  TASKDIALOGCONFIG dlg_help;
  HMENU btn_menu;        // handle to the menu
  HWND handle_btn_menu;  // handle to the button
  int show_shortcut;
  FL_ShortCtx shortcut;
  ITaskbarList3 *taskbar_list3;
  std::unordered_set<std::wstring> loaded_subs;  // list of processed files
  std::vector<std::wstring> pending_paths;
  std::vector<std::wstring> batch_loaded_roots;
  std::unordered_set<std::string> batch_sub_font_set;
  std::unordered_set<std::string> batch_loaded_sub_files;
  size_t batch_sub_fonts_size;
  uint32_t batch_num_sub;
  uint32_t batch_num_sub_font;
  size_t batch_first_font;
  int batch_active;
  std::vector<std::string> missing_fonts;
  std::wstring missing_summary;
  std::wstring missing_text;
  int suppress_help_once;
  int incremental_load;  // flag for incremental font loading
  LONG drop_guard;
  DWORD last_drop_tick;
  DWORD drop_debounce_ms;
} FL_AppCtx;
