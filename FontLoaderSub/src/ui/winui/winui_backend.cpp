#include "ui/winui/winui_backend.h"

#undef GetCurrentTime
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Storage.h>
#include <microsoft.ui.xaml.window.h>
#include <deque>
#include <algorithm>

#include "common/log.h"
#include "ui/winui/winui_runtime.h"
#include "res/resource.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Markup;
using namespace Microsoft::UI::Xaml::XamlTypeInfo;

namespace {
class App;
App *active = nullptr;
constexpr int kMinWindowWidth = 960;
constexpr int kMinWindowHeight = 600;
constexpr UINT_PTR kWindowSizeSubclass = 0x464C02;

struct DialogRequest {
  AppUiPage page;
  AppUiNotify notify;
  std::function<void()> completed;
};

std::wstring ButtonLabel(std::wstring value) {
  // TaskDialog accelerators are not literal text in XAML controls.
  for (size_t pos; (pos = value.find(L"(&")) != std::wstring::npos; ) {
    if (pos + 3 >= value.size() || value[pos + 3] != L')')
      break;
    value.erase(pos, 4);
  }
  for (size_t pos = 0; (pos = value.find(L'&', pos)) != std::wstring::npos; )
    value.erase(pos, 1);
  return value;
}

class App : public ApplicationT<App, IXamlMetadataProvider> {
public:
  App(const AppUiPage &page, const AppUiNotify &notify, AppUiLoadPaths load_paths)
      : initial_(page), notify_(notify), load_paths_(std::move(load_paths)) {}

  // Expose built-in control metadata without a XAML compiler or MSBuild project.
  IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const &type) {
    return provider_.GetXamlType(type);
  }
  IXamlType GetXamlType(hstring const &name) {
    return provider_.GetXamlType(name);
  }
  com_array<XmlnsDefinition> GetXmlnsDefinitions() {
    return provider_.GetXmlnsDefinitions();
  }

  void OnLaunched(LaunchActivatedEventArgs const &) {
    // Fall back only before notifying the controller and starting its worker.
    try {
      Resources().MergedDictionaries().Append(XamlControlsResources());
      window_ = Window();
      check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&hwnd_));
      SetWindowSubclass(hwnd_, WindowSizeProc, kWindowSizeSubclass,
          reinterpret_cast<DWORD_PTR>(this));
      active = this;
      Build();
      Navigate(initial_);
      window_.Activate();

      // Keep cancellation asynchronous: closing waits for font unloading.
      window_.Closed([this](auto &&, WindowEventArgs const &args) {
        if (dialog_) {
          args.Handled(true);
          dialog_.Hide();
          return;
        }
        if (started_ && notify_(hwnd_, TDN_BUTTON_CLICKED, IDCANCEL, 0) != S_OK) {
          args.Handled(true);
          return;
        }
        SaveWindowSize();
        timer_.Stop();
        if (started_)
          notify_(hwnd_, TDN_DESTROYED, 0, 0);
      });

