#include "exporter.h"

#include <string>

#include <Shobjidl.h>

#include "log.h"
#include "utf.h"

#define C_SAVE_RELEASE(obj)          \
  do {                               \
    if ((obj) != NULL) {             \
      (obj)->lpVtbl->Release((obj)); \
      obj = NULL;                    \
    }                                \
  } while (0)

typedef struct LoadedFontEnumCtx {
  const IEnumShellItemsVtbl *lpVtbl;
  FL_AppCtx *app;
  IShellItem *root;
  size_t i;
  ULONG volatile ref;
} LoadedFontEnumCtx;

static ULONG STDMETHODCALLTYPE LFEC_AddRef(IEnumShellItems *This);

static HRESULT STDMETHODCALLTYPE
LFEC_QueryInterface(IEnumShellItems *This, REFIID riid, void **out) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  if (IsEqualGUID(riid, IID_IUnknown) ||
      IsEqualGUID(riid, IID_IEnumShellItems)) {
    LFEC_AddRef(This);
    *out = (void *)c;
    return S_OK;
  } else {
    return E_NOINTERFACE;
  }
}

static ULONG STDMETHODCALLTYPE LFEC_AddRef(IEnumShellItems *This) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  return InterlockedIncrement(&c->ref);
}

static ULONG STDMETHODCALLTYPE LFEC_Release(IEnumShellItems *This) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  ULONG ret = InterlockedDecrement(&c->ref);
  if (ret == 0) {
    c->root->lpVtbl->Release(c->root);
    c->app->alloc->alloc(c, 0, c->app->alloc->arg);
  }
  return ret;
}

static HRESULT LFEC_NextOne(LoadedFontEnumCtx *c, IShellItem **item) {
  FL_LoaderCtx *fl = &c->app->loader;
  while (c->i != fl->loaded_font.size()) {
    FL_FontMatch *m = &fl->loaded_font[c->i];
    c->i++;
    if (!m->filename.empty() && (m->flag & FL_LOAD_DUP) == 0) {
      if (item != nullptr) {
        std::wstring file_w;
        if (!Utf8ToUtf16(m->filename.c_str(), &file_w))
          return E_FAIL;
        return SHCreateItemFromRelativeName(
            c->root, file_w.c_str(), nullptr, IID_IShellItem, (void **)item);
      } else {
        return S_OK;  // simulate create success
      }
    }
  }
  return S_FALSE;  // no more items
}

static HRESULT STDMETHODCALLTYPE LFEC_Next(
    IEnumShellItems *This,
    ULONG celt,
    IShellItem **rgelt,
    ULONG *pceltFetched) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  ULONG got = 0;
  HRESULT hr = S_FALSE;
  for (ULONG i = 0; i != celt; i++) {
    hr = LFEC_NextOne(c, rgelt ? &rgelt[got] : nullptr);
    if (hr == S_FALSE) {
      // no more left
      break;
    } else if (FAILED(hr)) {
      // should hide something
      break;
    } else {
      got++;
    }
  }
  if (pceltFetched != nullptr) {
    *pceltFetched = got;
  }
  return got > 0 ? S_OK : hr;
}

static HRESULT STDMETHODCALLTYPE LFEC_Skip(IEnumShellItems *This, ULONG celt) {
  return LFEC_Next(This, celt, nullptr, nullptr);
}

static HRESULT STDMETHODCALLTYPE LFEC_Reset(IEnumShellItems *This) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  c->i = 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
LFEC_Clone(IEnumShellItems *This, IEnumShellItems **ppenum) {
  LoadedFontEnumCtx *c = (LoadedFontEnumCtx *)This;
  LoadedFontEnumCtx *that = (LoadedFontEnumCtx *)c->app->alloc->alloc(
      nullptr, sizeof *c, c->app->alloc->arg);
  if (that == nullptr) {
    return E_OUTOFMEMORY;
  }
  that->lpVtbl = c->lpVtbl;
  that->app = c->app;
  that->root = c->root;
  that->i = c->i;
  that->ref = 1;
  c->root->lpVtbl->AddRef(c->root);
  *ppenum = (IEnumShellItems *)that;
  return S_OK;
}

static const IEnumShellItemsVtbl kLFEC_Verb = {
    LFEC_QueryInterface, LFEC_AddRef, LFEC_Release, LFEC_Next,
    LFEC_Skip,           LFEC_Reset,  LFEC_Clone,
};

