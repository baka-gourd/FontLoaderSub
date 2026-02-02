#pragma once

#include <string>
#include <cstddef>
#include <string>

bool Utf8ToUtf16(const char *input, size_t input_len, std::wstring *output);

bool Utf16ToUtf8(const wchar_t *input, size_t input_len, std::string *output);

std::wstring Utf8ToUtf16(const char *input, size_t input_len);

std::string Utf16ToUtf8(const wchar_t *input, size_t input_len);

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