      // Poll on the UI thread, matching the existing controller's timer contract.
      timer_ = DispatcherTimer();
      timer_.Interval(std::chrono::milliseconds(200));
      timer_.Tick([this](auto &&, auto &&) {
        if (busy_ && !dialogs_running_)
          notify_(hwnd_, TDN_TIMER, 0, 0);
      });
      started_ = true;
      SPDLOG_INFO("UI backend: WinUI 3");
      notify_(hwnd_, TDN_CREATED, 0, 0);
      timer_.Start();
    } catch (const hresult_error &error) {
      (void)error;
      SPDLOG_WARN("WinUI window initialization failed: {:#x}",
          static_cast<unsigned>(error.code().value));
      Exit();
    }
  }

  void Navigate(const AppUiPage &page) {
    // Confirmation is modal while the previous status page remains visible.
    if (page.confirmation) {
      busy_ = false;
      progress_.IsIndeterminate(false);
      ShowDialog(page, notify_, {});
      return;
    }

    // Keep controls stable while replacing the page's display data and actions.
    busy_ = page.busy;
    add_.IsEnabled(!busy_);
    tools_.IsEnabled(!busy_);
    window_.Title(page.title);
    caption_.Text(page.title);
    heading_.Text(page.heading);
    body_.Text(page.body);
    progress_.Visibility(busy_ ? Visibility::Visible : Visibility::Collapsed);
    progress_.IsIndeterminate(busy_);
    details_.Text(page.details.empty() ? Resource(IDS_NO_DETAILS) : hstring(page.details));
    expander_.Header(box_value(Resource(IDS_FONT_DETAILS)));
    expander_.IsExpanded(page.expanded);
    status_icon_.Glyph(busy_ ? L"\xE895" : L"\xE73E");

    // The footer's only action is the project's existing repository hyperlink.
    footer_.Visibility(page.footer.empty() ? Visibility::Collapsed : Visibility::Visible);
    buttons_.Children().Clear();
    for (const auto &action : page.buttons) {
      if (action.id == ID_BTN_MENU || action.id == IDOK || action.id == IDCLOSE)
        continue;
      Button button;
      button.Content(box_value(action.id == IDCLOSE
          ? Resource(IDS_EXIT_UNLOAD) : hstring(ButtonLabel(action.text))));
      button.MinWidth(96);
      button.Click([this, id = action.id](auto &&, auto &&) {
        if (notify_(hwnd_, TDN_BUTTON_CLICKED, id, 0) == S_OK)
          window_.Close();
      });

      // Focus the safe default, including Cancel on missing-font confirmation.
      if (busy_ && action.id == page.default_button) {
        button.Loaded([](auto const &sender, auto &&) {
          sender.template as<Button>().Focus(FocusState::Programmatic);
        });
      }
      buttons_.Children().Append(button);
    }
  }

  void SetText(TASKDIALOG_ELEMENTS element, PCWSTR text) {
    if (element == TDE_MAIN_INSTRUCTION)
      heading_.Text(text ? text : L"");
    else if (element == TDE_CONTENT)
      body_.Text(text ? text : L"");
  }

  bool Started() const { return started_; }

  void ShowDialog(AppUiPage page, AppUiNotify notify, std::function<void()> completed) {
    dialogs_.push_back({std::move(page), std::move(notify), std::move(completed)});
    if (!dialogs_running_) {
      dialogs_running_ = true;
      DrainDialogs();
    }
  }

  bool HasDialog() const { return dialogs_running_; }

  void UpdateDialog(const AppUiPage &page) {
    if (dialog_)
      SetDialogContent(page);
  }

  void SetStats(const AppUiStats &stats) {
    // Announce counts by label, and avoid rewriting unchanged values every timer tick.
    for (size_t i = 0; i < stats.size(); ++i) {
      const auto value = to_hstring(stats[i]);
      if (counts_[i].Text() == value)
        continue;
      counts_[i].Text(value);
      Automation::AutomationProperties::SetName(counts_[i], labels_[i] + L": " + value);
    }
  }

  void SetMenu(HMENU menu) {
    // Keep enabled states and command IDs sourced from the shared controller.
    MenuFlyout flyout;
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
      const UINT id = GetMenuItemID(menu, i);
      if (id == 0) {
        flyout.Items().Append(MenuFlyoutSeparator());
        continue;
      }

      wchar_t text[256] = {};
      GetMenuStringW(menu, i, text, _countof(text), MF_BYPOSITION);
      MenuFlyoutItem item;
      item.Text(ButtonLabel(text));
      item.IsEnabled(!(GetMenuState(menu, i, MF_BYPOSITION) & (MF_DISABLED | MF_GRAYED)));
      item.Click([this, id](auto &&, auto &&) { notify_(hwnd_, TDN_BUTTON_CLICKED, id, 0); });
      flyout.Items().Append(item);
    }
    tools_.Flyout(flyout);
  }

