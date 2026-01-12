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

  {
    appling_path_t runtime;
    size_t runtime_len = sizeof(appling_path_t);
    path_join(
      (const char *[]) {
        platform->path,
        "bin",
#if defined(APPLING_OS_WIN32)
        "pear-runtime.exe",
#else
        "pear-runtime",
#endif
        NULL
      },
      runtime,
      &runtime_len,
      path_behavior_system
    );
    uv_fs_t stat_req;
    int rc = uv_fs_stat(uv_default_loop(), &stat_req, runtime, NULL);
    if (rc < 0) {
      char buf[256];
      snprintf(buf, sizeof(buf), "missing(%d) %s", rc, runtime);
      appling__bootstrap_log("runtime-missing", buf);
    } else {
      appling__bootstrap_log("runtime-ok", runtime);
    }
    uv_fs_req_cleanup(&stat_req);
  }

  appling__bootstrap_log("launch-dll", path);
  {
    char buf[512];
    snprintf(buf, sizeof(buf), "platform=%s", platform->path);
    appling__bootstrap_log("launch-platform", buf);
  }

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

  {
    char buf[1024];
    snprintf(buf, sizeof(buf), "app=%s", app ? app->path : "(null)");
    appling__bootstrap_log("launch-app", buf);
  }
  {
    char buf[1024];
    if (link) {
      snprintf(buf, sizeof(buf), "link=%s/%s", link->id, link->data);
    } else {
      snprintf(buf, sizeof(buf), "link=(null)");
    }
    appling__bootstrap_log("launch-link", buf);
  }
  {
    char buf[512];
    snprintf(buf, sizeof(buf), "name=%s", name ? name : "(null)");
    appling__bootstrap_log("launch-name", buf);
  }

  err = -1;

#if defined(APPLING_OS_WIN32)
  appling_launch_cb launch_v0;
  int sym_err = uv_dlsym(&library, "appling_launch_v0", (void **) &launch_v0);
  if (sym_err == 0 && launch_v0 != NULL) {
    appling_launch_info_t info_v0 = info;
    info_v0.version = 0;
    info_v0.name = NULL;
    appling__bootstrap_log("launch-mode", "v0");
    err = launch_v0(&info_v0);
  } else {
    char buf[128];
    snprintf(buf, sizeof(buf), "sym err=%d", sym_err);
    appling__bootstrap_log("launch-v0-missing", buf);
  }
#endif

  if (err < 0) {
    appling__bootstrap_log("launch-mode", "default");
    err = launch(&info);
  }

  uv_dlclose(&library);

  if (err < 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "err=%d", err);
    appling__bootstrap_log("launch-entry", buf);
  }

  return err;
}
