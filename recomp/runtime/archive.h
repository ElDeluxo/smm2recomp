// Unpacking downloaded archives (zip, rar 4 / 5, 7z...) with the system's libarchive, loaded at run time (archive.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>

bool archive_available(void);
// archive_path into dest (made if missing). progress (may be NULL): files written so far. why: the reason on failure.
bool archive_extract(const char *archive_path, const char *dest, void (*progress)(int files, void *user), void *user,
                     char *why, size_t why_cap);
