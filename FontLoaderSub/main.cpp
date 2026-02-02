#include "main.h"

#include <atomic>
#include <string>

#include "ass_string.h"
#include "exporter.h"
#include "log.h"
#include "util.h"
#include "shortcut.h"
#include "mock_config.h"
#include "res/resource.h"
#include "utf.h"

#define kCacheFile L"fc-subs.ftdb"
#define kBlackFile L"fc-ignore.txt"
#define kMessageWindowClass L"FontLoaderSubMessageWindow"

static DWORD WINAPI app_worker(LPVOID param);
static LRESULT CALLBACK DoneDialogSubclassProc(
    HWND hWnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR uIdSubclass,
    DWORD_PTR dwRefData);

static std::wstring NormalizeSubtitlePath(const wchar_t *filePath) {
  wchar_t fullPath[MAX_PATH * 2];
  if (GetFullPathName(filePath, _countof(fullPath), fullPath, nullptr) == 0) {
    return {};
  }
  std::wstring normalized(fullPath);
  if (!normalized.empty()) {
    CharLowerBuffW(&normalized[0], static_cast<DWORD>(normalized.size()));
  }
  return normalized;
}

static int IsSubtitleFileLoaded(FL_AppCtx *c, const wchar_t *filePath) {
  std::wstring normalized = NormalizeSubtitlePath(filePath);
  if (normalized.empty())
    return 0;
  return c->loaded_subs.find(normalized) != c->loaded_subs.end();
}

static int AddSubtitleFileToLoaded(FL_AppCtx *c, const wchar_t *filePath) {
  std::wstring normalized = NormalizeSubtitlePath(filePath);
  if (normalized.empty())
    return 0;
  return c->loaded_subs.insert(std::move(normalized)).second ? 1 : 0;
}

static void *mem_realloc(void *existing, size_t size, void *arg) {
  auto heap = (HANDLE)arg;
  if (size == 0) {
    HeapFree(heap, 0, existing);
    return nullptr;
  }
  if (existing == nullptr) {
    return HeapAlloc(heap, HEAP_ZERO_MEMORY, size);
  }
  return HeapReAlloc(heap, HEAP_ZERO_MEMORY, existing, size);
}

static LRESULT CALLBACK
MessageWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  auto c = (FL_AppCtx *)GetWindowLongPtr(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_CREATE:
    return 0;
  case WM_DROPFILES:
    if (c && c->app_state == APP_DONE) {
      auto hDrop = (HDROP)wParam;
      if (InterlockedExchange(&c->drop_guard, 1) != 0) {
        DragFinish(hDrop);
        return 0;
      }
      DWORD now_tick = GetTickCount();
      if (now_tick - c->last_drop_tick < c->drop_debounce_ms) {
        DragFinish(hDrop);
        InterlockedExchange(&c->drop_guard, 0);
        return 0;
      }
      c->last_drop_tick = now_tick;
      UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, nullptr, 0);
      int has_new_files = 0;

      for (UINT i = 0; i < fileCount; i++) {
        UINT fileNameLen = DragQueryFile(hDrop, i, nullptr, 0);
        if (fileNameLen > 0) {
          std::wstring fileName;
          fileName.resize(fileNameLen + 1);
          UINT written = DragQueryFile(hDrop, i, &fileName[0], fileNameLen + 1);
          fileName.resize(written);

          // Check if subtitle file is already loaded
          if (!IsSubtitleFileLoaded(c, fileName.c_str())) {
            int r = fl_add_subs(&c->loader, fileName.c_str());
            if (r == FL_OK) {
              AddSubtitleFileToLoaded(c, fileName.c_str());
              std::string file_u8;
              if (Utf16ToUtf8(fileName.c_str(), &file_u8)) {
                SPDLOG_INFO("New subtitle loaded from drag-drop: {}", file_u8);
              } else {
                SPDLOG_INFO("New subtitle loaded from drag-drop (utf16)");
              }
              has_new_files = 1;
            } else {
              std::string file_u8;
              if (Utf16ToUtf8(fileName.c_str(), &file_u8)) {
                SPDLOG_INFO(
                    "Failed to add subtitle from drag-drop: {} (err={})",
                    file_u8, r);
              } else {
                SPDLOG_INFO(
                    "Failed to add subtitle from drag-drop (err={})", r);
              }
            }
          } else {
            std::string file_u8;
            if (Utf16ToUtf8(fileName.c_str(), &file_u8)) {
              SPDLOG_INFO("Skip already loaded subtitle: {}", file_u8);
            } else {
              SPDLOG_INFO("Skip already loaded subtitle (utf16)");
            }
          }
        }
      }
      DragFinish(hDrop);

      // Only reload if we have new files
      if (has_new_files) {
        if (c->thread_load != nullptr &&
            WaitForSingleObject(c->thread_load, 0) == WAIT_TIMEOUT) {
          SPDLOG_INFO("Drop ignored: worker already running");
          InterlockedExchange(&c->drop_guard, 0);
          return 0;
        }
        // Load fonts for new subtitles only (don't unload existing fonts)
        c->app_state = APP_LOAD_FONT;
        c->incremental_load = 1;  // Use incremental loading
        c->cancelled = 0;
        c->req_exit = 0;

        DWORD thread_id;
        c->thread_load = CreateThread(nullptr, 0, app_worker, c, 0, &thread_id);
        if (c->thread_load != nullptr && c->work_hwnd) {
          SendMessage(c->work_hwnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
        }
      }
      InterlockedExchange(&c->drop_guard, 0);
    }
    return 0;
  case WM_CLOSE:
    DestroyWindow(hWnd);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  default:
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
  }
}

