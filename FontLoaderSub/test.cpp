#include "ass_parser.h"

static int null_cb(const char *font, size_t cch, void *arg) {
  return 0;
}

int test_main() {
  const char data[] = "\xEF\xBB\xBF[Events]\r\n";
  ass_process_data(data, sizeof data - 1, null_cb, nullptr);
  return 1;
}
