#pragma once

// Match the Unicode Win32 declarations used by the application controller.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <Windows.h>
#include <CommCtrl.h>
#include <functional>
#include <array>
#include <string>
#include <vector>

// Keep the existing controller callbacks independent of the window renderer.
using AppUiLoadPaths = std::function<void(std::vector<std::wstring>)>;
using AppUiStats = std::array<unsigned long long, 5>;

enum class AppUiFontStatus { Loaded, Failed, Missing };
struct AppUiFontResult {
  AppUiFontStatus status;
  std::wstring name, file;
};

HRESULT AppUiRun(const TASKDIALOGCONFIG *config, bool force_old,
    AppUiLoadPaths load_paths, const std::wstring &font_path);
void AppUiNavigate(HWND window, const TASKDIALOGCONFIG *config);
void AppUiSetText(HWND window, TASKDIALOG_ELEMENTS element, PCWSTR text);
void AppUiStartProgress(HWND window);
void AppUiThemeNotification(HWND window, UINT notification);
bool AppUiIsModern(HWND window);

// Modern dialogs are asynchronous; callers retain their controller until completion.
bool AppUiShowDialog(const TASKDIALOGCONFIG *config,
    std::function<void()> completed = {});
void AppUiNavigateDialog(HWND window, const TASKDIALOGCONFIG *config);
bool AppUiHasDialog();
void AppUiSetStats(HWND window, const AppUiStats &stats);
void AppUiSetFontResults(HWND window, std::vector<AppUiFontResult> results);
void AppUiSetLibraryPath(HWND window, const std::wstring &path);
void AppUiSetMenu(HWND window, HMENU menu);