// Subclass procedure for the done dialog to handle drag-drop
static LRESULT CALLBACK DoneDialogSubclassProc(
    HWND hWnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR uIdSubclass,
    DWORD_PTR dwRefData) {
  auto c = (FL_AppCtx *)dwRefData;

  switch (uMsg) {
  case WM_DROPFILES:
    if (c && c->app_state == APP_DONE) {
      auto hDrop = (HDROP)wParam;
      if (InterlockedExchange(&c->drop_guard, 1) != 0) {
        DragFinish(hDrop);
        return 0;
      }
      DWORD now_tick = GetTickCount();
      if (now_tick - c->last_drop_tick < c->drop_debounce_ms) {
        DragFinish(hDrop);
        InterlockedExchange(&c->drop_guard, 0);
        return 0;
      }
      c->last_drop_tick = now_tick;
      UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, nullptr, 0);
      int has_new_files = 0;

      for (UINT i = 0; i < fileCount; i++) {
        UINT fileNameLen = DragQueryFile(hDrop, i, nullptr, 0);
        if (fileNameLen > 0) {
          std::wstring fileName;
          fileName.resize(fileNameLen + 1);
          UINT written = DragQueryFile(hDrop, i, &fileName[0], fileNameLen + 1);
          fileName.resize(written);

          // Check if subtitle file is already loaded
          if (!IsSubtitleFileLoaded(c, fileName.c_str())) {
            int r = fl_add_subs(&c->loader, fileName.c_str());
            if (r == FL_OK) {
              AddSubtitleFileToLoaded(c, fileName.c_str());
              std::string file_u8;
              if (Utf16ToUtf8(fileName.c_str(), &file_u8)) {
                SPDLOG_INFO(
                    "New subtitle loaded from done dialog: {}", file_u8);
              } else {
                SPDLOG_INFO("New subtitle loaded from done dialog (utf16)");
              }
              has_new_files = 1;
            } else {
              std::string file_u8;
              if (Utf16ToUtf8(fileName.c_str(), &file_u8)) {
                SPDLOG_INFO(
                    "Failed to add subtitle from done dialog: {} (err={})",
                    file_u8, r);
              } else {
                SPDLOG_INFO(
                    "Failed to add subtitle from done dialog (err={})", r);
              }
            }
          } else {
            std::string file_u8;
            if (Utf16ToUtf8(fileName.c_str(), &file_u8)) {
              SPDLOG_INFO("Skip already loaded subtitle: {}", file_u8);
            } else {
              SPDLOG_INFO("Skip already loaded subtitle (utf16)");
            }
          }
        }
      }
      DragFinish(hDrop);

      // Only reload if we have new files
      if (has_new_files) {
        if (c->thread_load != nullptr &&
            WaitForSingleObject(c->thread_load, 0) == WAIT_TIMEOUT) {
          SPDLOG_INFO("Drop ignored: worker already running");
          InterlockedExchange(&c->drop_guard, 0);
          return 0;
        }
        // Load fonts for new subtitles only (don't unload existing fonts)
        c->app_state = APP_LOAD_FONT;
        c->incremental_load = 1;  // Use incremental loading
        c->cancelled = 0;
        c->req_exit = 0;

        // Disable drag-drop before navigating away
        DragAcceptFiles(hWnd, FALSE);

        DWORD thread_id;
        c->thread_load = CreateThread(nullptr, 0, app_worker, c, 0, &thread_id);
        if (c->thread_load != nullptr) {
          SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
        }
      }
      InterlockedExchange(&c->drop_guard, 0);
    }
    return 0;
  default:
    break;
  }

  return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

