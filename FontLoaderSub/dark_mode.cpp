#include "dark_mode.h"

#include <Dwmapi.h>
#include <Richedit.h>
#include <Uxtheme.h>
#include <Vssym32.h>
#include <detours/detours.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

#include "log.h"

namespace {

enum class PreferredAppMode {
  Default,
  AllowDark,
  ForceDark,
  ForceLight,
};

enum class ThemeType {
  Button,
  TaskDialog,
  Tab,
  Progress,
  Link,
  TextStyle,
};

struct ThemeEntry {
  size_t references;
  ThemeType type;
};

using RtlGetNtVersionNumbersFn = void(WINAPI *)(DWORD *, DWORD *, DWORD *);
using ShouldAppsUseDarkModeFn = bool(WINAPI *)();
using AllowDarkModeForWindowFn = bool(WINAPI *)(HWND, bool);
using AllowDarkModeForAppFn = bool(WINAPI *)(bool);
using SetPreferredAppModeFn =
    PreferredAppMode(WINAPI *)(PreferredAppMode);
using FlushMenuThemesFn = void(WINAPI *)();

using OpenThemeDataFn = HTHEME(WINAPI *)(HWND, LPCWSTR);
using OpenThemeDataExFn = HTHEME(WINAPI *)(HWND, LPCWSTR, DWORD);
using OpenThemeDataForDpiFn = HTHEME(WINAPI *)(HWND, LPCWSTR, UINT);
using CloseThemeDataFn = HRESULT(WINAPI *)(HTHEME);
using GetThemeColorFn =
    HRESULT(WINAPI *)(HTHEME, int, int, int, COLORREF *);
using DrawThemeTextFn = HRESULT(WINAPI *)(
    HTHEME, HDC, int, int, LPCWSTR, int, DWORD, DWORD, LPCRECT);
using DrawThemeTextExFn = HRESULT(WINAPI *)(
    HTHEME, HDC, int, int, LPCWSTR, int, DWORD, LPRECT, const DTTOPTS *);
using DrawThemeBackgroundFn =
    HRESULT(WINAPI *)(HTHEME, HDC, int, int, LPCRECT, LPCRECT);
using DrawThemeBackgroundExFn =
    HRESULT(WINAPI *)(HTHEME, HDC, int, int, LPCRECT, const DTBGOPTS *);
using DrawThemeParentBackgroundFn =
    HRESULT(WINAPI *)(HWND, HDC, const RECT *);
using SetTextColorFn = COLORREF(WINAPI *)(HDC, COLORREF);

constexpr DWORD kWindows10Build1809 = 17763;
constexpr DWORD kWindows10Build1903 = 18362;
constexpr DWORD kDwmUseImmersiveDarkMode = 19;
constexpr DWORD kDwmUseImmersiveDarkModeV2 = 20;
constexpr UINT_PTR kTaskDialogSubclassId = 0x464C5344;
constexpr UINT_PTR kSysLinkSubclassId = 0x464C534C;
constexpr UINT_PTR kSysLinkParentSubclassId = 0x464C5350;
constexpr UINT_PTR kRichEditSubclassId = 0x464C5352;

constexpr COLORREF kWhite = RGB(255, 255, 255);
constexpr COLORREF kLink = RGB(64, 169, 255);
constexpr COLORREF kLinkHot = RGB(105, 192, 255);
constexpr COLORREF kLinkPressed = RGB(24, 144, 255);
constexpr COLORREF kLinkDisabled = RGB(140, 140, 140);
constexpr COLORREF kEditBorder = RGB(96, 96, 96);
constexpr COLORREF kDarkBackground = RGB(36, 36, 36);
constexpr COLORREF kGrayBackground = RGB(51, 51, 51);
constexpr COLORREF kEditBackground = RGB(33, 33, 33);

std::atomic<bool> g_dark{false};
bool g_supported = false;
bool g_hooksAttached = false;
DWORD g_build = 0;

HMODULE g_uxtheme = nullptr;
ShouldAppsUseDarkModeFn g_shouldAppsUseDarkMode = nullptr;
AllowDarkModeForWindowFn g_allowDarkModeForWindow = nullptr;
AllowDarkModeForAppFn g_allowDarkModeForApp = nullptr;
SetPreferredAppModeFn g_setPreferredAppMode = nullptr;
FlushMenuThemesFn g_flushMenuThemes = nullptr;

HBRUSH g_darkBrush = nullptr;
HBRUSH g_grayBrush = nullptr;
HBRUSH g_editBrush = nullptr;

OpenThemeDataFn g_trueOpenThemeData = nullptr;
OpenThemeDataExFn g_trueOpenThemeDataEx = nullptr;
OpenThemeDataForDpiFn g_trueOpenThemeDataForDpi = nullptr;
CloseThemeDataFn g_trueCloseThemeData = nullptr;
GetThemeColorFn g_trueGetThemeColor = nullptr;
DrawThemeTextFn g_trueDrawThemeText = nullptr;
DrawThemeTextExFn g_trueDrawThemeTextEx = nullptr;
DrawThemeBackgroundFn g_trueDrawThemeBackground = nullptr;
DrawThemeBackgroundExFn g_trueDrawThemeBackgroundEx = nullptr;
DrawThemeParentBackgroundFn g_trueDrawThemeParentBackground = nullptr;
SetTextColorFn g_trueSetTextColor = nullptr;

std::mutex g_themeMutex;
std::unordered_map<HTHEME, ThemeEntry> g_themeMap;
thread_local HWND g_paintingSysLink = nullptr;

template <typename T>
T LoadFunction(HMODULE module, const char *name) {
  return reinterpret_cast<T>(GetProcAddress(module, name));
}

template <typename T>
T LoadOrdinal(HMODULE module, WORD ordinal) {
  return reinterpret_cast<T>(
      GetProcAddress(module, MAKEINTRESOURCEA(ordinal)));
}

bool StartsWithIgnoreCase(const wchar_t *value, const wchar_t *prefix) {
  if (value == nullptr || prefix == nullptr)
    return false;
  const size_t length = wcslen(prefix);
  return _wcsnicmp(value, prefix, length) == 0;
}

bool EqualsIgnoreCase(const wchar_t *left, const wchar_t *right) {
  return left != nullptr && right != nullptr && _wcsicmp(left, right) == 0;
}

DWORD GetWindowsBuild() {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto getVersion =
      LoadFunction<RtlGetNtVersionNumbersFn>(ntdll, "RtlGetNtVersionNumbers");
  if (getVersion == nullptr)
    return 0;
  DWORD major = 0;
  DWORD minor = 0;
  DWORD build = 0;
  getVersion(&major, &minor, &build);
  return build & 0x0FFFFFFF;
}

bool IsHighContrast() {
  HIGHCONTRASTW highContrast = {};
  highContrast.cbSize = sizeof highContrast;
  if (!SystemParametersInfoW(
          SPI_GETHIGHCONTRAST, sizeof highContrast, &highContrast, 0)) {
    return false;
  }
  return (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool RefreshThemeState() {
  const bool dark =
      g_supported && !IsHighContrast() && g_shouldAppsUseDarkMode != nullptr &&
      g_shouldAppsUseDarkMode();
  return g_dark.exchange(dark, std::memory_order_relaxed) != dark;
}

bool IsDark() {
  return g_dark.load(std::memory_order_relaxed);
}

void OnThemeOpened(LPCWSTR classList, HTHEME theme) {
  if (classList == nullptr || theme == nullptr)
    return;

  ThemeType type;
  if (StartsWithIgnoreCase(classList, L"Button")) {
    type = ThemeType::Button;
  } else if (StartsWithIgnoreCase(classList, L"TaskDialog")) {
    type = ThemeType::TaskDialog;
  } else if (EqualsIgnoreCase(classList, L"Tab")) {
    type = ThemeType::Tab;
  } else if (
      EqualsIgnoreCase(classList, L"Progress") ||
      EqualsIgnoreCase(classList, L"Indeterminate::Progress")) {
    type = ThemeType::Progress;
  } else if (StartsWithIgnoreCase(classList, L"Link")) {
    type = ThemeType::Link;
  } else if (StartsWithIgnoreCase(classList, L"TextStyle")) {
    type = ThemeType::TextStyle;
  } else {
    return;
  }

  std::lock_guard<std::mutex> lock(g_themeMutex);
  auto [it, inserted] =
      g_themeMap.emplace(theme, ThemeEntry{1, type});
  if (!inserted)
    ++it->second.references;
}

bool GetThemeType(HTHEME theme, ThemeType *type) {
  std::lock_guard<std::mutex> lock(g_themeMutex);
  const auto it = g_themeMap.find(theme);
  if (it == g_themeMap.end())
    return false;
  *type = it->second.type;
  return true;
}

HTHEME WINAPI DarkOpenThemeData(HWND hwnd, LPCWSTR classList) {
  HTHEME theme = g_trueOpenThemeData(hwnd, classList);
  OnThemeOpened(classList, theme);
  return theme;
}

HTHEME WINAPI
DarkOpenThemeDataEx(HWND hwnd, LPCWSTR classList, DWORD flags) {
  HTHEME theme = g_trueOpenThemeDataEx(hwnd, classList, flags);
  OnThemeOpened(classList, theme);
  return theme;
}

HTHEME WINAPI
DarkOpenThemeDataForDpi(HWND hwnd, LPCWSTR classList, UINT dpi) {
  HTHEME theme = g_trueOpenThemeDataForDpi(hwnd, classList, dpi);
  OnThemeOpened(classList, theme);
  return theme;
}

HRESULT WINAPI DarkCloseThemeData(HTHEME theme) {
  {
    std::lock_guard<std::mutex> lock(g_themeMutex);
    const auto it = g_themeMap.find(theme);
    if (it != g_themeMap.end()) {
      if (--it->second.references == 0)
        g_themeMap.erase(it);
    }
  }
  return g_trueCloseThemeData(theme);
}

bool GetDarkThemeTextColor(
    ThemeType type,
    int part,
    int state,
    COLORREF *color) {
  switch (type) {
  case ThemeType::TaskDialog:
    *color =
        part == TDLG_MAININSTRUCTIONPANE
            ? RGB(128, 190, 255)
            : kWhite;
    return true;
  case ThemeType::Button:
    if ((part == BP_CHECKBOX || part == BP_RADIOBUTTON) &&
        state != PBS_DISABLED) {
      *color = kWhite;
      return true;
    }
    return false;
  case ThemeType::Tab:
    *color = kWhite;
    return true;
  case ThemeType::Link:
    if (part != LP_HYPERLINK)
      return false;
    *color = state == HLS_NORMALTEXT ? kWhite : kLink;
    return true;
  case ThemeType::TextStyle:
    if (part != TEXT_HYPERLINKTEXT)
      return false;
    switch (state) {
    case TS_HYPERLINK_HOT:
      *color = kLinkHot;
      break;
    case TS_HYPERLINK_PRESSED:
      *color = kLinkPressed;
      break;
    case TS_HYPERLINK_DISABLED:
      *color = kLinkDisabled;
      break;
    default:
      *color = kLink;
      break;
    }
    return true;
  case ThemeType::Progress:
    return false;
  }
  return false;
}

HRESULT WINAPI DarkGetThemeColor(
    HTHEME theme,
    int part,
    int state,
    int property,
    COLORREF *color) {
  const HRESULT result =
      g_trueGetThemeColor(theme, part, state, property, color);
  ThemeType type;
  COLORREF darkColor = 0;
  if (IsDark() && color != nullptr && property == TMT_TEXTCOLOR &&
      GetThemeType(theme, &type) &&
      GetDarkThemeTextColor(type, part, state, &darkColor)) {
    *color = darkColor;
  }
  return result;
}

HRESULT WINAPI DarkDrawThemeText(
    HTHEME theme,
    HDC dc,
    int part,
    int state,
    LPCWSTR text,
    int textLength,
    DWORD textFlags,
    DWORD textFlags2,
    LPCRECT rect) {
  ThemeType type;
  COLORREF darkColor = 0;
  if (IsDark() && GetThemeType(theme, &type) &&
      GetDarkThemeTextColor(type, part, state, &darkColor)) {
    DTTOPTS options = {};
    options.dwSize = sizeof options;
    options.dwFlags = DTT_TEXTCOLOR;
    options.crText = darkColor;
    return g_trueDrawThemeTextEx(
        theme, dc, part, state, text, textLength, textFlags,
        const_cast<RECT *>(rect), &options);
  }
  return g_trueDrawThemeText(
      theme, dc, part, state, text, textLength, textFlags, textFlags2, rect);
}

HRESULT WINAPI DarkDrawThemeTextEx(
    HTHEME theme,
    HDC dc,
    int part,
    int state,
    LPCWSTR text,
    int textLength,
    DWORD textFlags,
    LPRECT rect,
    const DTTOPTS *sourceOptions) {
  ThemeType type;
  COLORREF darkColor = 0;
  if (IsDark() && GetThemeType(theme, &type) &&
      GetDarkThemeTextColor(type, part, state, &darkColor)) {
    DTTOPTS options = {};
    if (sourceOptions != nullptr &&
        sourceOptions->dwSize >= sizeof(DTTOPTS)) {
      options = *sourceOptions;
    }
    options.dwSize = sizeof options;
    options.dwFlags |= DTT_TEXTCOLOR;
    options.crText = darkColor;
    return g_trueDrawThemeTextEx(
        theme, dc, part, state, text, textLength, textFlags, rect, &options);
  }
  return g_trueDrawThemeTextEx(
      theme, dc, part, state, text, textLength, textFlags, rect,
      sourceOptions);
}

HRESULT FillThemeRect(
    HDC dc,
    LPCRECT rect,
    const DTBGOPTS *options,
    HBRUSH brush) {
  if (dc == nullptr || rect == nullptr || brush == nullptr)
    return E_INVALIDARG;
  LPCRECT fillRect = rect;
  if (options != nullptr && (options->dwFlags & DTBG_CLIPRECT) != 0)
    fillRect = &options->rcClip;
  FillRect(dc, fillRect, brush);
  return S_OK;
}

HRESULT WINAPI DarkDrawThemeBackgroundEx(
    HTHEME theme,
    HDC dc,
    int part,
    int state,
    LPCRECT rect,
    const DTBGOPTS *options) {
  ThemeType type;
  if (IsDark() && GetThemeType(theme, &type)) {
    if (type == ThemeType::Progress && part == PP_TRANSPARENTBAR)
      return FillThemeRect(dc, rect, options, g_grayBrush);
    if (type == ThemeType::Progress &&
        (part == PP_PULSEOVERLAY || part == PP_PULSEOVERLAYVERT))
      return S_OK;
    if (type == ThemeType::TaskDialog) {
      if (part == TDLG_PRIMARYPANEL ||
          part == TDLG_MAININSTRUCTIONPANE ||
          part == TDLG_CONTENTPANE ||
          part == TDLG_EXPANDEDCONTENT ||
          part == TDLG_COMMANDLINKPANE ||
          part == TDLG_RADIOBUTTONPANE) {
        return FillThemeRect(dc, rect, options, g_grayBrush);
      }
      if (part == TDLG_SECONDARYPANEL ||
          part == TDLG_CONTROLPANE ||
          part == TDLG_BUTTONSECTION ||
          part == TDLG_BUTTONWRAPPER ||
          part == TDLG_FOOTNOTEPANE ||
          part == TDLG_FOOTNOTEAREA ||
          part == TDLG_EXPANDEDFOOTERAREA ||
          part == TDLG_PROGRESSBAR) {
        return FillThemeRect(dc, rect, options, g_darkBrush);
      }
      if (part == TDLG_FOOTNOTESEPARATOR)
        return FillThemeRect(dc, rect, options, g_grayBrush);
    }
  }
  return g_trueDrawThemeBackgroundEx(
      theme, dc, part, state, rect, options);
}

HRESULT WINAPI DarkDrawThemeBackground(
    HTHEME theme,
    HDC dc,
    int part,
    int state,
    LPCRECT rect,
    LPCRECT clipRect) {
  DTBGOPTS options = {};
  options.dwSize = sizeof options;
  if (clipRect != nullptr) {
    options.dwFlags = DTBG_CLIPRECT;
    options.rcClip = *clipRect;
  }
  return DarkDrawThemeBackgroundEx(
      theme, dc, part, state, rect, &options);
}

HRESULT WINAPI
DarkDrawThemeParentBackground(HWND hwnd, HDC dc, const RECT *rect) {
  return g_trueDrawThemeParentBackground(hwnd, dc, rect);
}

COLORREF WINAPI DarkSetTextColor(HDC dc, COLORREF color) {
  if (!IsDark() || g_paintingSysLink == nullptr)
    return g_trueSetTextColor(dc, color);

  COLORREF mapped = kWhite;
  if (!IsWindowEnabled(g_paintingSysLink)) {
    mapped = kLinkDisabled;
  } else {
    const int red = GetRValue(color);
    const int green = GetGValue(color);
    const int blue = GetBValue(color);
    const int maximum = (std::max)({red, green, blue});
    const int minimum = (std::min)({red, green, blue});
    if (maximum - minimum > 24) {
      POINT cursor = {};
      bool hot = false;
      if (GetCursorPos(&cursor) &&
          ScreenToClient(g_paintingSysLink, &cursor)) {
        LHITTESTINFO hit = {};
        hit.pt = cursor;
        hot =
            SendMessageW(
                g_paintingSysLink, LM_HITTEST, 0,
                reinterpret_cast<LPARAM>(&hit)) >= 0;
      }
      mapped =
          hot && (GetKeyState(VK_LBUTTON) & 0x8000) != 0
              ? kLinkPressed
              : (hot ? kLinkHot : kLink);
    }
  }
  return g_trueSetTextColor(dc, mapped);
}

bool AttachOne(PVOID *target, PVOID detour) {
  return *target == nullptr ||
         DetourAttach(target, detour) == NO_ERROR;
}

bool DetachOne(PVOID *target, PVOID detour) {
  return *target == nullptr ||
         DetourDetach(target, detour) == NO_ERROR;
}

bool AttachHooks() {
  if (DetourTransactionBegin() != NO_ERROR)
    return false;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueOpenThemeData),
          reinterpret_cast<PVOID>(DarkOpenThemeData)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueOpenThemeDataEx),
          reinterpret_cast<PVOID>(DarkOpenThemeDataEx)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueOpenThemeDataForDpi),
          reinterpret_cast<PVOID>(DarkOpenThemeDataForDpi)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueCloseThemeData),
          reinterpret_cast<PVOID>(DarkCloseThemeData)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueGetThemeColor),
          reinterpret_cast<PVOID>(DarkGetThemeColor)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeText),
          reinterpret_cast<PVOID>(DarkDrawThemeText)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeTextEx),
          reinterpret_cast<PVOID>(DarkDrawThemeTextEx)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeBackground),
          reinterpret_cast<PVOID>(DarkDrawThemeBackground)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeBackgroundEx),
          reinterpret_cast<PVOID>(DarkDrawThemeBackgroundEx)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeParentBackground),
          reinterpret_cast<PVOID>(DarkDrawThemeParentBackground)) ||
      !AttachOne(
          reinterpret_cast<PVOID *>(&g_trueSetTextColor),
          reinterpret_cast<PVOID>(DarkSetTextColor))) {
    DetourTransactionAbort();
    return false;
  }
  return DetourTransactionCommit() == NO_ERROR;
}

