#pragma once

#include <string>

#include "util.h"

int FlResolvePath(const wchar_t *path, std::wstring *out);

size_t FlPathParent(std::wstring *path);

typedef int (
    *FL_FileWalkCb)(const wchar_t *path, WIN32_FIND_DATA *data, void *arg);

int FlWalkDir(const wchar_t *path, FL_FileWalkCb callback, void *arg);