private:
  static LRESULT CALLBACK WindowSizeProc(HWND window, UINT message, WPARAM wparam,
      LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    // Enforce the same logical minimum while resizing on any DPI-scaled monitor.
    if (message == WM_GETMINMAXINFO) {
      auto app = reinterpret_cast<App *>(data);
      auto limits = reinterpret_cast<MINMAXINFO *>(lparam);
      const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
      limits->ptMinTrackSize.x = MulDiv(kMinWindowWidth, app->WindowDpi(), 96);
      limits->ptMinTrackSize.y = MulDiv(kMinWindowHeight, app->WindowDpi(), 96);
      return result;
    }
    if (message == WM_NCDESTROY)
      RemoveWindowSubclass(window, WindowSizeProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
  }

  UINT WindowDpi() const {
    // Resolve the newer API dynamically to preserve the Win32 startup fallback.
    using DpiFn = UINT(WINAPI *)(HWND);
    auto dpi = reinterpret_cast<DpiFn>(GetProcAddress(
        GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    const UINT value = dpi ? dpi(hwnd_) : 96;
    return value ? value : 96;
  }

  void SaveWindowSize() {
    // Persist the normal size, even when closing a maximized or minimized window.
    WINDOWPLACEMENT placement = {sizeof placement};
    if (!GetWindowPlacement(hwnd_, &placement))
      return;
    const auto &rect = placement.rcNormalPosition;
    const int width = MulDiv(rect.right - rect.left, 96, WindowDpi());
    const int height = MulDiv(rect.bottom - rect.top, 96, WindowDpi());
    if (width <= 0 || height <= 0)
      return;

    // Store both dimensions together in logical pixels, without a sidecar file.
    const DWORD size[] = {static_cast<DWORD>(width), static_cast<DWORD>(height)};
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\FontLoaderSub", 0, nullptr,
            0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
      RegSetValueExW(key, L"WinuiWindowSize", 0, REG_BINARY,
          reinterpret_cast<const BYTE *>(size), sizeof size);
      RegCloseKey(key);
    }
  }

  void RestoreWindowSize() {
    // Invalid or absent settings use the default size without blocking startup.
    DWORD size[] = {kMinWindowWidth, kMinWindowHeight};
    DWORD saved[2] = {}, bytes = sizeof saved;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\FontLoaderSub", L"WinuiWindowSize",
            RRF_RT_REG_BINARY, nullptr, saved, &bytes) == ERROR_SUCCESS &&
        bytes == sizeof saved && saved[0] >= 240 && saved[0] <= 16384 &&
        saved[1] >= 240 && saved[1] <= 16384) {
      size[0] = saved[0];
      size[1] = saved[1];
    }

    // Clamp the restored size and position to the current monitor's work area.
    MONITORINFO monitor = {sizeof monitor};
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor))
      return;
    const RECT work = monitor.rcWork;
    const int max_width = static_cast<int>(work.right - work.left);
    const int max_height = static_cast<int>(work.bottom - work.top);
    const int min_width = (std::min)(MulDiv(kMinWindowWidth, WindowDpi(), 96), max_width);
    const int min_height = (std::min)(MulDiv(kMinWindowHeight, WindowDpi(), 96), max_height);
    const int width = (std::clamp)(MulDiv(size[0], WindowDpi(), 96), min_width, max_width);
    const int height = (std::clamp)(MulDiv(size[1], WindowDpi(), 96), min_height, max_height);

    RECT rect = {};
    GetWindowRect(hwnd_, &rect);
    const int x = (std::clamp)(static_cast<int>(rect.left), static_cast<int>(work.left),
        static_cast<int>(work.right) - width);
    const int y = (std::clamp)(static_cast<int>(rect.top), static_cast<int>(work.top),
        static_cast<int>(work.bottom) - height);
    SetWindowPos(hwnd_, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void InitializeMetrics() {
    // Counts use native controls and explicit accessible names in each locale.
    const UINT ids[] = {IDS_STAT_LOADED, IDS_STAT_FAILED, IDS_STAT_UNMATCHED,
        IDS_STAT_FILES, IDS_STAT_FACES, IDS_STAT_SUBS};
    for (size_t i = 0; i < counts_.size(); ++i) {
      counts_[i] = root_.FindName(L"Stat" + to_hstring(i)).as<TextBlock>();
      labels_[i] = Resource(ids[i]);
      root_.FindName(L"Label" + to_hstring(i)).as<TextBlock>().Text(labels_[i]);
      Automation::AutomationProperties::SetName(counts_[i], labels_[i] + L": 0");
    }

    // Collection and loading statistics reflow independently on narrow windows.
    for (const auto *name : {L"CollectionMetrics", L"Metrics"}) {
      auto metrics = root_.FindName(name).as<Grid>();
      metrics.SizeChanged([](Windows::Foundation::IInspectable const &sender,
          SizeChangedEventArgs const &args) {
        auto grid = sender.as<Grid>();
        const uint32_t columns = args.NewSize().Width < 360 ? 1u : 3u;
        // Empty fallback rows must not add vertical gaps in the three-column layout.
        grid.RowSpacing(columns == 1 ? 16 : 0);
        for (uint32_t i = 0; i < grid.Children().Size(); ++i) {
          grid.ColumnDefinitions().GetAt(i).Width({i < columns ? 1.0 : 0.0, GridUnitType::Star});
          Grid::SetColumn(grid.Children().GetAt(i).as<FrameworkElement>(), static_cast<int>(i % columns));
          Grid::SetRow(grid.Children().GetAt(i).as<FrameworkElement>(), static_cast<int>(i / columns));
        }
      });
    }

    // Explicit viewport-based width avoids ScrollViewer's unconstrained measurement.
    auto scroll = root_.FindName(L"PageScroll").as<ScrollViewer>();
    scroll.SizeChanged([this](auto &&, SizeChangedEventArgs const &args) {
      const double padding = args.NewSize().Width < 520 ? 16 : 32;
      auto content = root_.FindName(L"PageContent").as<StackPanel>();
      content.Width((std::clamp)(args.NewSize().Width - padding * 2, 0.0, 960.0));
      content.Margin({padding, 16, padding, 24});
    });
  }

  hstring Resource(UINT id) const {
    PCWSTR text = nullptr;
    const int length = LoadStringW(initial_.instance, id, reinterpret_cast<PWSTR>(&text), 0);
    return length ? hstring(text, length) : hstring();
  }

  fire_and_forget DropFiles(DragEventArgs args) {
    // A deferral keeps the shell data alive while storage items are resolved.
    auto lifetime = get_strong();
    auto deferral = args.GetDeferral();
    try {
      std::vector<std::wstring> paths;
      const auto items = co_await args.DataView().GetStorageItemsAsync();
      for (const auto &item : items)
        paths.emplace_back(item.Path().c_str());
      if (!busy_ && !dialogs_running_)
        load_paths_(std::move(paths));
    } catch (const hresult_error &error) {
      (void)error;
      SPDLOG_WARN("Could not read dropped subtitles");
    }
    deferral.Complete();
  }

  void UpdateCaptionColors() {
    // Mica and theme resources handle the content; caption buttons need explicit colors.
    if (!window_.ExtendsContentIntoTitleBar())
      return;
    const bool dark = root_.ActualTheme() == ElementTheme::Dark;
    auto title = window_.AppWindow().TitleBar();
    title.ButtonBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
    title.ButtonInactiveBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
    title.ButtonForegroundColor(dark ? Windows::UI::Color{255, 255, 255, 255}
                                    : Windows::UI::Color{255, 0, 0, 0});
    title.ButtonInactiveForegroundColor(Windows::UI::Color{255, 128, 128, 128});
  }

  void RenderDetailPage() {
    // Bound layout work even when a font collection produces millions of characters.
    const size_t start = detail_offsets_.back();
    size_t end = start;
    const size_t limit = (std::min)(dialog_details_.size(), start + 8192);
    for (int lines = 0; lines < 80 && end < limit; ++lines) {
      const size_t newline = dialog_details_.find(L'\n', end);
      end = newline == std::wstring::npos ? limit : (std::min)(newline + 1, limit);
    }
    if (end < dialog_details_.size() && end > start &&
        dialog_details_[end - 1] >= 0xD800 && dialog_details_[end - 1] <= 0xDBFF)
      --end;

    detail_next_ = end;
    detail_page_.Text(dialog_details_.substr(start, end - start));
    detail_previous_.IsEnabled(detail_offsets_.size() > 1);
    detail_forward_.IsEnabled(end < dialog_details_.size());
    detail_number_.Text(to_hstring(detail_offsets_.size()));
  }

  void SetDialogContent(const AppUiPage &page) {
    // Copy text into controls so callers can release their temporary strings.
    dialog_.Title(box_value(page.heading.empty() ? page.title : page.heading));
    StackPanel content;
    content.Spacing(16);
    TextBlock message;
    std::wstring body = page.body;
    for (const auto *tag : {L"<A>", L"</A>"}) {
      for (size_t pos; (pos = body.find(tag)) != std::wstring::npos; )
        body.erase(pos, wcslen(tag));
    }

    // Help exposes its shortcut action as a button instead of HTML-like markup.
    message.Text(body);
    message.TextWrapping(TextWrapping::Wrap);
    message.IsTextSelectionEnabled(true);
    content.Children().Append(message);
    if (!page.details.empty()) {
      // Keep the full result, but only lay out one bounded page at a time.
      dialog_details_ = page.details;
      detail_offsets_ = {0};
      detail_page_ = TextBlock();
      detail_page_.TextWrapping(TextWrapping::Wrap);
      detail_page_.IsTextSelectionEnabled(true);
      content.Children().Append(detail_page_);

      StackPanel navigation;
      navigation.Orientation(Orientation::Horizontal);
      navigation.Spacing(12);
      detail_previous_ = Button();
      detail_forward_ = Button();
      detail_number_ = TextBlock();
      detail_number_.VerticalAlignment(VerticalAlignment::Center);
      detail_previous_.Content(box_value(Resource(IDS_PREVIOUS_PAGE)));
      detail_forward_.Content(box_value(Resource(IDS_NEXT_PAGE)));

      // Page changes only update the text block, retaining dialog focus and actions.
      detail_previous_.Click([this](auto &&, auto &&) {
        if (detail_offsets_.size() > 1)
          detail_offsets_.pop_back();
        RenderDetailPage();
      });
      detail_forward_.Click([this](auto &&, auto &&) {
        if (detail_next_ < dialog_details_.size())
          detail_offsets_.push_back(detail_next_);
        RenderDetailPage();
      });

      navigation.Children().Append(detail_previous_);
      navigation.Children().Append(detail_number_);
      navigation.Children().Append(detail_forward_);
      RenderDetailPage();
      if (detail_next_ < dialog_details_.size())
        content.Children().Append(navigation);
    }

    // Shortcut commands stay inside the dialog and refresh their labels in place.
    if (page.command_links) {
      for (const auto &action : page.buttons) {
        if (action.id == IDCLOSE || action.id == IDCANCEL)
          continue;
        Button button;
        button.Content(box_value(ButtonLabel(action.text)));
        button.HorizontalAlignment(HorizontalAlignment::Stretch);
        button.Click([this, id = action.id](auto &&, auto &&) {
          dialog_notify_(hwnd_, TDN_BUTTON_CLICKED, id, 0);
        });
        content.Children().Append(button);
      }
    }

    // Errors from shortcut operations remain visible without opening another window.
    if (!page.footer.empty()) {
      TextBlock footer;
      footer.Text(page.footer);
      footer.TextWrapping(TextWrapping::Wrap);
      content.Children().Append(footer);
    }
    ScrollViewer scroll;
    scroll.MaxHeight(360);
    scroll.Content(content);
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    dialog_.Content(scroll);
  }

  fire_and_forget DrainDialogs() {
    // Retain the app until the asynchronous dialog queue finishes.
    auto lifetime = get_strong();
    while (!dialogs_.empty()) {
      auto request = std::move(dialogs_.front());
      dialogs_.pop_front();
      int primary = 0, close = IDCLOSE;
      auto result = ContentDialogResult::None;
      dialog_notify_ = request.notify;

      try {
        dialog_ = ContentDialog();
        dialog_.XamlRoot(root_.XamlRoot());
        dialog_.RequestedTheme(root_.ActualTheme());
        SetDialogContent(request.page);
        for (const auto &action : request.page.buttons) {
          if (action.id == IDCLOSE || action.id == IDCANCEL) {
            close = action.id;
            dialog_.CloseButtonText(ButtonLabel(action.text));
          } else if (!request.page.command_links && !primary) {
            primary = action.id;
            dialog_.PrimaryButtonText(ButtonLabel(action.text));
          }
        }

        // Missing-font confirmation defaults to cancellation, never automatic loading.
        dialog_.DefaultButton(request.page.default_button == primary && primary
            ? ContentDialogButton::Primary : ContentDialogButton::Close);
        result = co_await dialog_.ShowAsync();
      } catch (const hresult_error &error) {
        (void)error;
        SPDLOG_WARN("ContentDialog failed: {:#x}",
            static_cast<unsigned>(error.code().value));
      }

      // Clear the active dialog before callbacks may navigate or enqueue another one.
      dialog_ = nullptr;
      dialog_notify_ = {};
      dialog_details_.clear();
      detail_offsets_.clear();
      if (request.notify)
        request.notify(hwnd_, TDN_BUTTON_CLICKED,
            result == ContentDialogResult::Primary ? primary : close, 0);
      if (request.completed)
        request.completed();
    }
    dialogs_running_ = false;
  }

  void Build() {
    // Theme resources provide Mica-compatible cards and high-contrast colors.
    root_ = XamlReader::Load(LR"(
<Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Background="Transparent">
  <Grid.Resources>
    <Style x:Key="StatCard" TargetType="Border">
      <Setter Property="Padding" Value="12"/>
      <Setter Property="CornerRadius" Value="8"/>
      <Setter Property="BorderThickness" Value="1"/>
      <Setter Property="Background" Value="{ThemeResource CardBackgroundFillColorDefaultBrush}"/>
      <Setter Property="BorderBrush" Value="{ThemeResource CardStrokeColorDefaultBrush}"/>
    </Style>
  </Grid.Resources>
  <!-- The viewport constrains the page width; cards never measure beyond it. -->
  <Grid.RowDefinitions>
    <RowDefinition Height="Auto"/>
    <RowDefinition Height="*"/>
    <RowDefinition Height="Auto"/>
  </Grid.RowDefinitions>
  <!-- Keep the caption area draggable without overlapping system buttons. -->
  <Grid x:Name="TitleBar" Height="48" Background="Transparent" Padding="20,0,160,0">
    <StackPanel Orientation="Horizontal" Spacing="12" VerticalAlignment="Center">
      <FontIcon Glyph="&#xE8D2;" FontSize="16"/>
      <TextBlock x:Name="Caption" FontSize="12" VerticalAlignment="Center"
                 TextTrimming="CharacterEllipsis"/>
    </StackPanel>
  </Grid>
  <!-- Collection totals precede the drop area and loading results. -->
  <ScrollViewer x:Name="PageScroll" Grid.Row="1" HorizontalScrollBarVisibility="Disabled"
                HorizontalScrollMode="Disabled" HorizontalContentAlignment="Stretch">
    <StackPanel x:Name="PageContent" Margin="32,16,32,24" Spacing="16" Width="0"
                HorizontalAlignment="Center">
      <Grid x:Name="CollectionMetrics" ColumnSpacing="16" RowSpacing="16">
        <Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition/><ColumnDefinition/></Grid.ColumnDefinitions>
        <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
        <Border Style="{StaticResource StatCard}"><StackPanel Spacing="4">
          <TextBlock x:Name="Stat3" Text="0" FontSize="28" FontWeight="SemiBold"/>
          <TextBlock x:Name="Label3" TextWrapping="Wrap"/></StackPanel></Border>
        <!-- Each statistic retains a label when columns reflow. -->
        <Border Grid.Column="1" Style="{StaticResource StatCard}"><StackPanel Spacing="4">
          <TextBlock x:Name="Stat4" Text="0" FontSize="28" FontWeight="SemiBold"/>
          <TextBlock x:Name="Label4" TextWrapping="Wrap"/></StackPanel></Border>
        <Border Grid.Column="2" Style="{StaticResource StatCard}"><StackPanel Spacing="4">
          <TextBlock x:Name="Stat5" Text="0" FontSize="28" FontWeight="SemiBold"/>
          <TextBlock x:Name="Label5" TextWrapping="Wrap"/></StackPanel></Border>
      </Grid>
      <!-- A full-size drop target remains a native button for keyboard activation. -->
      <Button x:Name="Add" HorizontalAlignment="Stretch" HorizontalContentAlignment="Stretch"
              Padding="0" BorderThickness="0" CornerRadius="8" UseSystemFocusVisuals="True"
              Background="{ThemeResource CardBackgroundFillColorDefaultBrush}">
        <Grid MinHeight="140">
          <Rectangle Stroke="{ThemeResource ControlStrokeColorDefaultBrush}" StrokeThickness="1"
                     StrokeDashArray="4,3" RadiusX="8" RadiusY="8" IsHitTestVisible="False"/>
          <Rectangle x:Name="DropHighlight" Fill="{ThemeResource AccentFillColorTertiaryBrush}"
                     Opacity="0" RadiusX="8" RadiusY="8" IsHitTestVisible="False"/>
          <!-- Guidance is centered inside the card, with no separate upload button. -->
          <StackPanel Margin="16" Spacing="8" VerticalAlignment="Center">
            <FontIcon Glyph="&#xE898;" FontSize="24"/>
            <TextBlock x:Name="DropTitle" FontSize="18" FontWeight="SemiBold"
                       TextAlignment="Center" TextWrapping="Wrap"/>
            <TextBlock x:Name="DropHint" TextAlignment="Center" TextWrapping="Wrap"
                       Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
          </StackPanel>
        </Grid>
      </Button>
      <!-- Loading overview is placed directly on Mica, without an enclosing card. -->
      <StackPanel Spacing="16">
        <Grid ColumnSpacing="12">
          <Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition Width="Auto"/></Grid.ColumnDefinitions>
          <TextBlock x:Name="Overview" Style="{StaticResource SubtitleTextBlockStyle}"
                     AutomationProperties.HeadingLevel="Level2" VerticalAlignment="Center"/>
          <DropDownButton x:Name="Tools" Grid.Column="1" MinHeight="32"/>
        </Grid>
        <!-- The status and cancel command remain available while loading. -->
        <StackPanel Orientation="Horizontal" Spacing="8">
          <FontIcon x:Name="StatusIcon" FontSize="16"/>
          <TextBlock x:Name="Heading" AutomationProperties.LiveSetting="Polite" VerticalAlignment="Center"/>
          <StackPanel x:Name="Buttons" Orientation="Horizontal" Spacing="8"/>
        </StackPanel>
        <ProgressBar x:Name="Progress" AutomationProperties.Name="FontLoaderSub"/>
        <!-- Loading counts form a second, independent group of three. -->
        <Grid x:Name="Metrics" ColumnSpacing="16" RowSpacing="16">
          <Grid.ColumnDefinitions><ColumnDefinition/><ColumnDefinition/><ColumnDefinition/></Grid.ColumnDefinitions>
          <Grid.RowDefinitions><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/><RowDefinition Height="Auto"/></Grid.RowDefinitions>
          <Border Style="{StaticResource StatCard}"><StackPanel Spacing="4">
          <TextBlock x:Name="Stat0" Text="0" FontSize="24" FontWeight="SemiBold"/>
            <TextBlock x:Name="Label0" TextWrapping="Wrap"/></StackPanel></Border>
          <!-- Failure and missing counts use text labels as well as numeric values. -->
          <Border Grid.Column="1" Style="{StaticResource StatCard}"><StackPanel Spacing="4">
          <TextBlock x:Name="Stat1" Text="0" FontSize="24" FontWeight="SemiBold"/>
            <TextBlock x:Name="Label1" TextWrapping="Wrap"/></StackPanel></Border>
          <Border Grid.Column="2" Style="{StaticResource StatCard}"><StackPanel Spacing="4">
          <TextBlock x:Name="Stat2" Text="0" FontSize="24" FontWeight="SemiBold"/>
            <TextBlock x:Name="Label2" TextWrapping="Wrap"/></StackPanel></Border>
        </Grid>
      </StackPanel>
      <!-- Long logs stay selectable without expanding the entire window. -->
      <Expander x:Name="DetailsExpander" HorizontalAlignment="Stretch"
                HorizontalContentAlignment="Stretch">
        <ScrollViewer MaxHeight="320" HorizontalScrollBarVisibility="Disabled">
          <TextBlock x:Name="Details" TextWrapping="Wrap" IsTextSelectionEnabled="True"/>
        </ScrollViewer>
      </Expander>
    </StackPanel>
  </ScrollViewer>
  <!-- The system caption buttons own minimize and close; the footer is informational. -->
  <StackPanel Grid.Row="2" Margin="32,12,32,20" Spacing="12">
    <TextBlock x:Name="KeepOpen" TextWrapping="Wrap" FontSize="12"
               Foreground="{ThemeResource TextFillColorSecondaryBrush}"/>
    <HyperlinkButton x:Name="Footer" Content="FontLoaderSub &#xB7; GPLv2" FontSize="12" Padding="0"/>
  </StackPanel>
</Grid>)").as<Grid>();

    // Resolve named controls once; subsequent progress updates only replace text.
    titlebar_ = root_.FindName(L"TitleBar").as<Grid>();
    caption_ = root_.FindName(L"Caption").as<TextBlock>();
    heading_ = root_.FindName(L"Heading").as<TextBlock>();
    body_ = TextBlock();
    progress_ = root_.FindName(L"Progress").as<ProgressBar>();
    details_ = root_.FindName(L"Details").as<TextBlock>();
    expander_ = root_.FindName(L"DetailsExpander").as<Expander>();
    buttons_ = root_.FindName(L"Buttons").as<StackPanel>();
    footer_ = root_.FindName(L"Footer").as<HyperlinkButton>();

    // Localize the home page independently of the legacy TaskDialog wording.
    const std::pair<PCWSTR, UINT> text[] = {
        {L"DropTitle", IDS_DROP_TITLE},
        {L"DropHint", IDS_DROP_HINT}, {L"Overview", IDS_OVERVIEW},
        {L"KeepOpen", IDS_KEEP_OPEN}};
    for (const auto &[name, id] : text)
      root_.FindName(name).as<TextBlock>().Text(Resource(id));
    add_ = root_.FindName(L"Add").as<Button>();
    tools_ = root_.FindName(L"Tools").as<DropDownButton>();
    status_icon_ = root_.FindName(L"StatusIcon").as<FontIcon>();

    // The primary action is also reachable without a pointer through Ctrl+O.
    Automation::AutomationProperties::SetName(add_, Resource(IDS_ADD_SUBTITLE));
    tools_.Content(box_value(Resource(IDS_MORE_ACTIONS)));
    add_.Click([this](auto &&, auto &&) { notify_(hwnd_, TDN_BUTTON_CLICKED, ID_BTN_ADD_SUBS, 0); });
    Input::KeyboardAccelerator open;
    open.Key(Windows::System::VirtualKey::O);
    open.Modifiers(Windows::System::VirtualKeyModifiers::Control);
    add_.KeyboardAccelerators().Append(open);
    InitializeMetrics();
    drop_highlight_ = root_.FindName(L"DropHighlight").as<UIElement>();

    // XAML drag-and-drop accepts subtitles and folders using the shared loader.
    root_.AllowDrop(true);
    root_.DragOver([this](auto &&, DragEventArgs const &args) {
      using namespace Windows::ApplicationModel::DataTransfer;
      args.AcceptedOperation(!busy_ && !dialogs_running_ &&
          args.DataView().Contains(StandardDataFormats::StorageItems())
          ? DataPackageOperation::Copy : DataPackageOperation::None);
      drop_highlight_.Opacity(args.AcceptedOperation() == DataPackageOperation::Copy ? 1.0 : 0.0);
      args.Handled(true);
    });
    root_.Drop([this](auto &&, DragEventArgs const &args) {
      drop_highlight_.Opacity(0);
      if (!busy_ && !dialogs_running_)
        DropFiles(args);
    });
    root_.DragLeave([this](auto &&, auto &&) { drop_highlight_.Opacity(0); });

    // Mica supplies its own solid fallback on systems without the material effect.
    window_.Content(root_);
    window_.SystemBackdrop(Media::MicaBackdrop());
    if (Microsoft::UI::Windowing::AppWindowTitleBar::IsCustomizationSupported()) {
      window_.ExtendsContentIntoTitleBar(true);
      window_.SetTitleBar(titlebar_);
      UpdateCaptionColors();
    } else {
      titlebar_.Visibility(Visibility::Collapsed);
    }

    // Match caption contrast to theme changes and show inactive-window state.
    root_.ActualThemeChanged([this](auto &&, auto &&) {
      UpdateCaptionColors();
      if (dialog_)
        dialog_.RequestedTheme(root_.ActualTheme());
    });
    window_.Activated([this](auto &&, WindowActivatedEventArgs const &args) {
      caption_.Opacity(args.WindowActivationState() == WindowActivationState::Deactivated ? 0.6 : 1.0);
    });
    footer_.Click([this](auto &&, auto &&) {
      notify_(hwnd_, TDN_HYPERLINK_CLICKED, 0, 0);
    });

    // Let ContentDialog own Escape while open; otherwise cancel the current work.
    root_.KeyDown([this](auto &&, Input::KeyRoutedEventArgs const &args) {
      if (!dialogs_running_ && args.Key() == Windows::System::VirtualKey::Escape) {
        args.Handled(true);
        if (notify_(hwnd_, TDN_BUTTON_CLICKED, IDCANCEL, 0) == S_OK)
          window_.Close();
      }
    });
    RestoreWindowSize();
  }

  // WinRT objects are constructed only after the runtime has been initialized.
  AppUiPage initial_;
  AppUiNotify notify_;
  AppUiLoadPaths load_paths_;
  XamlControlsXamlMetaDataProvider provider_;
  Window window_{nullptr};
  DispatcherTimer timer_{nullptr};
  TextBlock heading_{nullptr}, body_{nullptr}, details_{nullptr};
  ProgressBar progress_{nullptr};
  Expander expander_{nullptr};
  StackPanel buttons_{nullptr};
  HyperlinkButton footer_{nullptr};
  Grid root_{nullptr}, titlebar_{nullptr};
  TextBlock caption_{nullptr};
  Button add_{nullptr};
  DropDownButton tools_{nullptr};
  FontIcon status_icon_{nullptr};
  UIElement drop_highlight_{nullptr};
  std::array<TextBlock, 6> counts_{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  std::array<hstring, 6> labels_;
  ContentDialog dialog_{nullptr};
  std::wstring dialog_details_;
  std::vector<size_t> detail_offsets_;
  size_t detail_next_ = 0;
  TextBlock detail_page_{nullptr}, detail_number_{nullptr};
  Button detail_previous_{nullptr}, detail_forward_{nullptr};
  AppUiNotify dialog_notify_;
  std::deque<DialogRequest> dialogs_;
  bool dialogs_running_ = false;
  HWND hwnd_ = nullptr;
  bool busy_ = false, started_ = false;
};
} // namespace