void DetachHooks() {
  if (!g_hooksAttached || DetourTransactionBegin() != NO_ERROR)
    return;
  if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueOpenThemeData),
          reinterpret_cast<PVOID>(DarkOpenThemeData)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueOpenThemeDataEx),
          reinterpret_cast<PVOID>(DarkOpenThemeDataEx)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueOpenThemeDataForDpi),
          reinterpret_cast<PVOID>(DarkOpenThemeDataForDpi)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueCloseThemeData),
          reinterpret_cast<PVOID>(DarkCloseThemeData)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueGetThemeColor),
          reinterpret_cast<PVOID>(DarkGetThemeColor)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeText),
          reinterpret_cast<PVOID>(DarkDrawThemeText)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeTextEx),
          reinterpret_cast<PVOID>(DarkDrawThemeTextEx)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeBackground),
          reinterpret_cast<PVOID>(DarkDrawThemeBackground)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeBackgroundEx),
          reinterpret_cast<PVOID>(DarkDrawThemeBackgroundEx)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueDrawThemeParentBackground),
          reinterpret_cast<PVOID>(DarkDrawThemeParentBackground)) ||
      !DetachOne(
          reinterpret_cast<PVOID *>(&g_trueSetTextColor),
          reinterpret_cast<PVOID>(DarkSetTextColor))) {
    DetourTransactionAbort();
    return;
  }
  if (DetourTransactionCommit() == NO_ERROR)
    g_hooksAttached = false;
}

