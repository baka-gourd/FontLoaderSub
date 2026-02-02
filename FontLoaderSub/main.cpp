#include "main.h"

#include <cstring>
#include <string>

#include "ass_string.h"
#include "exporter.h"
#include "util.h"
#include "path.h"
#include "shortcut.h"
#include "mock_config.h"
#include "res/resource.h"
#include "utf.h"

#define kCacheFile L"fc-subs.ftdb"
#define kBlackFile L"fc-ignore.txt"
#define kMessageWindowClass L"FontLoaderSubMessageWindow"

static DWORD WINAPI AppWorker(LPVOID param);
static LRESULT CALLBACK DoneDialogSubclassProc(
    HWND hWnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR uIdSubclass,
    DWORD_PTR dwRefData);

static int IsSubtitleFileLoaded(FL_AppCtx *c, const wchar_t *filePath) {
  wchar_t fullPath[MAX_PATH * 2];
  if (GetFullPathName(filePath, _countof(fullPath), fullPath, NULL) == 0) {
    return 0;
  }

  const wchar_t *data = (const wchar_t *)str_db_get(&c->loaded_subs, 0);
  size_t totalLen = str_db_tell(&c->loaded_subs);

  for (size_t pos = 0; pos < totalLen;) {
    const wchar_t *loadedPath = data + pos;
    size_t len = wcslen(loadedPath);
    if (len > 0 && _wcsicmp(fullPath, loadedPath) == 0) {
      return 1;
    }
    pos += len + 1;
  }

  return 0;
}

static int AddSubtitleFileToLoaded(FL_AppCtx *c, const wchar_t *filePath) {
  wchar_t fullPath[MAX_PATH * 2];
  if (GetFullPathName(filePath, _countof(fullPath), fullPath, NULL) == 0) {
    return 0;
  }

  return str_db_push_u16_le(&c->loaded_subs, fullPath, 0) != NULL;
}

static void *mem_realloc(void *existing, size_t size, void *arg) {
  HANDLE heap = (HANDLE)arg;
  if (size == 0) {
    HeapFree(heap, 0, existing);
    return NULL;
  }
  if (existing == NULL) {
    return HeapAlloc(heap, HEAP_ZERO_MEMORY, size);
  }
  return HeapReAlloc(heap, HEAP_ZERO_MEMORY, existing, size);
}