static int AppCreateMessageWindow(FL_AppCtx *c) {
  WNDCLASSEXW wc = {0};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = MessageWindowProc;
  wc.hInstance = c->hInst;
  wc.lpszClassName = kMessageWindowClass;

  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return 0;
  }

  c->hwnd_message = CreateWindowExW(
      0, kMessageWindowClass, L"FontLoaderSubMessageWindow", 0, 0, 0, 0, 0,
      HWND_MESSAGE, nullptr, c->hInst, nullptr);

  if (c->hwnd_message == nullptr) {
    return 0;
  }

  SetWindowLongPtr(c->hwnd_message, GWLP_USERDATA, (LONG_PTR)c);
  return 1;
}

static void AppHelpUsage(FL_AppCtx *c, HWND hWnd) {
  c->show_shortcut = 0;
  c->dlg_help.hwndParent = hWnd;
  TaskDialogIndirect(&c->dlg_help, nullptr, nullptr, nullptr);
  if (c->show_shortcut) {
    ShortcutShow(&c->shortcut, hWnd);
  }
}

static int AppBuildLog(FL_AppCtx *c) {
  const auto &loaded = c->loader.loaded_font;
  c->log.clear();

  for (const auto &m : loaded) {
    const wchar_t *tag;
    if (m.flag & (FL_LOAD_DUP))
      tag = L"[^ ] ";
    else if (m.flag & (FL_OS_LOADED | FL_LOAD_OK))
      tag = L"[ok] ";
    else if (m.flag & (FL_LOAD_ERR))
      tag = L"[ X] ";
    else
      tag = L"[??] ";

    std::wstring face_w;
    if (!m.face.empty() && !Utf8ToUtf16(m.face.c_str(), &face_w))
      return 0;

    c->log.append(tag);
    c->log.append(face_w);

    if (!m.filename.empty() && !(m.flag & FL_LOAD_DUP)) {
      std::wstring file_w;
      if (!Utf8ToUtf16(m.filename.c_str(), &file_w))
        return 0;
      c->log.append(L" > ");
      c->log.append(file_w);
    }
    c->log.append(L"\n");
  }
  if (!c->log.empty())
    c->log.pop_back();
  return !c->log.empty() ? 1 : 0;
}

static int AppUpdateStatus(FL_AppCtx *c) {
  FS_Stat stat = {0};
  if (c->app_state == APP_SCAN_FONT) {
    stat.num_file = c->loader.num_scan_file.load(std::memory_order_relaxed);
    stat.num_face = c->loader.num_scan_face.load(std::memory_order_relaxed);
  } else if (c->loader.font_set && c->app_state != APP_LOAD_CACHE) {
    fs_stat(c->loader.font_set, &stat);
  }

  const DWORD_PTR loaded = static_cast<DWORD_PTR>(
      c->loader.num_font_loaded.load(std::memory_order_relaxed));
  const DWORD_PTR failed = static_cast<DWORD_PTR>(
      c->loader.num_font_failed.load(std::memory_order_relaxed));
  const DWORD_PTR unmatched = static_cast<DWORD_PTR>(
      c->loader.num_font_unmatched.load(std::memory_order_relaxed));
  const DWORD_PTR subs =
      static_cast<DWORD_PTR>(c->loader.num_sub.load(std::memory_order_relaxed));
  DWORD_PTR args[] = {
      // arguments
      loaded, failed, unmatched, stat.num_file, stat.num_face, subs,
  };
  FormatMessage(
      FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY,
      ResLoadString(c->hInst, IDS_LOAD_STAT), 0, 0, c->status_txt,
      _countof(c->status_txt), reinterpret_cast<va_list *>(args));

  LPARAM cap_id;
  if (c->cancelled || c->app_state == APP_CANCELLED) {
    cap_id = IDS_WORK_CANCELLING;
  } else {
    cap_id = c->app_state;
  }

  SendMessage(c->work_hwnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, cap_id);
  SendMessage(
      c->work_hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT,
      reinterpret_cast<LPARAM>(c->status_txt));

  return 0;
}

