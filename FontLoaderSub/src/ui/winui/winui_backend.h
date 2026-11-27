#pragma once

#include "ui/app_ui.h"
#include <functional>
#include <string>
#include <vector>

// View data is copied before returning; it never owns controller state.
struct AppUiButton {
  int id;
  std::wstring text;
};

struct AppUiPage {
  HINSTANCE instance = nullptr;
  std::wstring title, heading, body, details, footer;
  std::vector<AppUiButton> buttons;
  bool busy = false;
  bool confirmation = false;
  bool command_links = false;
  int default_button = 0;
};

using AppUiNotify = std::function<HRESULT(HWND, UINT, WPARAM, LPARAM)>;

// False means initialization failed before any controller work was started.
bool WinuiRun(const AppUiPage &page, const AppUiNotify &notify,
    AppUiLoadPaths load_paths, const std::wstring &font_path);
void WinuiNavigate(const AppUiPage &page);
void WinuiSetText(TASKDIALOG_ELEMENTS element, PCWSTR text);
void WinuiShowDialog(AppUiPage page, AppUiNotify notify,
    std::function<void()> completed);
void WinuiNavigateDialog(const AppUiPage &page);
bool WinuiHasDialog();
void WinuiSetStats(const AppUiStats &stats);
void WinuiSetFontResults(std::vector<AppUiFontResult> results);
void WinuiSetLibraryPath(const std::wstring &path);
void WinuiSetMenu(HMENU menu);
