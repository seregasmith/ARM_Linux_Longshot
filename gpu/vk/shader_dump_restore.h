// Install-local selection shared by the Linux host and Vulkan backend.
#pragma once
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char dir[4096];
    char uuid[33], stamp[17], format[5], mesa[64];
    unsigned vendor, device, driver, version, store;
} LongshotShaderDump;

// Missing selection is normal. An unreadable or malformed selection is an error.
static inline int longshot_shader_dump_read(const char* game, LongshotShaderDump* dump) {
    char path[4096], selection[48], magic[32];
    if (snprintf(path, sizeof path, "%s/cache/shader-dump.active", game) >= (int)sizeof path) return -1;
    FILE* f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;
    const int read = fgets(selection, sizeof selection, f) != NULL && fgetc(f) == EOF;
    fclose(f);
    if (!read || strlen(selection) != 45 || strncmp(selection, "shader-dump-", 12) || selection[44] != '\n' ||
        strspn(selection + 12, "0123456789abcdef") != 32) return -1;
    selection[44] = '\0';
    if (snprintf(dump->dir, sizeof dump->dir, "%s/cache/%s", game, selection) >= (int)sizeof dump->dir ||
        snprintf(path, sizeof path, "%s/shader-dump.runtime", dump->dir) >= (int)sizeof path) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    const int fields = fscanf(f, "%31s %8x %8x %8x %8x %32[0-9a-f] %16[0-9a-f] %4s %8x %63s", magic, &dump->vendor,
                              &dump->device, &dump->driver, &dump->version, dump->uuid, dump->stamp, dump->format,
                              &dump->store, dump->mesa);
    fclose(f);
    if (fields != 10 || strcmp(magic, "longshot-shader-dump-v1") || strlen(dump->uuid) != 32 || strlen(dump->stamp) != 16 ||
        (strcmp(dump->format, "HSP1") && strcmp(dump->format, "HSP2"))) return -1;
    return 1;
}