static DWORD WINAPI app_worker(LPVOID param) {
  auto c = static_cast<FL_AppCtx *>(param);
  int r = FL_OK;
  SPDLOG_INFO("AppWorker start");
  while (r == FL_OK && !c->cancelled && c->app_state != APP_DONE) {
    switch (c->app_state) {
    case APP_LOAD_SUB: {
      SPDLOG_INFO("State: APP_LOAD_SUB");
      if (MOCK_SUB_PATH) {
        r = fl_add_subs(&c->loader, nullptr);
        if (r == FL_OK) {
          AddSubtitleFileToLoaded(c, nullptr);
        }
      }
      for (int i = 1; i < c->argc && r == FL_OK; i++) {
        r = fl_add_subs(&c->loader, c->argv[i]);
        if (r == FL_OK) {
          AddSubtitleFileToLoaded(c, c->argv[i]);
        }
      }
      SPDLOG_INFO("APP_LOAD_SUB done, r={}", r);
      c->app_state = APP_LOAD_CACHE;
      break;
    }
    case APP_LOAD_CACHE: {
      SPDLOG_INFO("State: APP_LOAD_CACHE");
      fl_scan_fonts(&c->loader, c->font_path.c_str(), kCacheFile, kBlackFile);
      FS_Stat stat = {0};
      fs_stat(c->loader.font_set, &stat);
      if (stat.num_face == 0) {
        c->app_state = APP_SCAN_FONT;
      } else {
        c->app_state = APP_LOAD_FONT;
      }
      SPDLOG_INFO("APP_LOAD_CACHE done, faces={}", stat.num_face);
      break;
    }
    case APP_SCAN_FONT: {
      SPDLOG_INFO("State: APP_SCAN_FONT");
      if (fl_scan_fonts(
              &c->loader, c->font_path.c_str(), nullptr, kBlackFile) == FL_OK) {
        fl_save_cache(&c->loader, kCacheFile);
      }
      c->app_state = APP_LOAD_FONT;
      SPDLOG_INFO("APP_SCAN_FONT done");
      break;
    }
    case APP_LOAD_FONT: {
      SPDLOG_INFO("State: APP_LOAD_FONT incremental={}", c->incremental_load);
      if (c->incremental_load) {
        r = fl_load_fonts_incremental(&c->loader);
        c->incremental_load = 0;  // Reset flag
      } else {
        r = fl_load_fonts(&c->loader);
      }
      if (r == FL_OK)
        c->app_state = APP_DONE;
      SPDLOG_INFO("APP_LOAD_FONT done, r={}", r);
      break;
    }
    case APP_UNLOAD_FONT: {
      SPDLOG_INFO("State: APP_UNLOAD_FONT");
      fl_unload_fonts(&c->loader);
      if (c->req_exit) {
        c->cancelled = 1;
      } else {
        c->app_state = APP_SCAN_FONT;
      }
      SPDLOG_INFO("APP_UNLOAD_FONT done");
      break;
    }
    default: {
      // nop
    }
    }
  }
  if (c->cancelled) {
    fl_unload_fonts(&c->loader);
    c->app_state = APP_CANCELLED;
    SPDLOG_INFO("AppWorker cancelled");
  }

  SPDLOG_INFO("AppWorker exit r={}", r);
  return 0;
}

