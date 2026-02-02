#include "util.h"

#include <Windows.h>
#include <Shlwapi.h>
#include <intrin.h>
#include <climits>

#pragma intrinsic(__movsb)
#pragma intrinsic(__stosb)

int FlMemMap(const wchar_t *path, memmap_t *mmap) {
  mmap->map = nullptr;
  mmap->data = nullptr;
  mmap->size = 0;
  HANDLE h;
  do {
    h = CreateFile(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
      break;
    mmap->map = CreateFileMapping(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mmap->map == nullptr)
      break;
    mmap->data = MapViewOfFile(mmap->map, FILE_MAP_READ, 0, 0, 0);
    if (mmap->data == nullptr)
      break;
    DWORD high = 0;
    mmap->size = GetFileSize(h, &high);
    // mmap->size = (high * 0x100000000UL) | (mmap->size);
  } while (0);

  if (mmap->data == nullptr) {
    FlMemUnmap(mmap);
  }
  CloseHandle(h);
  return 0;
}

int FlMemUnmap(memmap_t *mmap) {
  UnmapViewOfFile(mmap->data);
  CloseHandle(mmap->map);
  mmap->map = nullptr;
  mmap->data = nullptr;
  mmap->size = 0;
  return 0;
}

static int FlTestUtf8(const uint8_t *buffer, size_t size) {
  const uint8_t *p, *last;
  int rem = 0;
  for (p = buffer, last = buffer + size; p != last; p++) {
    if (rem) {
      if ((*p & 0xc0) == 0x80) {
        // 10xxxxxx
        --rem;
      } else {
        return 0;
      }
    } else if ((*p & 0x80) == 0) {
      // rem = 0;
    } else if ((*p & 0xe0) == 0xc0) {
      // 110xxxxx
      rem = 1;
    } else if ((*p & 0xf0) == 0xe0) {
      // 1110xxxx
      rem = 2;
    } else if ((*p & 0xf8) == 0xf0) {
      // 11110xxx
      rem = 3;
    } else {
      return 0;
    }
  }
  return rem == 0;
}

static bool FlToInt(size_t value, int *out) {
  if (out == nullptr)
    return false;
  if (value > (size_t)INT_MAX) {
    *out = 0;
    return false;
  }
  *out = (int)value;
  return true;
}

static wchar_t *FlTextTryDecodeWide(
    UINT codepage,
    const uint8_t *mstr,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  wchar_t *buf = nullptr;
  int ok = 0;
  int in_len = 0;
  do {
    if (!FlToInt(bytes, &in_len))
      break;
    const int r = MultiByteToWideChar(
        codepage, 0, (const char *)mstr, in_len, nullptr, 0);
    *cch = r;
    if (r == 0)
      break;

    buf = (wchar_t *)alloc->alloc(buf, (r + 1) * sizeof buf[0], alloc->arg);
    if (buf == nullptr)
      break;

    const int new_r =
        MultiByteToWideChar(codepage, 0, (const char *)mstr, in_len, buf, r);
    if (new_r == 0 || new_r != r)
      break;
    buf[r] = 0;
    ok = 1;
  } while (0);

  if (!ok) {
    alloc->alloc(buf, 0, alloc->arg);
    buf = nullptr;
  }
  return buf;
}

static wchar_t *FlTextDecodeUtf16(
    int big_endian,
    const uint8_t *mstr,
    size_t bytes,
    size_t *cch,
    allocator_t *alloc) {
  const wchar_t *wstr = (const wchar_t *)mstr;
  wchar_t *buf = nullptr;
  int ok = 0;

  do {
    const size_t r = *cch = bytes / 2;
    buf = (wchar_t *)alloc->alloc(buf, (r + 1) * sizeof buf[0], alloc->arg);
    if (buf == nullptr)
      break;

    for (size_t i = 0; i != r; i++) {
      buf[i] = big_endian ? be16(wstr[i]) : wstr[i];
    }
    buf[r] = 0;
    ok = 1;
  } while (0);

  if (!ok) {
    alloc->alloc(buf, 0, alloc->arg);
    buf = nullptr;
  }
  return buf;
}

static char *FlTextCopyUtf8(
    const uint8_t *mstr,
    size_t bytes,
    size_t *len,
    allocator_t *alloc) {
  char *buf = (char *)alloc->alloc(nullptr, bytes + 1, alloc->arg);
  if (buf == nullptr)
    return nullptr;
  zmemcpy(buf, mstr, bytes);
  buf[bytes] = 0;
  if (len)
    *len = bytes;
  return buf;
}

static char *FlTextFromWide(
    const wchar_t *wstr,
    size_t cch,
    size_t *len,
    allocator_t *alloc) {
  int in_len = 0;
  if (!FlToInt(cch, &in_len))
    return nullptr;

  const int needed = WideCharToMultiByte(
      CP_UTF8, 0, wstr, in_len, nullptr, 0, nullptr, nullptr);
  if (needed <= 0)
    return nullptr;

  char *buf = (char *)alloc->alloc(nullptr, needed + 1, alloc->arg);
  if (buf == nullptr)
    return nullptr;

  const int written = WideCharToMultiByte(
      CP_UTF8, 0, wstr, in_len, buf, needed, nullptr, nullptr);
  if (written <= 0) {
    alloc->alloc(buf, 0, alloc->arg);
    return nullptr;
  }
  buf[written] = 0;
  if (len)
    *len = (size_t)written;
  return buf;
}

char *FlTextDecode(
    const uint8_t *buf,
    size_t bytes,
    size_t *len,
    allocator_t *alloc) {
  char *res = nullptr;
  if (bytes == 0)
    return res;

  // detect BOM
  if (buf[0] == 0xef && buf[1] == 0xbb && buf[2] == 0xbf) {
    res = FlTextCopyUtf8(buf + 3, bytes - 3, len, alloc);
  } else if (buf[0] == 0xff && buf[1] == 0xfe) {
    // UTF-16 LE
    size_t cch = 0;
    wchar_t *wbuf = FlTextDecodeUtf16(0, buf + 2, bytes - 2, &cch, alloc);
    if (wbuf) {
      res = FlTextFromWide(wbuf, cch, len, alloc);
      alloc->alloc(wbuf, 0, alloc->arg);
    }
  } else if (buf[0] == 0xfe && buf[1] == 0xff) {
    // UTF-16 BE
    size_t cch = 0;
    wchar_t *wbuf = FlTextDecodeUtf16(1, buf + 2, bytes - 2, &cch, alloc);
    if (wbuf) {
      res = FlTextFromWide(wbuf, cch, len, alloc);
      alloc->alloc(wbuf, 0, alloc->arg);
    }
  }

  // detect UTF-8
  if (!res && FlTestUtf8(buf, bytes)) {
    res = FlTextCopyUtf8(buf, bytes, len, alloc);
  }
  // final resort
  if (!res) {
    size_t cch = 0;
    wchar_t *wbuf = FlTextTryDecodeWide(CP_ACP, buf, bytes, &cch, alloc);
    if (wbuf) {
      res = FlTextFromWide(wbuf, cch, len, alloc);
      alloc->alloc(wbuf, 0, alloc->arg);
    }
  }
  return res;
}

static int is_digit(wchar_t ch) {
  if (L'0' <= ch && ch <= L'9')
    return ch - L'0';
  else
    return -1;
}

int FlVersionCmp(const wchar_t *a, const wchar_t *b) {
  const wchar_t *ptr_a = a, *ptr_b = b;
  int cmp = 0;

  if (b == nullptr)
    return 1;
  if (a == nullptr)
    return -1;

  while (*ptr_a && *ptr_b && cmp == 0) {
    if (is_digit(*ptr_a) >= 0 && is_digit(*ptr_b) >= 0) {
      // seek to the end of digits
      const wchar_t *start_a = ptr_a, *start_b = ptr_b;
      while (is_digit(*ptr_a) >= 0)
        ptr_a++;
      while (is_digit(*ptr_b) >= 0)
        ptr_b++;
      // compare from right to left
      const wchar_t *dig_a = ptr_a, *dig_b = ptr_b;
      while (dig_a != start_a && dig_b != start_b) {
        dig_a--, dig_b--;
        cmp = *dig_a - *dig_b;
      }
      // leading zero
      while (dig_a != start_a && dig_a[-1] == L'0') {
        dig_a--;
      }
      while (dig_b != start_b && dig_b[-1] == L'0') {
        dig_b--;
      }
      if (dig_a != start_a) {
        cmp = 1;
      } else if (dig_b != start_b) {
        cmp = -1;
      }
    } else if (*ptr_a != *ptr_b) {
      cmp = *ptr_a - *ptr_b;
    } else {
      ptr_a++, ptr_b++;
    }
  }

  if (cmp == 0) {
    if (*ptr_a)
      cmp = 1;
    else if (*ptr_b)
      cmp = -1;
  }

  return cmp;
}

int FlStrCmpIW(const wchar_t *a, const wchar_t *b) {
  return StrCmpIW(a, b);
}

#include <ShellScalingApi.h>

BOOL PerMonitorDpiHack() {
  typedef BOOL(WINAPI * PFN_SetProcessDpiAwarenessContext)(
      DPI_AWARENESS_CONTEXT value);
  typedef BOOL(WINAPI * PFN_SetProcessDPIAware)(VOID);
  typedef HRESULT(WINAPI * PFN_SetProcessDpiAwareness)(PROCESS_DPI_AWARENESS);
  typedef BOOL(WINAPI * PFN_EnablePerMonitorDialogScaling)();
  PFN_SetProcessDpiAwarenessContext pSetProcessDpiAwarenessContext = nullptr;
  PFN_EnablePerMonitorDialogScaling pEnablePerMonitorDialogScaling = nullptr;
  PFN_SetProcessDPIAware pSetProcessDPIAware = nullptr;
  PFN_SetProcessDpiAwareness pSetProcessDpiAwareness = nullptr;
  DWORD result = 0;

  HMODULE user32 = GetModuleHandle(L"USER32");
  if (user32 == nullptr)
    return FALSE;

  pSetProcessDpiAwarenessContext =
      (PFN_SetProcessDpiAwarenessContext)GetProcAddress(
          user32, "SetProcessDpiAwarenessContext");
  // find a private function, available on RS1, attempt 1
  /*
  pEnablePerMonitorDialogScaling =
      (PFN_EnablePerMonitorDialogScaling)GetProcAddress(
          user32, "EnablePerMonitorDialogScaling");
  */
  if (pEnablePerMonitorDialogScaling == nullptr) {
    // attempt 2:
    pEnablePerMonitorDialogScaling =
        (PFN_EnablePerMonitorDialogScaling)GetProcAddress(user32, (LPCSTR)2577);
  }
  pSetProcessDPIAware =
      (PFN_SetProcessDPIAware)GetProcAddress(user32, "SetProcessDPIAware");
  pSetProcessDpiAwareness = (PFN_SetProcessDpiAwareness)GetProcAddress(
      user32, "SetProcessDpiAwarenessInternal");

  if (pSetProcessDpiAwarenessContext) {
    // preferred, official API, available since Win10 Creators
    pSetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  } else if (pSetProcessDpiAwareness) {
    if (pEnablePerMonitorDialogScaling) {
      // enable per-monitor scaling on Win10RS1+
      result = pSetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
      result = pEnablePerMonitorDialogScaling();
    } else {
      result = pSetProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE);
    }
  } else if (pSetProcessDPIAware) {
    result = pSetProcessDPIAware();
  }

  return 0;
}

const TCHAR *ResLoadString(HMODULE hInstance, UINT idText) {
  int res;
  const TCHAR *textptr = nullptr;
  res = LoadString(hInstance, idText, (TCHAR *)&textptr, 0);
  if (textptr == nullptr) {
    // logA("Failed to load res string");
    textptr = L"";  // failback
  }
  return textptr;
}

void *zmemset(void *dest, int ch, size_t count) {
  __stosb((unsigned char *)dest, (unsigned char)ch, count);
  return dest;
}

void *zmemcpy(void *dest, const void *src, size_t count) {
  __movsb((unsigned char *)dest, (const unsigned char *)src, count);
  return dest;
}
