#include "ui/app_ui.h"

#include "platform/win32/dark_mode.h"
#include "common/log.h"

#ifdef FONTLOADERSUB_WINUI
#include "res/resource.h"
#include "common/util.h"
#include "ui/winui/winui_backend.h"
#endif

namespace {
HWND modern_window = nullptr;

#ifdef FONTLOADERSUB_WINUI
const TASKDIALOGCONFIG *current_page = nullptr;

std::wstring Text(const TASKDIALOGCONFIG &page, PCWSTR value) {
  if (!value)
    return {};
  if (IS_INTRESOURCE(value))
    value = ResLoadString(page.hInstance, LOWORD(value));
  return value ? value : L"";
}

AppUiPage MakePage(const TASKDIALOGCONFIG &config) {
  // Resolve resources in the controller's module before reaching WinUI.
  AppUiPage page;
  page.instance = config.hInstance;
  page.title = Text(config, config.pszWindowTitle);
  page.heading = Text(config, config.pszMainInstruction);
  page.body = Text(config, config.pszContent);
  page.details = Text(config, config.pszExpandedInformation);
  page.details_label = Text(config, config.pszCollapsedControlText);
  if (page.details_label.empty())
    page.details_label = Text(config, MAKEINTRESOURCEW(IDS_DETAILS));
  page.footer = Text(config, config.pszFooter);

  // Native and modern pages use the same button IDs and controller actions.
  page.busy = (config.dwFlags & TDF_CALLBACK_TIMER) != 0;
  page.expanded = (config.dwFlags & TDF_EXPANDED_BY_DEFAULT) != 0;
  page.command_links = (config.dwFlags & TDF_USE_COMMAND_LINKS) != 0;
  page.confirmation = config.pszMainInstruction == MAKEINTRESOURCEW(IDS_MISSING_FONT);
  page.default_button = config.nDefaultButton;
  for (UINT i = 0; i < config.cButtons; ++i)
    page.buttons.push_back(
        {config.pButtons[i].nButtonID, Text(config, config.pButtons[i].pszButtonText)});

  if (config.dwCommonButtons & TDCBF_OK_BUTTON)
    page.buttons.push_back({IDOK, Text(config, MAKEINTRESOURCEW(IDS_MINIMIZE))});
  if (config.dwCommonButtons & TDCBF_CANCEL_BUTTON)
    page.buttons.push_back({IDCANCEL, Text(config, MAKEINTRESOURCEW(IDS_CANCEL))});
  if (config.dwCommonButtons & TDCBF_CLOSE_BUTTON)
    page.buttons.push_back({IDCLOSE, Text(config, MAKEINTRESOURCEW(IDS_CLOSE))});
  return page;
}

HRESULT Notify(HWND window, UINT event, WPARAM wparam, LPARAM lparam) {
  if (event == TDN_CREATED)
    modern_window = window;
  const auto *page = current_page;
  const HRESULT result = page->pfCallback
      ? page->pfCallback(window, event, wparam, lparam, page->lpCallbackData)
      : S_OK;
  if (event == TDN_DESTROYED)
    modern_window = nullptr;
  return result;
}
#endif
} // namespace

HRESULT AppUiRun(const TASKDIALOGCONFIG *config, bool force_old, AppUiLoadPaths load_paths) {
#ifdef FONTLOADERSUB_WINUI
  // The environment override also makes the compatibility UI accessible.
  wchar_t preference[16] = {};
  GetEnvironmentVariableW(L"FONTLOADERSUB_UI", preference, _countof(preference));
  current_page = config;
  if (!force_old && _wcsicmp(preference, L"win32") != 0 &&
      WinuiRun(MakePage(*config), Notify, std::move(load_paths)))
    return S_OK;
  modern_window = nullptr;
#endif
  SPDLOG_INFO("UI backend: Win32");
  return TaskDialogIndirect(config, nullptr, nullptr, nullptr);
}

void AppUiNavigate(HWND window, const TASKDIALOGCONFIG *config) {
#ifdef FONTLOADERSUB_WINUI
  if (AppUiIsModern(window)) {
    current_page = config;
    WinuiNavigate(MakePage(*config));
    Notify(window, TDN_NAVIGATED, 0, 0);
    return;
  }
#endif
  SendMessageW(window, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(config));
}

void AppUiSetText(HWND window, TASKDIALOG_ELEMENTS element, PCWSTR text) {
#ifdef FONTLOADERSUB_WINUI
  if (AppUiIsModern(window)) {
    const auto value = Text(*current_page, text);
    WinuiSetText(element, value.c_str());
    return;
  }
#endif
  SendMessageW(window, TDM_SET_ELEMENT_TEXT, element, reinterpret_cast<LPARAM>(text));
}

bool AppUiIsModern(HWND window) {
  return window && window == modern_window;
}

void AppUiStartProgress(HWND window) {
  if (!AppUiIsModern(window))
    SendMessageW(window, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
}

void AppUiThemeNotification(HWND window, UINT notification) {
  if (!AppUiIsModern(window))
    DarkModeTaskDialogNotification(window, notification);
}

bool AppUiShowDialog(const TASKDIALOGCONFIG *config, std::function<void()> completed) {
#ifdef FONTLOADERSUB_WINUI
  if (modern_window && (!config->hwndParent || AppUiIsModern(config->hwndParent))) {
    // Copy the callback, not the temporary config or its borrowed strings.
    const auto callback = config->pfCallback;
    const auto data = config->lpCallbackData;
    WinuiShowDialog(MakePage(*config),
        [callback, data](HWND window, UINT event, WPARAM wparam, LPARAM lparam) {
          return callback ? callback(window, event, wparam, lparam, data) : S_OK;
        }, std::move(completed));
    return true;
  }
#endif
  return false;
}

void AppUiNavigateDialog(HWND window, const TASKDIALOGCONFIG *config) {
#ifdef FONTLOADERSUB_WINUI
  if (AppUiIsModern(window)) {
    WinuiNavigateDialog(MakePage(*config));
    return;
  }
#endif
  SendMessageW(window, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(config));
}

bool AppUiHasDialog() {
#ifdef FONTLOADERSUB_WINUI
  return modern_window && WinuiHasDialog();
#else
  return false;
#endif
}

void AppUiSetStats(HWND window, const AppUiStats &stats) {
#ifdef FONTLOADERSUB_WINUI
  if (AppUiIsModern(window))
    WinuiSetStats(stats);
#endif
}

void AppUiSetMenu(HWND window, HMENU menu) {
#ifdef FONTLOADERSUB_WINUI
  if (AppUiIsModern(window))
    WinuiSetMenu(menu);
#endif
}