static DWORD WINAPI AppCacheWorker(LPVOID param) {
  auto c = static_cast<FL_AppCtx *>(param);

  while (true) {
    fl_cache_fonts(&c->loader, c->evt_stop_cache);
    if (WaitForSingleObject(c->evt_stop_cache, 5 * 60 * 1000) != WAIT_TIMEOUT)
      break;
  }
  return 0;
}

static HRESULT CALLBACK DlgWorkProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  auto c = (FL_AppCtx *)dwRefData;
  int navigated = 0;
  if (uNotification == TDN_CREATED || uNotification == TDN_NAVIGATED) {
    c->work_hwnd = hWnd;
    SendMessage(hWnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);

    if (c->thread_load != nullptr) {
      DWORD r = WaitForSingleObject(c->thread_load, 0);
      if (r != WAIT_TIMEOUT) {
        CloseHandle(c->thread_load);
        c->thread_load = nullptr;
      }
    }
    if (c->thread_load == nullptr) {
      DWORD thread_id;
      c->thread_load = CreateThread(nullptr, 0, app_worker, c, 0, &thread_id);
      if (c->thread_load == nullptr) {
        // fatal error, try exit early
        c->cancelled = 1;
        c->app_state = APP_CANCELLED;
        PostMessage(hWnd, WM_CLOSE, 0, 0);
      }
    }
  } else if (uNotification == TDN_BUTTON_CLICKED) {
    if (wParam == IDCANCEL) {
      if (c->app_state == APP_CANCELLED) {
        return S_OK;  // exit cleared
      }
      if (!c->req_exit) {
        c->cancelled = 1;
        fl_cancel(&c->loader);  // signal cancel event
      }
    }
  } else if (uNotification == TDN_TIMER) {
    DWORD r = WaitForSingleObject(c->thread_load, 0);
    if (r != WAIT_TIMEOUT) {
      // worker exited
      CloseHandle(c->thread_load);
      c->thread_load = nullptr;
      if (c->taskbar_list3) {
        c->taskbar_list3->lpVtbl->SetProgressState(
            c->taskbar_list3, hWnd, TBPF_NOPROGRESS);
      }
      if (c->app_state == APP_DONE) {
        // worker exited without error...
        if (!c->cancelled) {
          // and has not been cancelled
          if (AppBuildLog(c)) {
            c->dlg_done.pszExpandedInformation = c->log.c_str();
          } else {
            c->dlg_done.pszExpandedInformation = nullptr;
          }
          AppUpdateStatus(c);
          c->dlg_done.pszContent = c->status_txt;
          PostMessage(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);
          SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_done);
          navigated = 1;
        } else {
          // worker done, then cancelled before timer,
          // work again to continue cancellation routine
          c->app_state = APP_UNLOAD_FONT;
          SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
        }
      } else {
        if (c->app_state != APP_CANCELLED) {
          // it's an error
          TaskDialog(
              hWnd, c->hInst, MAKEINTRESOURCE(IDS_APP_NAME_VER), L"Error...",
              nullptr, TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, nullptr);
        }
        PostMessage(hWnd, WM_CLOSE, 0, 0);
      }
    } else {
      // work in progress
      if (c->taskbar_list3) {
        c->taskbar_list3->lpVtbl->SetProgressState(
            c->taskbar_list3, hWnd, TBPF_INDETERMINATE);
      }
    }
  }
  if (!navigated)
    AppUpdateStatus(c);
  return S_FALSE;
}

static HRESULT CALLBACK DlgHelpProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  auto c = (FL_AppCtx *)dwRefData;
  if (uNotification == TDN_HYPERLINK_CLICKED) {
    PostMessage(hWnd, WM_CLOSE, 0, 0);
    c->show_shortcut = 1;
  }
  return S_OK;
}