static LRESULT CALLBACK
MessageWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  FL_AppCtx *c = (FL_AppCtx *)GetWindowLongPtr(hWnd, GWLP_USERDATA);

  switch (uMsg) {
  case WM_CREATE:
    return 0;
  case WM_DROPFILES:
    if (c && c->app_state == APP_DONE) {
      HDROP hDrop = (HDROP)wParam;
      UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);
      int has_new_files = 0;

      for (UINT i = 0; i < fileCount; i++) {
        UINT fileNameLen = DragQueryFile(hDrop, i, NULL, 0);
        if (fileNameLen > 0) {
          wchar_t *fileName = (wchar_t *)HeapAlloc(
              GetProcessHeap(), 0, (fileNameLen + 1) * sizeof(wchar_t));
          if (fileName) {
            DragQueryFile(hDrop, i, fileName, fileNameLen + 1);

            // Check if subtitle file is already loaded
            if (!IsSubtitleFileLoaded(c, fileName)) {
              int r = fl_add_subs(&c->loader, fileName);
              if (r == FL_OK) {
                AddSubtitleFileToLoaded(c, fileName);
                has_new_files = 1;
              }
            }

            HeapFree(GetProcessHeap(), 0, fileName);
          }
        }
      }
      DragFinish(hDrop);

      // Only reload if we have new files
      if (has_new_files) {
        // Load fonts for new subtitles only (don't unload existing fonts)
        c->app_state = APP_LOAD_FONT;
        c->incremental_load = 1;  // Use incremental loading
        c->cancelled = 0;
        c->req_exit = 0;

        DWORD thread_id;
        c->thread_load = CreateThread(NULL, 0, AppWorker, c, 0, &thread_id);
        if (c->thread_load != NULL && c->work_hwnd) {
          SendMessage(c->work_hwnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
        }
      }
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
  FL_AppCtx *c = (FL_AppCtx *)dwRefData;

  switch (uMsg) {
  case WM_DROPFILES:
    if (c && c->app_state == APP_DONE) {
      HDROP hDrop = (HDROP)wParam;
      UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);
      int has_new_files = 0;

      for (UINT i = 0; i < fileCount; i++) {
        UINT fileNameLen = DragQueryFile(hDrop, i, NULL, 0);
        if (fileNameLen > 0) {
          wchar_t *fileName = (wchar_t *)HeapAlloc(
              GetProcessHeap(), 0, (fileNameLen + 1) * sizeof(wchar_t));
          if (fileName) {
            DragQueryFile(hDrop, i, fileName, fileNameLen + 1);

            // Check if subtitle file is already loaded
            if (!IsSubtitleFileLoaded(c, fileName)) {
              int r = fl_add_subs(&c->loader, fileName);
              if (r == FL_OK) {
                AddSubtitleFileToLoaded(c, fileName);
                has_new_files = 1;
              }
            }

            HeapFree(GetProcessHeap(), 0, fileName);
          }
        }
      }
      DragFinish(hDrop);

      // Only reload if we have new files
      if (has_new_files) {
        // Load fonts for new subtitles only (don't unload existing fonts)
        c->app_state = APP_LOAD_FONT;
        c->incremental_load = 1;  // Use incremental loading
        c->cancelled = 0;
        c->req_exit = 0;

        // Disable drag-drop before navigating away
        DragAcceptFiles(hWnd, FALSE);

        DWORD thread_id;
        c->thread_load = CreateThread(NULL, 0, AppWorker, c, 0, &thread_id);
        if (c->thread_load != NULL) {
          SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg_work);
        }
      }
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
      HWND_MESSAGE, NULL, c->hInst, NULL);

  if (c->hwnd_message == NULL) {
    return 0;
  }

  SetWindowLongPtr(c->hwnd_message, GWLP_USERDATA, (LONG_PTR)c);
  return 1;
}

static void AppHelpUsage(FL_AppCtx *c, HWND hWnd) {
  c->show_shortcut = 0;
  c->dlg_help.hwndParent = hWnd;
  TaskDialogIndirect(&c->dlg_help, NULL, NULL, NULL);
  if (c->show_shortcut) {
    ShortcutShow(&c->shortcut, hWnd);
  }
}

static int AppBuildLog(FL_AppCtx *c) {
  vec_t *loaded = &c->loader.loaded_font;
  str_db_t *log = &c->log;

  str_db_seek(log, 0);
  FL_FontMatch *data = (FL_FontMatch *)loaded->data;

  for (size_t i = 0; i != loaded->n; i++) {
    const wchar_t *tag;
    FL_FontMatch *m = &data[i];
    if (m->flag & (FL_LOAD_DUP))
      tag = L"[^ ] ";
    else if (m->flag & (FL_OS_LOADED | FL_LOAD_OK))
      tag = L"[ok] ";
    else if (m->flag & (FL_LOAD_ERR))
      tag = L"[ X] ";
    else if (1 || m->flag & (FL_LOAD_MISS))
      tag = L"[??] ";
    std::wstring face_w;
    if (m->face && !Utf8ToUtf16(m->face, std::strlen(m->face), &face_w))
      return 0;
    if (!str_db_push_u16_le(log, tag, 0) ||
        !str_db_push_u16_le(log, face_w.c_str(), face_w.size()))
      return 0;
    if (m->filename && !(m->flag & FL_LOAD_DUP)) {
      std::wstring file_w;
      if (!Utf8ToUtf16(m->filename, std::strlen(m->filename), &file_w))
        return 0;
      if (!str_db_push_u16_le(log, L" > ", 0) ||
          !str_db_push_u16_le(log, file_w.c_str(), file_w.size()))
        return 0;
    }
    if (!str_db_push_u16_le(log, L"\n", 0))
      return 0;
  }
  const size_t pos = str_db_tell(log);
  if (!pos)
    return 0;
  wchar_t *buf = (wchar_t *)str_db_get(log, 0);
  buf[pos - 1] = 0;
  return 1;
}

