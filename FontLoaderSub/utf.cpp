#include "utf.h"

#if __has_include(<simdutf.h>)
#include <simdutf.h>
#elif __has_include(<simdutf/simdutf.h>)
#include <simdutf/simdutf.h>
#else
#include <simdutf.h>
#endif

#include <climits>

static_assert(sizeof(wchar_t) == 2, "UTF-16 wchar_t required on Windows");

bool Utf8ToUtf16(const char *input, size_t input_len, std::wstring *output) {
  if (output == NULL) {
    return false;
  }
  output->clear();
  if (input_len == 0) {
    return true;
  }

  if (!simdutf::validate_utf8(input, input_len)) {
    return false;
  }
  size_t needed = simdutf::utf16_length_from_utf8(input, input_len);
  output->resize(needed);
  size_t written = simdutf::convert_utf8_to_utf16le(
      input, input_len, reinterpret_cast<char16_t *>(&(*output)[0]));
  if (written == 0 && input_len != 0) {
    output->clear();
    return false;
  }
  output->resize(written);
  return true;
}

bool Utf8ToUtf16(absl::string_view input, std::wstring *output) {
  return Utf8ToUtf16(input.data(), input.size(), output);
}

bool Utf8ToUtf16(const char *input, std::wstring *output) {
  if (input == NULL) {
    if (output)
      output->clear();
    return false;
  }
  return Utf8ToUtf16(absl::string_view(input), output);
}

bool Utf16ToUtf8(const wchar_t *input, size_t input_len, std::string *output) {
  if (output == NULL) {
    return false;
  }
  output->clear();
  if (input_len == 0) {
    return true;
  }

  const char16_t *utf16 = reinterpret_cast<const char16_t *>(input);
  if (!simdutf::validate_utf16le(utf16, input_len)) {
    return false;
  }
  size_t needed = simdutf::utf8_length_from_utf16le(utf16, input_len);
  output->resize(needed);
  size_t written =
      simdutf::convert_utf16le_to_utf8(utf16, input_len, &(*output)[0]);
  if (written == 0 && input_len != 0) {
    output->clear();
    return false;
  }
  output->resize(written);
  return true;
}

bool Utf16ToUtf8(std::wstring_view input, std::string *output) {
  return Utf16ToUtf8(input.data(), input.size(), output);
}

bool Utf16ToUtf8(const wchar_t *input, std::string *output) {
  if (input == NULL) {
    if (output)
      output->clear();
    return false;
  }
  return Utf16ToUtf8(std::wstring_view(input), output);
}

std::wstring Utf8ToUtf16(const char *input, size_t input_len) {
  std::wstring output;
  Utf8ToUtf16(input, input_len, &output);
  return output;
}

std::wstring Utf8ToUtf16(absl::string_view input) {
  std::wstring output;
  Utf8ToUtf16(input, &output);
  return output;
}

std::wstring Utf8ToUtf16(const char *input) {
  std::wstring output;
  Utf8ToUtf16(input, &output);
  return output;
}

std::string Utf16ToUtf8(const wchar_t *input, size_t input_len) {
  std::string output;
  Utf16ToUtf8(input, input_len, &output);
  return output;
}

std::string Utf16ToUtf8(std::wstring_view input) {
  std::string output;
  Utf16ToUtf8(input, &output);
  return output;
}

std::string Utf16ToUtf8(const wchar_t *input) {
  std::string output;
  Utf16ToUtf8(input, &output);
  return output;
}
