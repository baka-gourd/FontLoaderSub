#include "shortcut.h"

#include <Shlwapi.h>
#include <ShlObj.h>
#include <ObjIdl.h>

#include <cwchar>
#include <limits>

#include "dark_mode.h"
#include "res/resource.h"

typedef enum {
  SHORTCUT_MODE_QUERY,
  SHORTCUT_MODE_CREATE,
  SHORTCUT_MODE_DELETE
} ShortcutMode;

#define BUTTON_ID_START 1024

typedef int (*ShortcutTogglers)(FL_ShortCtx *c, ShortcutMode mode);

static size_t ShortcutWcsnlen(const wchar_t *str, size_t n) {
  for (size_t i = 0; i != n; i++) {
    if (str[i] == 0)
      return i;
  }
  return n;
}

static void ShortcutTmpClear(FL_ShortCtx *c) {
  c->tmp.clear();
}

static void ShortcutTmpAppend(FL_ShortCtx *c, const wchar_t *str, size_t cch) {
  if (str == nullptr)
    return;
  const size_t len = cch ? ShortcutWcsnlen(str, cch) : wcslen(str);
  c->tmp.append(str, len);
}

static int ShortcutExplorerDirectory(
    FL_ShortCtx *ctx,
    const WCHAR *key_path,
    ShortcutMode mode);

static void ShortcutRefresh(FL_ShortCtx *ctx, int error);

static HRESULT CALLBACK DlgShortcutProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData);

void ShortcutInit(FL_ShortCtx *c, HINSTANCE hInst, allocator_t *alloc) {
  (void)alloc;
  c->dlg.cbSize = sizeof c->dlg;
  c->dlg.hInstance = hInst;
  c->dlg.pszContent = MAKEINTRESOURCE(IDS_MANAGE_SHORTCUT);
  c->dlg.pButtons = c->button;
  c->dlg.cButtons = FL_SHORTCUT_MAX;
  c->dlg.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  c->dlg.dwFlags |= TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
  c->dlg.lpCallbackData = (LONG_PTR)c;
  c->dlg.pfCallback = DlgShortcutProc;
  for (int i = 0; i != FL_SHORTCUT_MAX; i++) {
    c->button[i].nButtonID = BUTTON_ID_START + i;
  }
  c->tmp.clear();
}

void ShortcutShow(FL_ShortCtx *c, HWND hWnd) {
  c->dlg.hwndParent = hWnd;
  c->dlg.pszWindowTitle = c->dlg_title;
  c->dlg.nDefaultButton = IDCLOSE;
  ShortcutRefresh(c, 0);
  TaskDialogIndirect(&c->dlg, nullptr, nullptr, nullptr);
}

int ShortcutExplorerDirectory(
    FL_ShortCtx *c,
    const WCHAR *key_path,
    ShortcutMode mode) {
  HKEY root = nullptr;
  HKEY command = nullptr;
  LSTATUS ret;
  int succ = 0;
  ShortcutTmpClear(c);

  do {
    ShortcutTmpAppend(c, key_path, 0);
    ShortcutTmpAppend(c, c->key, 0);
    const WCHAR *key = c->tmp.c_str();

    if (mode == SHORTCUT_MODE_QUERY) {
      ret = RegOpenKeyEx(HKEY_CURRENT_USER, key, 0, KEY_QUERY_VALUE, &root);
      if (ret == ERROR_SUCCESS) {
        succ = 1;
      }
    } else if (mode == SHORTCUT_MODE_DELETE) {
      ret = RegDeleteTree(HKEY_CURRENT_USER, key);
      succ = 1;
    } else if (mode == SHORTCUT_MODE_CREATE) {
      DWORD disposition;
      ret = RegCreateKeyEx(
          HKEY_CURRENT_USER, key, 0, nullptr, REG_OPTION_NON_VOLATILE,
          KEY_ALL_ACCESS, nullptr, &root, &disposition);
      if (ret != ERROR_SUCCESS)
        break;

      WCHAR res_suffix[16];
      FormatMessage(
          FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_ARGUMENT_ARRAY,
          L"\",-%1!i!", 0, 0, res_suffix, _countof(res_suffix),
          (va_list *)&c->dir_bg_menu_str_id);

      ShortcutTmpClear(c);
      ShortcutTmpAppend(c, L"@\"", 0);
      ShortcutTmpAppend(c, c->path, 0);
      ShortcutTmpAppend(c, res_suffix, 0);
      const WCHAR *verb = c->tmp.c_str();
      const size_t verb_bytes = c->tmp.size() * sizeof verb[0];
      if (verb_bytes > (std::numeric_limits<DWORD>::max)()) {
        ret = ERROR_MORE_DATA;
        break;
      }
      ret = RegSetValueEx(
          root, TEXT("MUIVerb"), 0, REG_SZ, (const BYTE *)verb,
          static_cast<DWORD>(verb_bytes));
      if (ret != ERROR_SUCCESS)
        break;

      ret = RegCreateKeyEx(
          root, L"command", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
          nullptr, &command, &disposition);
      if (ret != ERROR_SUCCESS)
        break;

      ShortcutTmpClear(c);
      ShortcutTmpAppend(c, L"\"", 0);
      ShortcutTmpAppend(c, c->path, 0);
      ShortcutTmpAppend(c, L"\" \"%V\"", 0);
      const WCHAR *path = c->tmp.c_str();
      const size_t path_bytes = c->tmp.size() * sizeof path[0];
      if (path_bytes > (std::numeric_limits<DWORD>::max)()) {
        ret = ERROR_MORE_DATA;
        break;
      }
      ret = RegSetValueEx(
          command, nullptr, 0, REG_SZ, (const BYTE *)path,
          static_cast<DWORD>(path_bytes));
      if (ret != ERROR_SUCCESS)
        break;
      succ = 1;
    }
  } while (0);

  if (command)
    RegCloseKey(command);
  if (root)
    RegCloseKey(root);
  return succ;
}