static HRESULT LFEC_Create(FL_AppCtx *app, IEnumShellItems **ppenum) {
  FL_LoaderCtx *fl = &app->loader;
  std::wstring font_path = fl->font_path;
  if (font_path.empty())
    return E_FAIL;
  if (font_path.size() >= 4 && font_path[0] == L'\\' && font_path[1] == L'\\' &&
      font_path[2] == L'?' && font_path[3] == L'\\') {
    // case 1: \\?\E:\... -> E:\...
    font_path.erase(0, 4);
    if (font_path.size() >= 4 && font_path[0] == L'U' && font_path[1] == L'N' &&
        font_path[2] == L'C' && font_path[3] == L'\\') {
      // case 2: \\?\UNC\tsclient\... -> \\tsclient\...
      font_path.erase(0, 2);
      font_path[0] = L'\\';
    }
  }
  IShellItem *dir_root = nullptr;
  HRESULT hr = SHCreateItemFromParsingName(
      font_path.c_str(), nullptr, IID_IShellItem, (void **)&dir_root);
  if (FAILED(hr))
    return hr;

  LoadedFontEnumCtx *that = (LoadedFontEnumCtx *)app->alloc->alloc(
      nullptr, sizeof *that, app->alloc->arg);
  if (that == nullptr) {
    dir_root->lpVtbl->Release(dir_root);
    return E_OUTOFMEMORY;
  }
  that->lpVtbl = &kLFEC_Verb;
  that->app = app;
  that->root = dir_root;
  that->i = 0;
  that->ref = 1;
  *ppenum = (IEnumShellItems *)that;
  return S_OK;
}

int ExportLoadedFonts(HWND hWnd, FL_AppCtx *c) {
  int succ = 0;
  HRESULT hr;
  IFileDialog *pfd = nullptr;
  FILEOPENDIALOGOPTIONS options;
  IShellItem *dest = nullptr;
  IEnumShellItems *font_enum = nullptr;
  LPWSTR path_name = nullptr;
  IFileOperation *file_opt = nullptr;

  do {
    SPDLOG_INFO("ExportLoadedFonts start");
    // prepare "Select folder" dialog
    hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
        IID_IFileOpenDialog, (void **)&pfd);
    if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->GetOptions(pfd, &options);
    if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->SetOptions(pfd, options | FOS_PICKFOLDERS);
    if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->Show(pfd, hWnd);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
      // cancelled
      SPDLOG_INFO("ExportLoadedFonts cancelled by user");
      succ = 1;
      break;
    } else if (FAILED(hr))
      break;
    hr = pfd->lpVtbl->GetResult(pfd, &dest);
    if (FAILED(hr))
      break;
    SIGDN dn = SIGDN_FILESYSPATH;
    hr = dest->lpVtbl->GetDisplayName(dest, dn, &path_name);
    if (FAILED(hr))
      break;
    if (path_name) {
      std::string path_u8;
      if (Utf16ToUtf8(path_name, &path_u8)) {
        SPDLOG_INFO("Export target: {}", path_u8);
      }
    }

    // prepare font list and copy
    hr = LFEC_Create(c, &font_enum);
    if (FAILED(hr))
      break;
    hr = CoCreateInstance(
        CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_IFileOperation,
        (void **)&file_opt);
    if (FAILED(hr))
      break;
    hr = file_opt->lpVtbl->SetOwnerWindow(file_opt, hWnd);
    if (FAILED(hr))
      break;
    hr = file_opt->lpVtbl->CopyItems(file_opt, (IUnknown *)font_enum, dest);
    if (FAILED(hr))
      break;
    hr = file_opt->lpVtbl->PerformOperations(file_opt);
    if (FAILED(hr))
      break;

    ShellExecute(nullptr, nullptr, path_name, nullptr, nullptr, SW_SHOW);
    SPDLOG_INFO("ExportLoadedFonts done");
    succ = 1;
  } while (0);

  C_SAVE_RELEASE(pfd);
  C_SAVE_RELEASE(dest);
  C_SAVE_RELEASE(font_enum);
  C_SAVE_RELEASE(file_opt);
  CoTaskMemFree(path_name);
  if (!succ) {
    SPDLOG_ERROR("ExportLoadedFonts failed");
    TaskDialog(
        nullptr, c->hInst, MAKEINTRESOURCE(IDS_APP_NAME_VER), L"Error...",
        nullptr, TDCBF_CLOSE_BUTTON, TD_ERROR_ICON, nullptr);
  }
  return 0;
}
