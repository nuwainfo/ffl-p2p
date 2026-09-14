/**
SPDX-License-Identifier: Apache-2.0

FastFileLink CLI - Fast, no-fuss file sharing
Copyright (C) 2025-2026 FastFileLink contributors
*/

#include "Log.h"

#include <stdio.h>

static const char *logLevelName(int level) {
    static const char *const names[] = {
        "VERBOSE",
        "DEBUG",
        "INFO",
        "WARN",
        "ERROR",
        "FATAL",
    };

    if (level < 0 || level >= (int)(sizeof(names) / sizeof(names[0]))) {
        return "LOG";
    }

    return names[level];
}

void writeFFLP2PNativeLog(int level, const char *message) {
    if (!message) {
        return;
    }

    fprintf(stderr, "%s %s\n", logLevelName(level), message);
    fflush(stderr);
}
