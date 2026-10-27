#pragma once

#include <string>
#include <string_view>

#include "absl/strings/string_view.h"

bool Utf8ToUtf16(const char *input, size_t input_len, std::wstring *output);

bool Utf8ToUtf16(const char *input, std::wstring *output);

bool Utf8ToUtf16(absl::string_view input, std::wstring *output);

bool Utf16ToUtf8(const wchar_t *input, size_t input_len, std::string *output);

bool Utf16ToUtf8(const wchar_t *input, std::string *output);

bool Utf16ToUtf8(std::wstring_view input, std::string *output);

std::wstring Utf8ToUtf16(const char *input, size_t input_len);

std::wstring Utf8ToUtf16(const char *input);

std::wstring Utf8ToUtf16(absl::string_view input);

std::string Utf16ToUtf8(const wchar_t *input, size_t input_len);

std::string Utf16ToUtf8(const wchar_t *input);

std::string Utf16ToUtf8(std::wstring_view input);

inline bool Utf8ToUtf16(const std::string &input, std::wstring *output) {
  return Utf8ToUtf16(input.data(), input.size(), output);
}

inline bool Utf16ToUtf8(const std::wstring &input, std::string *output) {
  return Utf16ToUtf8(input.data(), input.size(), output);
}

inline std::wstring Utf8ToUtf16(const std::string &input) {
  return Utf8ToUtf16(input.data(), input.size());
}

inline std::string Utf16ToUtf8(const std::wstring &input) {
  return Utf16ToUtf8(input.data(), input.size());
}