static int ShortcutExplorerDirectoryBackground(
    FL_ShortCtx *c,
    ShortcutMode mode) {
  return ShortcutExplorerDirectory(
      c, L"Software\\Classes\\Directory\\Background\\shell\\", mode);
}

int ShortcutSendTo(FL_ShortCtx *c, ShortcutMode mode) {
  int succ = 0;
  HRESULT hr;
  PWSTR sendto_path = nullptr;
  ShortcutTmpClear(c);

  do {
    hr = SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &sendto_path);
    if (FAILED(hr))
      break;
    ShortcutTmpAppend(c, sendto_path, 0);
    ShortcutTmpAppend(c, L"\\", 0);
    ShortcutTmpAppend(c, ResLoadString(c->dlg.hInstance, c->sendto_str_id), 0);
    ShortcutTmpAppend(c, L".lnk", 0);
    const WCHAR *path = c->tmp.c_str();
    if (mode == SHORTCUT_MODE_QUERY) {
      HANDLE h = CreateFile(
          path, 0, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        succ = 1;
      }
    } else if (mode == SHORTCUT_MODE_DELETE) {
      HANDLE h = CreateFile(
          path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        succ = 1;
      }
    } else if (mode == SHORTCUT_MODE_CREATE) {
      IShellLink *psl;
      hr = CoCreateInstance(
          CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLink,
          (void **)&psl);
      if (SUCCEEDED(hr)) {
        // set shortcut target
        hr = psl->lpVtbl->SetPath(psl, c->path);
        if (SUCCEEDED(hr)) {
          IPersistFile *ppf;
          hr =
              psl->lpVtbl->QueryInterface(psl, IID_IPersistFile, (void **)&ppf);
          if (SUCCEEDED(hr)) {
            hr = ppf->lpVtbl->Save(ppf, path, TRUE);
            if (SUCCEEDED(hr)) {
              succ = 1;
            }
            ppf->lpVtbl->Release(ppf);
          }
        }
        psl->lpVtbl->Release(psl);
      }
    }
  } while (0);
  if (sendto_path)
    CoTaskMemFree(sendto_path);
  return succ;
}

static void ShortcutRefresh(FL_ShortCtx *c, int error) {
  c->setup[FL_SHORTCUT_CONTEXT] =
      ShortcutExplorerDirectoryBackground(c, SHORTCUT_MODE_QUERY);
  c->button[FL_SHORTCUT_CONTEXT].pszButtonText =
      c->setup[FL_SHORTCUT_CONTEXT] ? MAKEINTRESOURCE(IDS_SHORTCUT_DEL_DIR_BG)
                                    : MAKEINTRESOURCE(IDS_SHORTCUT_ADD_DIR_BG);
  c->setup[FL_SHORTCUT_SENDTO] = ShortcutSendTo(c, SHORTCUT_MODE_QUERY);
  c->button[FL_SHORTCUT_SENDTO].pszButtonText =
      c->setup[FL_SHORTCUT_SENDTO] ? MAKEINTRESOURCE(IDS_SHORTCUT_DEL_SENDTO)
                                   : MAKEINTRESOURCE(IDS_SHORTCUT_ADD_SENDTO);

  if (error) {
    c->dlg.pszFooterIcon = TD_WARNING_ICON;
    c->dlg.pszFooter = MAKEINTRESOURCE(error);
  } else {
    c->dlg.pszFooterIcon = nullptr;
    c->dlg.pszFooter = nullptr;
  }
}

static const ShortcutTogglers kShortcutToggler[FL_SHORTCUT_MAX] = {
    ShortcutSendTo, ShortcutExplorerDirectoryBackground};

static HRESULT CALLBACK DlgShortcutProc(
    HWND hWnd,
    UINT uNotification,
    WPARAM wParam,
    LPARAM lParam,
    LONG_PTR dwRefData) {
  DarkModeTaskDialogNotification(hWnd, uNotification);
  FL_ShortCtx *c = (FL_ShortCtx *)dwRefData;
  switch (uNotification) {
  case TDN_BUTTON_CLICKED: {
    if (BUTTON_ID_START <= wParam &&
        wParam < BUTTON_ID_START + FL_SHORTCUT_MAX) {
      int id = static_cast<int>(wParam - BUTTON_ID_START);
      int succ = kShortcutToggler[id](
          c, c->setup[id] ? SHORTCUT_MODE_DELETE : SHORTCUT_MODE_CREATE);
      // if (GetTickCount() / 1000 % 2 == 0) succ = 0;
      int err = c->setup[id] ? IDS_SHORTCUT_ERROR_DEL : IDS_SHORTCUT_ERROR_ADD;
      ShortcutRefresh(c, succ ? 0 : err);
      c->dlg.nDefaultButton = static_cast<int>(wParam);
      SendMessage(hWnd, TDM_NAVIGATE_PAGE, 0, (LPARAM)&c->dlg);
      return S_FALSE;
    }
  }
  }
  // return S_FALSE; // not to close
  return S_OK;  // otherwise
}
