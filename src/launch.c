#include <js.h>
#include <log.h>
#include <path.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <stdio.h>
#include <uv.h>

#include "../include/appling.h"

static void
appling__bootstrap_log(const char *tag, const char *detail) {
  const char *log_path = getenv("PEAR_BOOTSTRAP_LOG");
  if (log_path == NULL || log_path[0] == '\0') return;

  FILE *fp = fopen(log_path, "a");
  if (fp == NULL) return;

  uint64_t ts = uv_hrtime();
  fprintf(fp, "[%llu] %s: %s\n",
    (unsigned long long) ts,
    tag ? tag : "event",
    detail ? detail : ""
  );
  fclose(fp);
}

int
appling_launch(const appling_platform_t *platform, const appling_app_t *app, const appling_link_t *link, const char *name) {
  int err;

  appling_path_t path;
  size_t path_len = sizeof(appling_path_t);

  path_join(
    (const char *[]) {platform->path, "lib", appling_platform_entry, NULL},
    path,
    &path_len,
    path_behavior_system
  );

  appling__bootstrap_log("launch-dll", path);

  uv_lib_t library;
  err = uv_dlopen(path, &library);
  if (err < 0) {
    const char *dlerr = uv_dlerror(&library);
    char buf[256];
    snprintf(buf, sizeof(buf), "err=%d %s", err, dlerr ? dlerr : "unknown");
    appling__bootstrap_log("launch-dlopen", buf);
    return err;
  }

  appling_launch_cb launch;
  err = uv_dlsym(&library, "appling_launch_v0", (void **) &launch);
  if (err < 0) {
    uv_dlclose(&library);

    const char *dlerr = uv_dlerror(&library);
    char buf[256];
    snprintf(buf, sizeof(buf), "err=%d %s", err, dlerr ? dlerr : "unknown");
    appling__bootstrap_log("launch-dlsym", buf);

    return err; // Must exist
  }

  appling_launch_info_t info = {
    .version = 1,
    .path = path,
    .platform = platform,
    .app = app,
    .link = link,
    .name = name,
  };

  err = launch(&info);

  uv_dlclose(&library);

  if (err < 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "err=%d", err);
    appling__bootstrap_log("launch-entry", buf);
  }

  return err;
}