static HRESULT CALLBACK DlgDoneButtonDispatch(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    FL_AppCtx *c) {
  // all return S_FALSE: never close dialog
  switch (wParam) {
  case IDCANCEL:
  case IDCLOSE:
  case ID_BTN_RESCAN: {
    // Disable drag-drop before navigating away
    DragAcceptFiles(hWnd, FALSE);

    if (wParam != ID_BTN_RESCAN) {
      c->req_exit = 1;
    }
    SetEvent(c->evt_stop_cache);
    if (WaitForSingleObject(c->thread_cache, 1000) != WAIT_OBJECT_0) {
      TerminateThread(c->thread_cache, 2);
    }
    CloseHandle(c->thread_cache);
    c->thread_cache = nullptr;
    c->app_state = APP_UNLOAD_FONT;
    SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
    return S_FALSE;
  }
  case IDOK: {
    ShowWindow(hWnd, SW_MINIMIZE);
    return S_FALSE;
  }
  case ID_BTN_MENU: {
    RECT rect;
    POINT pt;
    HMENU menu = GetSubMenu(c->btn_menu, 0);
    HWND btn = c->handle_btn_menu;
    if (GetWindowRect(btn, &rect)) {
      pt.x = rect.left;
      pt.y = rect.bottom;
    } else {
      GetCursorPos(&pt);
    }
    BOOL r = TrackPopupMenu(
        menu, TPM_NONOTIFY | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, nullptr);
    if (r != FALSE) {
      return DlgDoneButtonDispatch(hWnd, uNotification, r, lParam, c);
    }
    return S_FALSE;
  }
  case ID_BTN_EXPORT: {
    ExportLoadedFonts(hWnd, c);
    return S_FALSE;
  }
  case ID_BTN_HELP: {
    AppHelpUsage(c, hWnd);
    return S_FALSE;
  }
  default: {
    return S_FALSE;
  }
  }
}

static BOOL CALLBACK DlgDoneFindMenuBtnCb(HWND hWnd, LPARAM lParam) {
  auto c = reinterpret_cast<FL_AppCtx *>(lParam);
  WCHAR buffer[16];
  const WCHAR *target = ResLoadString(c->hInst, IDS_MENU);
  if (target == nullptr) {
    return FALSE;  // stop! we are in trouble
  }
  if (const int len = GetWindowText(hWnd, buffer, _countof(buffer)); len != 0) {
    if (ass_strncmp(buffer, target, len + 1) == 0) {
      c->handle_btn_menu = hWnd;
      return FALSE;
    }
  }
  return TRUE;
}

static HRESULT CALLBACK DlgDoneProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  auto c = (FL_AppCtx *)dwRefData;
  if (uNotification == TDN_NAVIGATED) {
    c->thread_cache = nullptr;

    FS_Stat stat = {0};
    fs_stat(c->loader.font_set, &stat);
    if (c->loader.num_sub_font.load(std::memory_order_relaxed) == 0 ||
        stat.num_face == 0) {
      EnableMenuItem(c->btn_menu, ID_BTN_EXPORT, MF_BYCOMMAND | MF_GRAYED);
      AppHelpUsage(c, hWnd);
    } else {
      DWORD thread_id;
      EnableMenuItem(c->btn_menu, ID_BTN_EXPORT, MF_BYCOMMAND | MF_ENABLED);
      ResetEvent(c->evt_stop_cache);
      c->thread_cache =
          CreateThread(nullptr, 0, AppCacheWorker, c, 0, &thread_id);
    }

    // find the "Menu" button
    c->handle_btn_menu = nullptr;
    EnumChildWindows(hWnd, DlgDoneFindMenuBtnCb, (LPARAM)c);

    // Install subclass to handle drag-drop
    SetWindowSubclass(hWnd, DoneDialogSubclassProc, 0, (DWORD_PTR)c);

    // Enable drag-drop on the dialog
    DragAcceptFiles(hWnd, TRUE);
  } else if (uNotification == TDN_DESTROYED) {
    // Remove subclass and disable drag-drop when dialog is destroyed
    RemoveWindowSubclass(hWnd, DoneDialogSubclassProc, 0);
    DragAcceptFiles(hWnd, FALSE);
  } else if (uNotification == TDN_HYPERLINK_CLICKED) {
    // the only URL is the github repo
    auto url = L"https://github.com/baka-gourd/FontLoaderSub";
    ShellExecute(nullptr, nullptr, url, nullptr, nullptr, SW_SHOW);
  } else if (uNotification == TDN_BUTTON_CLICKED) {
    return DlgDoneButtonDispatch(hWnd, uNotification, wParam, lParam, c);
  }
  return S_OK;
}

static const TASKDIALOG_BUTTON kDlgDoneButtons[] = {
    {ID_BTN_MENU, MAKEINTRESOURCE(IDS_MENU)}};