bool WinuiRun(const AppUiPage &page, const AppUiNotify &notify, AppUiLoadPaths load_paths) {
  // No WinRT call is made on old Windows or when the runtime is unavailable.
  WinuiRuntime runtime;
  if (!runtime.Initialize())
    return false;
  bool started = false;
  bool apartment = false;
  com_ptr<App> app;

  try {
    init_apartment(apartment_type::single_threaded);
    apartment = true;
    Application::Start([&](auto &&) { app = make_self<App>(page, notify, std::move(load_paths)); });
    started = app && app->Started();
  } catch (const hresult_error &error) {
    (void)error;
    started = app && app->Started();
    SPDLOG_WARN("WinUI initialization failed: {:#x}",
        static_cast<unsigned>(error.code().value));
  }

  // Release all activation factories before removing the runtime dependency.
  active = nullptr;
  app = nullptr;
  clear_factory_cache();
  if (apartment)
    uninit_apartment();
  return started;
}

void WinuiNavigate(const AppUiPage &page) {
  active->Navigate(page);
}

void WinuiSetText(TASKDIALOG_ELEMENTS element, PCWSTR text) {
  active->SetText(element, text);
}

void WinuiShowDialog(AppUiPage page, AppUiNotify notify, std::function<void()> completed) {
  active->ShowDialog(std::move(page), std::move(notify), std::move(completed));
}

void WinuiNavigateDialog(const AppUiPage &page) {
  active->UpdateDialog(page);
}

bool WinuiHasDialog() {
  return active && active->HasDialog();
}

void WinuiSetStats(const AppUiStats &stats) {
  active->SetStats(stats);
}

void WinuiSetMenu(HMENU menu) {
  active->SetMenu(menu);
}
