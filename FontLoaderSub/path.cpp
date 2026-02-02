#include "path.h"

#include <string>

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

static int
WalkDirDfs(const std::wstring &dir, FL_FileWalkCb callback, void *arg) {
  std::wstring search = dir;
  if (!search.empty() && search.back() != L'\\')
    search += L'\\';
  search += L"*";

  WIN32_FIND_DATA fd;
  HANDLE find_handle = FindFirstFile(search.c_str(), &fd);
  if (find_handle == INVALID_HANDLE_VALUE) {
    return FL_OK;
  }

  int r = FL_OK;
  do {
    if ((fd.cFileName[0] == L'.' && fd.cFileName[1] == 0) ||
        (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.' &&
         fd.cFileName[2] == 0)) {
      continue;
    }

    std::wstring full = dir;
    if (!full.empty() && full.back() != L'\\')
      full += L'\\';
    full += fd.cFileName;

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      r = WalkDirDfs(full, callback, arg);
    } else {
      r = callback(full.c_str(), &fd, arg);
    }
  } while (r == FL_OK && FindNextFile(find_handle, &fd));

  FindClose(find_handle);
  return r;
}

int FlWalkDir(const wchar_t *path, FL_FileWalkCb callback, void *arg) {
  if (path == NULL || callback == NULL)
    return FL_OS_ERROR;
  const DWORD attr = GetFileAttributes(path);
  if (attr == INVALID_FILE_ATTRIBUTES)
    return FL_OK;
  if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    WIN32_FIND_DATA fd;
    HANDLE find_handle = FindFirstFile(path, &fd);
    if (find_handle == INVALID_HANDLE_VALUE)
      return FL_OK;
    const int r = callback(path, &fd, arg);
    FindClose(find_handle);
    return r;
  }
  return WalkDirDfs(path, callback, arg);
}