static TASKDIALOGCONFIG MakeDlgWorkTemplate() {
  TASKDIALOGCONFIG cfg = {};
  cfg.cbSize = sizeof cfg;
  cfg.pszWindowTitle = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
  cfg.dwFlags =
      TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_SIZE_TO_CONTENT;
  cfg.pszMainInstruction = L"";
  cfg.pfCallback = DlgWorkProc;
  return cfg;
}

static TASKDIALOGCONFIG MakeDlgDoneTemplate() {
  TASKDIALOGCONFIG cfg = {};
  cfg.cbSize = sizeof cfg;
  cfg.pszWindowTitle = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON | TDCBF_OK_BUTTON;
  cfg.pszMainInstruction = MAKEINTRESOURCE(IDS_WORK_DONE);
  cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS |
                TDF_SIZE_TO_CONTENT;
  cfg.pszFooterIcon = TD_SHIELD_ICON;
  cfg.pszFooter = L"GPLv2: <A>github.com/baka-gourd/FontLoaderSub</A>";
  cfg.pfCallback = DlgDoneProc;
  cfg.cButtons =
      static_cast<UINT>(sizeof(kDlgDoneButtons) / sizeof(kDlgDoneButtons[0]));
  cfg.pButtons = kDlgDoneButtons;
  cfg.nDefaultButton = IDOK;
  return cfg;
}

static TASKDIALOGCONFIG MakeDlgHelpTemplate() {
  TASKDIALOGCONFIG cfg = {};
  cfg.cbSize = sizeof cfg;
  cfg.pszWindowTitle = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  cfg.pszMainIcon = TD_INFORMATION_ICON;
  cfg.pszMainInstruction = MAKEINTRESOURCE(IDS_HELP);
  cfg.pszContent = MAKEINTRESOURCE(IDS_USAGE);
  cfg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  cfg.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  cfg.pfCallback = DlgHelpProc;
  return cfg;
}

static const TASKDIALOGCONFIG kDlgWorkTemplate = MakeDlgWorkTemplate();
static const TASKDIALOGCONFIG kDlgDoneTemplate = MakeDlgDoneTemplate();
static const TASKDIALOGCONFIG kDlgHelpTemplate = MakeDlgHelpTemplate();

static int AppInit(FL_AppCtx *c, HINSTANCE hInst, allocator_t *alloc) {
  SPDLOG_INFO("AppInit start");
  c->hInst = hInst;
  c->alloc = alloc;

  c->dlg_work = kDlgWorkTemplate;
  c->dlg_work.hInstance = hInst;
  c->dlg_work.lpCallbackData = (LONG_PTR)c;

  c->dlg_done = kDlgDoneTemplate;
  c->dlg_done.hInstance = hInst;
  c->dlg_done.lpCallbackData = (LONG_PTR)c;

  c->dlg_help = kDlgHelpTemplate;
  c->dlg_help.hInstance = hInst;
  c->dlg_help.lpCallbackData = (LONG_PTR)c;

  c->btn_menu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_BTN_MENU));
  if (c->btn_menu == nullptr) {
    SPDLOG_ERROR("LoadMenu failed");
    return 0;
  }

  c->argv = CommandLineToArgvW(GetCommandLine(), &c->argc);
  if (c->argv == nullptr) {
    SPDLOG_ERROR("CommandLineToArgvW failed");
    return 0;
  }
  DWORD initial = MAX_PATH;
  while (1) {
    c->full_exe_path.resize(initial);
    DWORD ret = GetModuleFileName(nullptr, &c->full_exe_path[0], initial);
    if (ret == 0) {
      SPDLOG_ERROR("GetModuleFileName failed");
      return 0;
    }
    if (ret < initial - 1) {
      c->full_exe_path.resize(ret);
      break;
    }
    initial = initial * 2;
  }
  ShortcutInit(&c->shortcut, hInst, c->alloc);
  c->shortcut.key = L"FontLoaderSub";  // registry key
  c->shortcut.dlg_title = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  c->shortcut.dir_bg_menu_str_id = IDS_SHELL_VERB;
  c->shortcut.sendto_str_id = IDS_SENDTO;
  c->shortcut.path = c->full_exe_path.c_str();
  c->app_state = APP_LOAD_SUB;
  c->incremental_load = 0;  // Initialize flag
  c->drop_guard = 0;
  c->last_drop_tick = 0;
  c->drop_debounce_ms = 250;
  if (fl_init(&c->loader, c->alloc) != FL_OK) {
    SPDLOG_ERROR("fl_init failed");
    return 0;
  }
  c->log.clear();
  c->loaded_subs.clear();
  c->font_path = c->full_exe_path;

  if (MOCK_FONT_PATH != NULL)
    c->font_path.assign(nullptr);

  c->evt_stop_cache = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (c->evt_stop_cache == nullptr) {
    SPDLOG_ERROR("CreateEvent for cache failed");
    return 0;
  }

  if (!AppCreateMessageWindow(c)) {
    SPDLOG_ERROR("AppCreateMessageWindow failed");
    return 0;
  }

  if (SUCCEEDED(CoCreateInstance(
          CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, IID_ITaskbarList3,
          (void **)&c->taskbar_list3))) {
    if (FAILED(c->taskbar_list3->lpVtbl->HrInit(c->taskbar_list3))) {
      c->taskbar_list3->lpVtbl->Release(c->taskbar_list3);
      c->taskbar_list3 = nullptr;
    }
  }

  SPDLOG_INFO("AppInit done");
  return 1;
}

