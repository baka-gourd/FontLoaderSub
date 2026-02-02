#include "path.h"
#include "path.h"

#include <filesystem>
#include <system_error>
#include <string>

namespace fs = std::filesystem;

int FlResolvePath(const wchar_t *path, std::wstring *out) {
  if (out == NULL)
    return FL_OUT_OF_MEMORY;

  int r = FL_OK;
  HANDLE handle = INVALID_HANDLE_VALUE;

  do {
    handle = CreateFile(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
      r = FL_OS_ERROR;
      break;
    }

    const DWORD name_flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD size = GetFinalPathNameByHandle(handle, NULL, 0, name_flags);
    if (size == 0) {
      r = FL_OS_ERROR;
      break;
    }

    std::wstring buffer;
    buffer.resize(size);
    const DWORD cch =
        GetFinalPathNameByHandle(handle, &buffer[0], size, name_flags);
    if (cch == 0 || cch >= size) {
      r = FL_OS_ERROR;
      break;
    }
    buffer.resize(cch);
    *out = std::move(buffer);
  } while (0);

  if (handle != INVALID_HANDLE_VALUE)
    CloseHandle(handle);
  return r;
}

size_t FlPathParent(std::wstring *path) {
  if (path == NULL)
    return 0;
  const size_t pos = path->find_last_of(L'\\');
  if (pos == std::wstring::npos) {
    path->clear();
    return 0;
  }
  path->resize(pos);
  return pos;
}

int FlWalkDir(const wchar_t *path, FL_FileWalkCb callback, void *arg) {
  if (path == NULL || callback == NULL)
    return FL_OS_ERROR;
  std::error_code ec;
  const fs::path in_path(path);
  if (!fs::exists(in_path, ec))
    return FL_OK;

  WIN32_FIND_DATAW fd = {};
  WIN32_FILE_ATTRIBUTE_DATA fad = {};

  if (fs::is_regular_file(in_path, ec)) {
    if (GetFileAttributesExW(in_path.c_str(), GetFileExInfoStandard, &fad)) {
      fd.dwFileAttributes = fad.dwFileAttributes;
      fd.nFileSizeHigh = fad.nFileSizeHigh;
      fd.nFileSizeLow = fad.nFileSizeLow;
      return callback(in_path.c_str(), &fd, arg);
    }
    return FL_OK;
  }

  if (!fs::is_directory(in_path, ec))
    return FL_OK;

  const auto options = fs::directory_options::skip_permission_denied;
  fs::recursive_directory_iterator it(in_path, options, ec);
  fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const fs::path entry_path = it->path();
    if (it->is_directory(ec))
      continue;
    if (ec) {
      ec.clear();
      continue;
    }
    if (GetFileAttributesExW(entry_path.c_str(), GetFileExInfoStandard, &fad)) {
      fd.dwFileAttributes = fad.dwFileAttributes;
      fd.nFileSizeHigh = fad.nFileSizeHigh;
      fd.nFileSizeLow = fad.nFileSizeLow;
      const int r = callback(entry_path.c_str(), &fd, arg);
      if (r != FL_OK)
        return r;
    }
  }
  return FL_OK;
}
