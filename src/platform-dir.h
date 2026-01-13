#ifndef APPLING_PLATFORM_DIR_H
#define APPLING_PLATFORM_DIR_H

#include <ctype.h>
#include <path.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#ifdef _WIN32
#include <windows.h>
#endif

static inline int
appling_platform__is_msix_redirected_local(const char *path) {
  if (path == NULL) return 0;
#ifdef _WIN32
  char lower[1024];
  size_t len = strlen(path);
  if (len >= sizeof(lower)) len = sizeof(lower) - 1;
  for (size_t i = 0; i < len; i++) {
    lower[i] = (char) tolower((unsigned char) path[i]);
  }
  lower[len] = '\0';

  return strstr(lower, "\\packages\\") != NULL ||
         strstr(lower, "\\localcache\\local") != NULL;
#else
  return 0;
#endif
}


static inline int
appling_platform__resolve_dir(appling_path_t out, size_t *out_len) {
  const char *local = getenv("LOCALAPPDATA");
#ifdef _WIN32
  int msix_redirected = local && local[0] && appling_platform__is_msix_redirected_local(local);
  if (local && local[0] && !msix_redirected) {
    if (out && out_len && *out_len > 0) {
      strncpy(out, local, *out_len - 1);
      out[*out_len - 1] = '\0';
      *out_len = strlen(out);
    }
    return 0;
  }
  if (msix_redirected) {
    const char *program = getenv("PROGRAMDATA");
    if (program && program[0]) {
      if (out && out_len && *out_len > 0) {
        strncpy(out, program, *out_len - 1);
        out[*out_len - 1] = '\0';
        *out_len = strlen(out);
      }
      return 0;
    }
  }
  const char *profile = getenv("USERPROFILE");
  if (profile && profile[0]) {
    size_t len = *out_len;
    int err = path_join(
      (const char *[]) {profile, "AppData", "Local", NULL},
      out,
      &len,
      path_behavior_system
    );
    if (err == 0 && out_len) *out_len = len;
    return err;
  }
#endif
  return uv_os_homedir(out, out_len);
}

#endif


