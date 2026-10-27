#pragma once

#include <Windows.h>
#include <string>

// The bootstrapper is embedded; no application-side DLL is distributed.
class WinuiRuntime {
public:
  bool Initialize();
  ~WinuiRuntime();

private:
  HMODULE module_ = nullptr;
  bool initialized_ = false;
  std::wstring directory_, path_;
};
