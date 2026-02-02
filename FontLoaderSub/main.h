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

#include <string>
#include <unordered_set>

#include "res/resource.h"
#include "font_loader.h"
#include "shortcut.h"

typedef enum {
  APP_LOAD_SUB = IDS_WORK_SUBTITLE,
  APP_LOAD_CACHE = IDS_WORK_CACHE,
  APP_SCAN_FONT = IDS_WORK_FONT,
  APP_LOAD_FONT = IDS_WORK_LOAD,
  APP_UNLOAD_FONT = IDS_WORK_UNLOAD,
  APP_DONE = IDS_WORK_DONE,
  APP_CANCELLED
} FL_AppState;

typedef struct {
  HINSTANCE hInst;
  allocator_t *alloc;
  int argc;
  LPWSTR *argv;

  int cancelled;
  int error;
  int req_exit;
  FL_LoaderCtx loader;
  FL_AppState app_state;
  wchar_t status_txt[256];  // should be sufficient
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
  TASKDIALOGCONFIG dlg_help;
  HMENU btn_menu;        // handle to the menu
  HWND handle_btn_menu;  // handle to the button
  int show_shortcut;
  FL_ShortCtx shortcut;
  ITaskbarList3 *taskbar_list3;
  std::unordered_set<std::wstring> loaded_subs;  // list of processed files
  int incremental_load;  // flag for incremental font loading
  LONG drop_guard;
  DWORD last_drop_tick;
  DWORD drop_debounce_ms;
} FL_AppCtx;