static int AppRun(FL_AppCtx *c) {
  SPDLOG_INFO("AppRun start");
  if (0 && GetAsyncKeyState(VK_SHIFT)) {
    ShortcutShow(&c->shortcut, nullptr);
    return 0;
  }

  TaskDialogIndirect(&c->dlg_work, nullptr, nullptr, nullptr);

  // clean up
  if (WaitForSingleObject(c->thread_load, 16384) == WAIT_TIMEOUT) {
    TerminateThread(c->thread_load, 1);
    fl_unload_fonts(&c->loader);
  }
  PostMessage(HWND_BROADCAST, WM_FONTCHANGE, 0, 0);

  // Signal the message window to quit and destroy it
  if (c->hwnd_message) {
    PostMessage(c->hwnd_message, WM_CLOSE, 0, 0);
  }

  // Message loop for the hidden window
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  SPDLOG_INFO("AppRun done");
  return 0;
}

FL_AppCtx g_app;

int test_main();

int WINAPI _tWinMain(
    HINSTANCE hInstance,
    HINSTANCE hPrevInstance,
    LPTSTR lpCmdLine,
    int nCmdShow) {
  PerMonitorDpiHack();
  if (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) != S_OK) {
    return 0;
  }

  HANDLE heap = HeapCreate(0, 0, 0);
  allocator_t alloc = {};
  alloc.alloc = mem_realloc;
  alloc.arg = heap;
  fl_log::Init();
  SPDLOG_INFO("FontLoaderSub start, pid={}", GetCurrentProcessId());
  FL_AppCtx *ctx = &g_app;
  if (ctx == nullptr || !AppInit(ctx, hInstance, &alloc)) {
    TaskDialog(
        nullptr, hInstance, MAKEINTRESOURCE(IDS_APP_NAME_VER), L"Error...",
        nullptr, TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, nullptr);
    SPDLOG_ERROR("AppInit failed");
    fl_log::Shutdown();
    return 1;
  }
  if (ctx->argc > 0) {
    SPDLOG_INFO("Command line argc={}", ctx->argc);
    for (int i = 0; i < ctx->argc; i++) {
      std::string arg_u8;
      if (Utf16ToUtf8(ctx->argv[i], &arg_u8)) {
        SPDLOG_INFO("argv[{}]={}", i, arg_u8);
      }
    }
  }
  AppRun(ctx);

  if (ctx->hwnd_message) {
    DestroyWindow(ctx->hwnd_message);
    ctx->hwnd_message = nullptr;
  }

  SPDLOG_INFO("FontLoaderSub exit");
  fl_log::Shutdown();
  return 0;
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  const UINT u_ret_code =
      _tWinMain((HINSTANCE)&__ImageBase, nullptr, nullptr, SW_SHOWDEFAULT);
  ExitProcess(u_ret_code);
}
