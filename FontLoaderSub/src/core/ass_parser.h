#pragma once

#include <stddef.h>

typedef int (*ASS_FontCallback)(const char *font, size_t cch, void *arg);

void ass_process_data(
    const char *data,
    size_t cch,
    ASS_FontCallback cb,
    void *arg);
