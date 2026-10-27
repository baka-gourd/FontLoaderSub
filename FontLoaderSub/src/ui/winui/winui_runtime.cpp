#include "ui/winui/winui_runtime.h"

#include <appmodel.h>
#include "common/log.h"

namespace {
bool SupportedWindows() {
  // Query the real version without requiring a Windows 10 compatibility manifest.
  using VersionFn = void(WINAPI *)(DWORD *, DWORD *, DWORD *);
  auto version = reinterpret_cast<VersionFn>(GetProcAddress(
      GetModuleHandleW(L"ntdll.dll"), "RtlGetNtVersionNumbers"));
  DWORD major = 0, minor = 0, build = 0;
  if (version)
    version(&major, &minor, &build);
  return major >= 10 && (build & 0x0fffffff) >= 17763;
}
} // namespace

bool WinuiRuntime::Initialize() {
  if (!SupportedWindows())
    return false;

  // Use a new private directory, never load a DLL from the working directory.
  wchar_t temporary[MAX_PATH + 1] = {}, name[40] = {};
  GUID guid;
  const DWORD length = GetTempPathW(_countof(temporary), temporary);
  if (!length || length >= _countof(temporary) || FAILED(CoCreateGuid(&guid)))
    return false;
  StringFromGUID2(guid, name, _countof(name));
  directory_ = std::wstring(temporary) + L"FontLoaderSub-" + name;
  if (!CreateDirectoryW(directory_.c_str(), nullptr))
    return false;

  // Extract only Microsoft's small bootstrapper, not the Windows App Runtime.
  path_ = directory_ + L"\\Microsoft.WindowsAppRuntime.Bootstrap.dll";
  HMODULE instance = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10));
  HGLOBAL data = resource ? LoadResource(instance, resource) : nullptr;
  const DWORD size = resource ? SizeofResource(instance, resource) : 0;
  const void *bytes = data ? LockResource(data) : nullptr;
  if (!bytes || !size)
    return false;

  // A partial extraction must never be loaded.
  HANDLE file = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;
  DWORD written = 0;
  const BOOL saved = WriteFile(file, bytes, size, &written, nullptr);
  CloseHandle(file);
  if (!saved || written != size)
    return false;

  // Quiet initialization: missing runtimes cause fallback, never an install prompt.
  module_ = LoadLibraryExW(path_.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!module_)
    return false;
  using InitializeFn = HRESULT(WINAPI *)(UINT32, PCWSTR, PACKAGE_VERSION, UINT32);
  auto initialize = reinterpret_cast<InitializeFn>(
      GetProcAddress(module_, "MddBootstrapInitialize2"));
  const HRESULT result = initialize ? initialize(0x00010008, L"", {}, 0) : E_NOINTERFACE;
  initialized_ = SUCCEEDED(result);
  SPDLOG_INFO("WinUI runtime initialization: {:#x}", static_cast<unsigned>(result));
  return initialized_;
}

WinuiRuntime::~WinuiRuntime() {
  // The caller releases XAML objects and factory caches before this cleanup.
  if (module_) {
    using ShutdownFn = void(WINAPI *)();
    auto shutdown = reinterpret_cast<ShutdownFn>(
        GetProcAddress(module_, "MddBootstrapShutdown"));
    if (initialized_ && shutdown)
      shutdown();
    FreeLibrary(module_);
  }
  if (!path_.empty())
    DeleteFileW(path_.c_str());
  if (!directory_.empty())
    RemoveDirectoryW(directory_.c_str());
}
