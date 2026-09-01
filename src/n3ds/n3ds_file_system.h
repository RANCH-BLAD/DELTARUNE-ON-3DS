#pragma once

#include "../file_system.h"

typedef struct N3DSFileSystemResolvedPathEntry N3DSFileSystemResolvedPathEntry;

typedef struct {
    FileSystem base;
    char* romfsBasePath;
    char* saveBasePath;
    N3DSFileSystemResolvedPathEntry* resolvedPathCache;
} N3DSFileSystem;

N3DSFileSystem* N3DSFileSystem_create(const char* romfsBasePath, const char* saveBasePath);
void N3DSFileSystem_destroy(N3DSFileSystem* fs);