bool GetClassName(HWND hwnd, wchar_t (&className)[128]) {
  return GetClassNameW(hwnd, className, _countof(className)) != 0;
}

bool IsRichEdit(const wchar_t *className) {
  return StartsWithIgnoreCase(className, L"RichEdit") ||
         StartsWithIgnoreCase(className, L"RICHEDIT");
}

void DrawRichEditBorder(HWND hwnd) {
  HDC dc = GetWindowDC(hwnd);
  if (dc == nullptr)
    return;

  RECT rect = {};
  if (GetWindowRect(hwnd, &rect)) {
    OffsetRect(&rect, -rect.left, -rect.top);
    const COLORREF borderColor =
        IsDark()
            ? (GetFocus() == hwnd ? kLink : kEditBorder)
            : (GetFocus() == hwnd ? GetSysColor(COLOR_HIGHLIGHT)
                                  : GetSysColor(COLOR_WINDOWFRAME));
    SetDCBrushColor(dc, borderColor);
    FrameRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
  }
  ReleaseDC(hwnd, dc);
}

LRESULT CALLBACK RichEditSubclass(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR) {
  if (message == WM_NCCALCSIZE) {
    const LRESULT result =
        DefSubclassProc(hwnd, message, wParam, lParam);
    RECT *clientRect = nullptr;
    if (wParam != FALSE) {
      auto params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lParam);
      clientRect = &params->rgrc[0];
    } else {
      clientRect = reinterpret_cast<RECT *>(lParam);
    }
    if (clientRect != nullptr &&
        clientRect->right - clientRect->left > 2 &&
        clientRect->bottom - clientRect->top > 2) {
      InflateRect(clientRect, -1, -1);
    }
    return result;
  }
  if (message == WM_NCPAINT) {
    const LRESULT result =
        DefSubclassProc(hwnd, message, wParam, lParam);
    DrawRichEditBorder(hwnd);
    return result;
  }
  if (message == WM_SETFOCUS || message == WM_KILLFOCUS ||
      message == WM_SETTINGCHANGE || message == WM_THEMECHANGED) {
    const LRESULT result =
        DefSubclassProc(hwnd, message, wParam, lParam);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    return result;
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, RichEditSubclass, kRichEditSubclassId);
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK SysLinkSubclass(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR) {
  if (message == WM_PAINT || message == WM_PRINTCLIENT) {
    HWND previous = g_paintingSysLink;
    g_paintingSysLink = hwnd;
    const LRESULT result =
        DefSubclassProc(hwnd, message, wParam, lParam);
    g_paintingSysLink = previous;
    return result;
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, SysLinkSubclass, kSysLinkSubclassId);
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK SysLinkParentSubclass(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR) {
  if (message == WM_CTLCOLORSTATIC && IsDark()) {
    HWND child = reinterpret_cast<HWND>(lParam);
    wchar_t className[128] = {};
    if (child != nullptr && GetClassName(child, className) &&
        EqualsIgnoreCase(className, WC_LINK)) {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, kWhite);
      SetBkMode(dc, TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
  } else if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(
        hwnd, SysLinkParentSubclass, kSysLinkParentSubclassId);
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

void ApplyRichEditColors(HWND hwnd, bool dark) {
  SendMessageW(
      hwnd, EM_SETBKGNDCOLOR, 0,
      dark ? kEditBackground : GetSysColor(COLOR_WINDOW));
  CHARFORMAT2W format = {};
  format.cbSize = sizeof format;
  format.dwMask = CFM_COLOR;
  format.crTextColor =
      dark ? kWhite : GetSysColor(COLOR_WINDOWTEXT);
  SendMessageW(
      hwnd, EM_SETCHARFORMAT, SCF_ALL,
      reinterpret_cast<LPARAM>(&format));
  SendMessageW(
      hwnd, EM_SETCHARFORMAT, SCF_DEFAULT,
      reinterpret_cast<LPARAM>(&format));
}

void ApplyControlTheme(HWND hwnd, bool taskDialog) {
  wchar_t className[128] = {};
  if (!GetClassName(hwnd, className))
    return;

  const bool dark = IsDark();
  const bool progress = EqualsIgnoreCase(className, PROGRESS_CLASSW);
  const bool richEdit = IsRichEdit(className);
  if (richEdit && g_allowDarkModeForWindow != nullptr)
    g_allowDarkModeForWindow(hwnd, dark);
  LPCWSTR theme = nullptr;
  if (dark) {
    if (EqualsIgnoreCase(className, WC_COMBOBOXW)) {
      theme = L"DarkMode_CFD";
    } else if (EqualsIgnoreCase(className, WC_EDITW)) {
      const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
      theme =
          (style & ES_MULTILINE) != 0
              ? L"DarkMode_Explorer"
              : L"DarkMode_CFD";
    } else if (progress) {
      // Keep the native progress theme so Windows retains its original
      // marquee geometry, green fill, and animation timing.
      theme = nullptr;
    } else if (taskDialog && EqualsIgnoreCase(className, WC_BUTTONW)) {
      theme = L"DarkMode_Explorer";
    } else {
      theme = L"DarkMode_Explorer";
    }
  } else if (taskDialog && EqualsIgnoreCase(className, WC_BUTTONW)) {
    theme = L"Explorer";
  }

  SetWindowTheme(hwnd, theme, nullptr);
  if (richEdit) {
    DWORD_PTR subclassData = 0;
    if (!GetWindowSubclass(
            hwnd, RichEditSubclass, kRichEditSubclassId,
            &subclassData)) {
      SetWindowSubclass(
          hwnd, RichEditSubclass, kRichEditSubclassId, 0);
      SetWindowPos(
          hwnd, nullptr, 0, 0, 0, 0,
          SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
              SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    ApplyRichEditColors(hwnd, dark);
  }
  if (taskDialog && EqualsIgnoreCase(className, WC_LINK)) {
    SetWindowSubclass(hwnd, SysLinkSubclass, kSysLinkSubclassId, 0);
    HWND parent = GetParent(hwnd);
    if (parent != nullptr) {
      SetWindowSubclass(
          parent, SysLinkParentSubclass, kSysLinkParentSubclassId, 0);
    }
  }
  if (richEdit) {
    RedrawWindow(
        hwnd, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
  } else {
    InvalidateRect(hwnd, nullptr, TRUE);
  }
}

BOOL CALLBACK ApplyChildTheme(HWND hwnd, LPARAM lParam) {
  ApplyControlTheme(hwnd, lParam != 0);
  return TRUE;
}

void ApplyWindowTheme(HWND hwnd, bool taskDialog) {
  if (hwnd == nullptr || !IsWindow(hwnd))
    return;

  const BOOL dark = IsDark() ? TRUE : FALSE;
  if (g_allowDarkModeForWindow != nullptr)
    g_allowDarkModeForWindow(hwnd, dark != FALSE);

  HRESULT result = DwmSetWindowAttribute(
      hwnd, kDwmUseImmersiveDarkModeV2, &dark, sizeof dark);
  if (FAILED(result)) {
    DwmSetWindowAttribute(
        hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof dark);
  }
  if (g_flushMenuThemes != nullptr)
    g_flushMenuThemes();

  EnumChildWindows(
      hwnd, ApplyChildTheme, taskDialog ? static_cast<LPARAM>(1) : 0);
  RedrawWindow(
      hwnd, nullptr, nullptr,
      RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

LRESULT HandleControlColor(UINT message, WPARAM wParam) {
  HDC dc = reinterpret_cast<HDC>(wParam);
  SetTextColor(dc, kWhite);
  if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
    SetBkColor(dc, kEditBackground);
    return reinterpret_cast<LRESULT>(g_editBrush);
  }
  SetBkMode(dc, TRANSPARENT);
  return reinterpret_cast<LRESULT>(g_grayBrush);
}

LRESULT CALLBACK TaskDialogSubclass(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR,
    DWORD_PTR) {
  switch (message) {
  case WM_SETTINGCHANGE:
  case WM_THEMECHANGED:
    RefreshThemeState();
    ApplyWindowTheme(hwnd, true);
    break;
  case WM_CTLCOLORDLG:
    if (IsDark())
      return reinterpret_cast<LRESULT>(g_darkBrush);
    break;
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
    if (IsDark())
      return HandleControlColor(message, wParam);
    break;
  case WM_NCDESTROY:
    RemoveWindowSubclass(
        hwnd, TaskDialogSubclass, kTaskDialogSubclassId);
    break;
  default:
    break;
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

HRESULT CALLBACK BasicTaskDialogCallback(
    HWND hwnd,
    UINT notification,
    WPARAM,
    LPARAM,
    LONG_PTR) {
  DarkModeTaskDialogNotification(hwnd, notification);
  return S_OK;
}

}  // namespace

bool DarkModeInitialize() {
  g_build = GetWindowsBuild();
  g_uxtheme = LoadLibraryW(L"uxtheme.dll");
  if (g_uxtheme == nullptr || g_build < kWindows10Build1809)
    return false;

  g_shouldAppsUseDarkMode =
      LoadOrdinal<ShouldAppsUseDarkModeFn>(g_uxtheme, 132);
  g_allowDarkModeForWindow =
      LoadOrdinal<AllowDarkModeForWindowFn>(g_uxtheme, 133);
  g_flushMenuThemes = LoadOrdinal<FlushMenuThemesFn>(g_uxtheme, 136);
  if (g_build < kWindows10Build1903) {
    g_allowDarkModeForApp =
        LoadOrdinal<AllowDarkModeForAppFn>(g_uxtheme, 135);
  } else {
    g_setPreferredAppMode =
        LoadOrdinal<SetPreferredAppModeFn>(g_uxtheme, 135);
  }

  g_supported =
      g_shouldAppsUseDarkMode != nullptr &&
      g_allowDarkModeForWindow != nullptr &&
      g_flushMenuThemes != nullptr &&
      (g_allowDarkModeForApp != nullptr ||
       g_setPreferredAppMode != nullptr);
  if (!g_supported)
    return false;

  if (g_setPreferredAppMode != nullptr)
    g_setPreferredAppMode(PreferredAppMode::AllowDark);
  else
    g_allowDarkModeForApp(true);

  g_darkBrush = CreateSolidBrush(kDarkBackground);
  g_grayBrush = CreateSolidBrush(kGrayBackground);
  g_editBrush = CreateSolidBrush(kEditBackground);
  if (g_darkBrush == nullptr || g_grayBrush == nullptr ||
      g_editBrush == nullptr) {
    DarkModeShutdown();
    return false;
  }

  g_trueOpenThemeData =
      LoadFunction<OpenThemeDataFn>(g_uxtheme, "OpenThemeData");
  g_trueOpenThemeDataEx =
      LoadFunction<OpenThemeDataExFn>(g_uxtheme, "OpenThemeDataEx");
  g_trueOpenThemeDataForDpi =
      LoadFunction<OpenThemeDataForDpiFn>(
          g_uxtheme, "OpenThemeDataForDpi");
  g_trueCloseThemeData =
      LoadFunction<CloseThemeDataFn>(g_uxtheme, "CloseThemeData");
  g_trueGetThemeColor =
      LoadFunction<GetThemeColorFn>(g_uxtheme, "GetThemeColor");
  g_trueDrawThemeText =
      LoadFunction<DrawThemeTextFn>(g_uxtheme, "DrawThemeText");
  g_trueDrawThemeTextEx =
      LoadFunction<DrawThemeTextExFn>(g_uxtheme, "DrawThemeTextEx");
  g_trueDrawThemeBackground =
      LoadFunction<DrawThemeBackgroundFn>(
          g_uxtheme, "DrawThemeBackground");
  g_trueDrawThemeBackgroundEx =
      LoadFunction<DrawThemeBackgroundExFn>(
          g_uxtheme, "DrawThemeBackgroundEx");
  g_trueDrawThemeParentBackground =
      LoadFunction<DrawThemeParentBackgroundFn>(
          g_uxtheme, "DrawThemeParentBackground");
  HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
  if (gdi32 == nullptr)
    gdi32 = LoadLibraryW(L"gdi32.dll");
  g_trueSetTextColor =
      LoadFunction<SetTextColorFn>(gdi32, "SetTextColor");

  RefreshThemeState();
  g_hooksAttached = AttachHooks();
  if (!g_hooksAttached)
    SPDLOG_WARN("Failed to install uxtheme dark-mode hooks");
  return true;
}

void DarkModeShutdown() {
  DetachHooks();
  {
    std::lock_guard<std::mutex> lock(g_themeMutex);
    g_themeMap.clear();
  }
  if (g_darkBrush != nullptr)
    DeleteObject(g_darkBrush);
  if (g_grayBrush != nullptr)
    DeleteObject(g_grayBrush);
  if (g_editBrush != nullptr)
    DeleteObject(g_editBrush);
  g_darkBrush = nullptr;
  g_grayBrush = nullptr;
  g_editBrush = nullptr;
  g_dark.store(false, std::memory_order_relaxed);
  g_supported = false;
}

void DarkModeTaskDialogNotification(HWND hwnd, UINT notification) {
  if (!g_supported)
    return;
  if (notification == TDN_CREATED) {
    SetWindowSubclass(
        hwnd, TaskDialogSubclass, kTaskDialogSubclassId, 0);
  }
  if (notification == TDN_CREATED ||
      notification == TDN_DIALOG_CONSTRUCTED ||
      notification == TDN_NAVIGATED) {
    RefreshThemeState();
    ApplyWindowTheme(hwnd, true);
  } else if (notification == TDN_DESTROYED) {
    RemoveWindowSubclass(
        hwnd, TaskDialogSubclass, kTaskDialogSubclassId);
  }
}

bool DarkModeHandleDialogMessage(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM,
    LRESULT *result) {
  if (!g_supported)
    return false;

  if (message == WM_INITDIALOG) {
    RefreshThemeState();
    ApplyWindowTheme(hwnd, false);
    return false;
  }
  if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED) {
    RefreshThemeState();
    ApplyWindowTheme(hwnd, false);
    return false;
  }
  if (!IsDark())
    return false;

  if (message == WM_CTLCOLORDLG) {
    *result = reinterpret_cast<LRESULT>(g_grayBrush);
    return true;
  }
  if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN ||
      message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
    *result = HandleControlColor(message, wParam);
    return true;
  }
  return false;
}

HRESULT AppTaskDialog(
    HWND owner,
    HINSTANCE instance,
    PCWSTR windowTitle,
    PCWSTR mainInstruction,
    PCWSTR content,
    TASKDIALOG_COMMON_BUTTON_FLAGS commonButtons,
    PCWSTR icon,
    int *button) {
  TASKDIALOGCONFIG config = {};
  config.cbSize = sizeof config;
  config.hwndParent = owner;
  config.hInstance = instance;
  config.pszWindowTitle = windowTitle;
  config.pszMainInstruction = mainInstruction;
  config.pszContent = content;
  config.dwCommonButtons = commonButtons;
  config.pszMainIcon = icon;
  config.dwFlags =
      TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  config.pfCallback = BasicTaskDialogCallback;
  return TaskDialogIndirect(&config, button, nullptr, nullptr);
}
