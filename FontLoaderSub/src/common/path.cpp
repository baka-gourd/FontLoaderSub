#include "common/path.h"

#include <string>
#include <vector>

int FlResolvePath(const wchar_t *path, std::wstring *out) {
  if (out == nullptr)
    return FL_OUT_OF_MEMORY;

  int r = FL_OK;
  HANDLE handle = INVALID_HANDLE_VALUE;

  do {
    handle = CreateFile(
        path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      r = FL_OS_ERROR;
      break;
    }

    const DWORD name_flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD size = GetFinalPathNameByHandle(handle, nullptr, 0, name_flags);
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
  if (path == nullptr)
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
  if (path == nullptr || callback == nullptr)
    return FL_OS_ERROR;
  // Use Win32 enumeration so the static C++ filesystem runtime is not required.
  WIN32_FILE_ATTRIBUTE_DATA attributes = {};
  if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
    return FL_OK;
  WIN32_FIND_DATAW fd = {};
  if (!(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    fd.dwFileAttributes = attributes.dwFileAttributes;
    fd.nFileSizeHigh = attributes.nFileSizeHigh;
    fd.nFileSizeLow = attributes.nFileSizeLow;
    return callback(path, &fd, arg);
  }

  // Do not recurse into directory links; inaccessible directories are skipped.
  std::vector<std::wstring> directories{path};
  while (!directories.empty()) {
    std::wstring directory = std::move(directories.back());
    directories.pop_back();
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/')
      directory += L'\\';
    HANDLE find = FindFirstFileW((directory + L'*').c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE)
      continue;

    // Close the search handle even when the callback stops enumeration.
    int result = FL_OK;
    do {
      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
        continue;
      const std::wstring entry = directory + fd.cFileName;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
          directories.push_back(entry);
      } else {
        result = callback(entry.c_str(), &fd, arg);
        if (result != FL_OK)
          break;
      }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    if (result != FL_OK)
      return result;
  }
  return FL_OK;
}