static int AppUpdateStatus(FL_AppCtx *c) {
  FS_Stat stat = {0};
  if (c->loader.font_set) {
    fs_stat(c->loader.font_set, &stat);
  }

  DWORD_PTR args[] = {
      // arguments
      c->loader.num_font_loaded,
      c->loader.num_font_failed,
      c->loader.num_font_unmatched,
      stat.num_file,
      stat.num_face,
      c->loader.num_sub,
  };
  FormatMessage(
      FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY,
      ResLoadString(c->hInst, IDS_LOAD_STAT), 0, 0, c->status_txt,
      _countof(c->status_txt), (va_list *)args);

  LPARAM cap_id;
  if (c->cancelled || c->app_state == APP_CANCELLED) {
    cap_id = IDS_WORK_CANCELLING;
  } else {
    cap_id = c->app_state;
  }

  SendMessage(c->work_hwnd, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, cap_id);
  SendMessage(
      c->work_hwnd, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (LPARAM)c->status_txt);

  return 0;
}

static DWORD WINAPI AppWorker(LPVOID param) {
  FL_AppCtx *c = (FL_AppCtx *)param;
  int r = FL_OK;
  while (r == FL_OK && !c->cancelled && c->app_state != APP_DONE) {
    switch (c->app_state) {
    case APP_LOAD_SUB: {
      if (MOCK_SUB_PATH) {
        r = fl_add_subs(&c->loader, MOCK_SUB_PATH);
        if (r == FL_OK) {
          AddSubtitleFileToLoaded(c, MOCK_SUB_PATH);
        }
      }
      for (int i = 1; i < c->argc && r == FL_OK; i++) {
        r = fl_add_subs(&c->loader, c->argv[i]);
        if (r == FL_OK) {
          AddSubtitleFileToLoaded(c, c->argv[i]);
        }
      }
      c->app_state = APP_LOAD_CACHE;
      break;
    }
    case APP_LOAD_CACHE: {
      fl_scan_fonts(&c->loader, c->font_path, kCacheFile, kBlackFile);
      FS_Stat stat = {0};
      fs_stat(c->loader.font_set, &stat);
      if (stat.num_face == 0) {
        c->app_state = APP_SCAN_FONT;
      } else {
        c->app_state = APP_LOAD_FONT;
      }
      break;
    }
    case APP_SCAN_FONT: {
      if (fl_scan_fonts(&c->loader, c->font_path, NULL, kBlackFile) == FL_OK) {
        fl_save_cache(&c->loader, kCacheFile);
      }
      c->app_state = APP_LOAD_FONT;
      break;
    }
    case APP_LOAD_FONT: {
      if (c->incremental_load) {
        r = fl_load_fonts_incremental(&c->loader);
        c->incremental_load = 0;  // Reset flag
      } else {
        r = fl_load_fonts(&c->loader);
      }
      if (r == FL_OK)
        c->app_state = APP_DONE;
      break;
    }
    case APP_UNLOAD_FONT: {
      fl_unload_fonts(&c->loader);
      if (c->req_exit) {
        c->cancelled = 1;
      } else {
        c->app_state = APP_SCAN_FONT;
      }
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
  }

  return 0;
}

static DWORD WINAPI AppCacheWorker(LPVOID param) {
  FL_AppCtx *c = (FL_AppCtx *)param;

  while (1) {
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
  FL_AppCtx *c = (FL_AppCtx *)dwRefData;
  int navigated = 0;
  if (uNotification == TDN_CREATED || uNotification == TDN_NAVIGATED) {
    c->work_hwnd = hWnd;
    SendMessage(hWnd, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);

    DWORD thread_id;
    c->thread_load = CreateThread(NULL, 0, AppWorker, c, 0, &thread_id);
    if (c->thread_load == NULL) {
      // fatal error, try exit early
      c->cancelled = 1;
      c->app_state = APP_CANCELLED;
      PostMessage(hWnd, WM_CLOSE, 0, 0);
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
      c->thread_load = NULL;
      if (c->taskbar_list3) {
        c->taskbar_list3->lpVtbl->SetProgressState(
            c->taskbar_list3, hWnd, TBPF_NOPROGRESS);
      }
      if (c->app_state == APP_DONE) {
        // worker exited without error...
        if (!c->cancelled) {
          // and has not been cancelled
          if (AppBuildLog(c)) {
            c->dlg_done.pszExpandedInformation = str_db_get(&c->log, 0);
          } else {
            c->dlg_done.pszExpandedInformation = NULL;
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
              NULL, TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, NULL);
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
  FL_AppCtx *c = (FL_AppCtx *)dwRefData;
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
    c->thread_cache = NULL;
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
        menu, TPM_NONOTIFY | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
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
  FL_AppCtx *c = (FL_AppCtx *)lParam;
  WCHAR buffer[16];
  const WCHAR *target = ResLoadString(c->hInst, IDS_MENU);
  if (target == NULL) {
    return FALSE;  // stop! we are in trouble
  }
  int len = GetWindowText(hWnd, buffer, _countof(buffer));
  if (len != 0) {
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
  FL_AppCtx *c = (FL_AppCtx *)dwRefData;
  if (uNotification == TDN_NAVIGATED) {
    c->thread_cache = NULL;

    FS_Stat stat = {0};
    fs_stat(c->loader.font_set, &stat);
    if (c->loader.num_sub_font == 0 || stat.num_face == 0) {
      EnableMenuItem(c->btn_menu, ID_BTN_EXPORT, MF_BYCOMMAND | MF_GRAYED);
      AppHelpUsage(c, hWnd);
    } else {
      DWORD thread_id;
      EnableMenuItem(c->btn_menu, ID_BTN_EXPORT, MF_BYCOMMAND | MF_ENABLED);
      ResetEvent(c->evt_stop_cache);
      c->thread_cache = CreateThread(NULL, 0, AppCacheWorker, c, 0, &thread_id);
    }

    // find the "Menu" button
    c->handle_btn_menu = NULL;
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
    const WCHAR *url = L"https://github.com/yzwduck/FontLoaderSub";
    ShellExecute(NULL, NULL, url, NULL, NULL, SW_SHOW);
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
  cfg.pszFooter = L"GPLv2: <A>github.com/yzwduck/FontLoaderSub</A>";
  cfg.pfCallback = DlgDoneProc;
  cfg.cButtons = (UINT)(sizeof(kDlgDoneButtons) / sizeof(kDlgDoneButtons[0]));
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
  c->hInst = hInst;
  c->alloc = alloc;

  memcpy(&c->dlg_work, &kDlgWorkTemplate, sizeof c->dlg_work);
  c->dlg_work.hInstance = hInst;
  c->dlg_work.lpCallbackData = (LONG_PTR)c;

  memcpy(&c->dlg_done, &kDlgDoneTemplate, sizeof c->dlg_done);
  c->dlg_done.hInstance = hInst;
  c->dlg_done.lpCallbackData = (LONG_PTR)c;

  memcpy(&c->dlg_help, &kDlgHelpTemplate, sizeof c->dlg_help);
  c->dlg_help.hInstance = hInst;
  c->dlg_help.lpCallbackData = (LONG_PTR)c;

  c->btn_menu = LoadMenu(hInst, MAKEINTRESOURCE(IDR_BTN_MENU));
  if (c->btn_menu == NULL)
    return 0;

  c->argv = CommandLineToArgvW(GetCommandLine(), &c->argc);
  if (c->argv == NULL)
    return 0;
  if (str_db_init(&c->full_exe_path, c->alloc, 0, 0))
    return 0;

  DWORD initial = MAX_PATH;
  while (1) {
    if (vec_prealloc(&c->full_exe_path.vec, initial) < initial)
      return 0;
    DWORD ret = GetModuleFileName(
        NULL, (WCHAR *)str_db_get(&c->full_exe_path, 0), initial);
    if (ret == 0)
      return 0;
    if (ret < initial) {
      // sufficient buffer size
      break;
    } else {
      initial = initial * 2;
    }
  }
  if (str_db_push_u16_le(
          &c->full_exe_path, str_db_get(&c->full_exe_path, 0), 0) == NULL)
    return 0;
  ShortcutInit(&c->shortcut, hInst, c->alloc);
  c->shortcut.key = L"FontLoaderSub";  // registry key
  c->shortcut.dlg_title = MAKEINTRESOURCE(IDS_APP_NAME_VER);
  c->shortcut.dir_bg_menu_str_id = IDS_SHELL_VERB;
  c->shortcut.sendto_str_id = IDS_SENDTO;
  c->shortcut.path = str_db_get(&c->full_exe_path, 0);
  c->app_state = APP_LOAD_SUB;
  c->incremental_load = 0;  // Initialize flag
  if (fl_init(&c->loader, c->alloc) != FL_OK)
    return 0;
  str_db_init(&c->log, c->alloc, 0, 0);
  // loaded_subs stores a list of NUL-terminated strings, so pad_len must be 1.
  // (pad_len=0 is only suitable for building a single concatenated string.)
  str_db_init(&c->loaded_subs, c->alloc, 0, 1);
  c->font_path = str_db_get(&c->full_exe_path, 0);

  if (MOCK_FONT_PATH)
    c->font_path = MOCK_FONT_PATH;

  c->evt_stop_cache = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (c->evt_stop_cache == NULL)
    return 0;

  if (!AppCreateMessageWindow(c))
    return 0;

  if (SUCCEEDED(CoCreateInstance(
          CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, IID_ITaskbarList3,
          (void **)&c->taskbar_list3))) {
    if (FAILED(c->taskbar_list3->lpVtbl->HrInit(c->taskbar_list3))) {
      c->taskbar_list3->lpVtbl->Release(c->taskbar_list3);
      c->taskbar_list3 = NULL;
    }
  }

  return 1;
}

static int AppRun(FL_AppCtx *c) {
  if (0 && GetAsyncKeyState(VK_SHIFT)) {
    ShortcutShow(&c->shortcut, NULL);
    return 0;
  }

  TaskDialogIndirect(&c->dlg_work, NULL, NULL, NULL);

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
  while (GetMessage(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

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
  if (CoInitializeEx(NULL, COINIT_APARTMENTTHREADED) != S_OK) {
    return 0;
  }

  HANDLE heap = HeapCreate(0, 0, 0);
  allocator_t alloc = {};
  alloc.alloc = mem_realloc;
  alloc.arg = heap;
  FL_AppCtx *ctx = &g_app;
  if (ctx == NULL || !AppInit(ctx, hInstance, &alloc)) {
    TaskDialog(
        NULL, hInstance, MAKEINTRESOURCE(IDS_APP_NAME_VER), L"Error...", NULL,
        TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, NULL);
    return 1;
  }
  AppRun(ctx);

  if (ctx->hwnd_message) {
    DestroyWindow(ctx->hwnd_message);
    ctx->hwnd_message = NULL;
  }

  return 0;
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

void MyEntryPoint() {
  UINT uRetCode;
  uRetCode = _tWinMain((HINSTANCE)&__ImageBase, NULL, NULL, SW_SHOWDEFAULT);
  ExitProcess(uRetCode);
}
