#include <js.h>
#include <log.h>
#include <path.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <stdio.h>
#include <uv.h>

#if defined(APPLING_OS_WIN32)
#define _CRT_SECURE_NO_WARNINGS
#include <process.h>
#include <wchar.h>
#include <windows.h>
#include <assert.h>
#endif

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

#if defined(APPLING_OS_WIN32)
static inline int32_t
appling__wtf8_decode1(const char **input) {
  uint32_t code_point;
  uint8_t b1;
  uint8_t b2;
  uint8_t b3;
  uint8_t b4;

  b1 = **input;
  if (b1 <= 0x7F) return b1;
  if (b1 < 0xC2) return -1;
  code_point = b1;

  b2 = *++*input;
  if ((b2 & 0xC0) != 0x80) return -1;
  code_point = (code_point << 6) | (b2 & 0x3F);
  if (b1 <= 0xDF) return 0x7FF & code_point;

  b3 = *++*input;
  if ((b3 & 0xC0) != 0x80) return -1;
  code_point = (code_point << 6) | (b3 & 0x3F);
  if (b1 <= 0xEF) return 0xFFFF & code_point;

  b4 = *++*input;
  if ((b4 & 0xC0) != 0x80) return -1;
  code_point = (code_point << 6) | (b4 & 0x3F);
  if (b1 <= 0xF4) {
    code_point &= 0x1FFFFF;
    if (code_point <= 0x10FFFF) return code_point;
  }

  return -1;
}

static inline ssize_t
appling__utf16_length_from_wtf8(const char *source) {
  size_t target_len = 0;
  int32_t code_point;

  do {
    code_point = appling__wtf8_decode1(&source);

    if (code_point < 0) return -1;
    if (code_point > 0xFFFF) target_len++;

    target_len++;
  } while (*source++);

  return target_len;
}

static inline void
appling__wtf8_to_utf16(const char *source, uint16_t *target) {
  int32_t code_point;

  do {
    code_point = appling__wtf8_decode1(&source);

    if (code_point > 0xFFFF) {
      *target++ = (((code_point - 0x10000) >> 10) + 0xD800);
      *target++ = ((code_point - 0x10000) & 0x3FF) + 0xDC00;
    } else {
      *target++ = code_point;
    }
  } while (*source++);
}

static inline int
appling__utf8_to_utf16(const char *utf8, WCHAR **result) {
  int len;
  len = appling__utf16_length_from_wtf8(utf8);
  if (len < 0) return -1;

  WCHAR *utf16 = malloc(len * sizeof(WCHAR));
  assert(utf16);

  appling__wtf8_to_utf16(utf8, utf16);

  *result = utf16;

  return 0;
}

static inline WCHAR *
appling__quote_argument(const WCHAR *source, WCHAR *target) {
  size_t len = wcslen(source);
  size_t i;
  int quote_hit;
  WCHAR *start;

  if (len == 0) {
    *(target++) = L'"';
    *(target++) = L'"';

    return target;
  }

  if (NULL == wcspbrk(source, L" \t\"")) {
    wcsncpy(target, source, len);
    target += len;

    return target;
  }

  if (NULL == wcspbrk(source, L"\"\\")) {
    *(target++) = L'"';
    wcsncpy(target, source, len);
    target += len;
    *(target++) = L'"';

    return target;
  }

  *(target++) = L'"';
  start = target;
  quote_hit = 1;

  for (i = len; i > 0; --i) {
    *(target++) = source[i - 1];

    if (quote_hit && source[i - 1] == L'\\') {
      *(target++) = L'\\';
    } else if (source[i - 1] == L'"') {
      quote_hit = 1;
      *(target++) = L'\\';
    } else {
      quote_hit = 0;
    }
  }

  target[0] = L'\0';
  _wcsrev(start);
  *(target++) = L'"';

  return target;
}

static inline int
appling__argv_to_command_line(const char *const *args, WCHAR **result) {
  const char *const *arg;
  WCHAR *dst = NULL;
  WCHAR *tmp = NULL;
  size_t dst_len = 0;
  size_t tmp_len = 0;
  WCHAR *pos;
  int arg_count = 0;

  for (arg = args; *arg; arg++) {
    ssize_t arg_len = appling__utf16_length_from_wtf8(*arg);

    if (arg_len < 0) return arg_len;

    dst_len += arg_len;

    if ((size_t) arg_len > tmp_len) tmp_len = arg_len;

    arg_count++;
  }

  dst_len = dst_len * 2 + arg_count * 2;

  dst = malloc(dst_len * sizeof(WCHAR));
  assert(dst);

  tmp = malloc(tmp_len * sizeof(WCHAR));
  assert(tmp);

  pos = dst;

  for (arg = args; *arg; arg++) {
    ssize_t arg_len = appling__utf16_length_from_wtf8(*arg);

    appling__wtf8_to_utf16(*arg, tmp);

    pos = appling__quote_argument(tmp, pos);

    *pos++ = *(arg + 1) ? L' ' : L'\0';
  }

  free(tmp);

  *result = dst;

  return 0;
}

static int
appling__launch_direct(const appling_platform_t *platform, const appling_app_t *app, const appling_link_t *link, const char *name) {
  int err;

  if (platform == NULL || app == NULL || link == NULL) {
    appling__bootstrap_log("launch-direct-skip", "missing platform/app/link");
    return -1;
  }

  appling_path_t runtime;
  size_t runtime_len = sizeof(appling_path_t);
  path_join(
    (const char *[]) {platform->path, "bin", "pear-runtime.exe", NULL},
    runtime,
    &runtime_len,
    path_behavior_system
  );

  appling_path_t appling;
  strncpy(appling, app->path, sizeof(appling) - 1);
  appling[sizeof(appling) - 1] = '\0';

  int alias_applied = 0;
  const char *local = getenv("LOCALAPPDATA");
  if (local && local[0] && name && name[0]) {
    if (strstr(appling, "\\WindowsApps\\") != NULL ||
        strstr(appling, "/WindowsApps/") != NULL) {
      appling__bootstrap_log("launch-direct-app-windowsapps", appling);
      appling_path_t fallback;
      snprintf(
        fallback,
        sizeof(fallback),
        "%s\\Microsoft\\WindowsApps\\%s.exe",
        local,
        name
      );
      strncpy(appling, fallback, sizeof(appling) - 1);
      appling[sizeof(appling) - 1] = '\0';
      alias_applied = 1;
      appling__bootstrap_log("launch-direct-app-alias", appling);
    }
  }

  {
    uv_fs_t stat_req;
    int rc = uv_fs_stat(uv_default_loop(), &stat_req, appling, NULL);
    if (rc < 0) {
      char buf[256];
      snprintf(buf, sizeof(buf), "missing(%d) %s", rc, appling);
      appling__bootstrap_log("launch-direct-app-missing", buf);
      if (!alias_applied && local && local[0] && name && name[0]) {
        appling_path_t fallback;
        snprintf(
          fallback,
          sizeof(fallback),
          "%s\\Microsoft\\WindowsApps\\%s.exe",
          local,
          name
        );
        strncpy(appling, fallback, sizeof(appling) - 1);
        appling[sizeof(appling) - 1] = '\0';
        appling__bootstrap_log("launch-direct-app-fallback", appling);
      }
    } else {
      appling__bootstrap_log("launch-direct-app", appling);
    }
    uv_fs_req_cleanup(&stat_req);
  }

  char link_buf[7 /* pear:// */ + APPLING_ID_MAX + 1 /* / */ + APPLING_LINK_DATA_MAX + 1 /* NULL */] = {'\0'};
  strcat(link_buf, "pear://");
  strcat(link_buf, link->id);
  if (strlen(link->data)) {
    strcat(link_buf, "/");
    strcat(link_buf, link->data);
  }

  char *argv[8];
  size_t i = 0;
  argv[i++] = runtime;
  argv[i++] = "run";
  argv[i++] = "--trusted";
  argv[i++] = "--appling";
  argv[i++] = appling;
  argv[i++] = "--no-sandbox";
  argv[i++] = link_buf;
  argv[i] = NULL;

  {
    char cmd[1024];
    cmd[0] = '\0';
    for (size_t j = 0; argv[j] != NULL; j++) {
      if (j > 0) strncat(cmd, " ", sizeof(cmd) - strlen(cmd) - 1);
      strncat(cmd, argv[j], sizeof(cmd) - strlen(cmd) - 1);
    }
    appling__bootstrap_log("launch-direct-cmd", cmd);
  }

  STARTUPINFOW si;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);

  PROCESS_INFORMATION pi;
  ZeroMemory(&pi, sizeof(pi));

  WCHAR *application_name;
  err = appling__utf8_to_utf16(runtime, &application_name);
  if (err < 0) {
    char buf[128];
    snprintf(buf, sizeof(buf), "utf16 err=%d", err);
    appling__bootstrap_log("launch-direct-utf16", buf);
    return err;
  }

  WCHAR *command_line;
  err = appling__argv_to_command_line((const char *const *) argv, &command_line);
  if (err < 0) {
    char buf[128];
    snprintf(buf, sizeof(buf), "cmdline err=%d", err);
    appling__bootstrap_log("launch-direct-cmdline", buf);
    free(application_name);
    return err;
  }

  WCHAR *current_dir = NULL;
  err = appling__utf8_to_utf16(platform->path, &current_dir);
  if (err == 0 && current_dir != NULL) {
    appling__bootstrap_log("launch-direct-cwd", platform->path);
  } else {
    appling__bootstrap_log("launch-direct-cwd", "(null)");
    if (current_dir) {
      free(current_dir);
      current_dir = NULL;
    }
  }

  BOOL success = CreateProcessW(
    application_name,
    command_line,
    NULL,
    NULL,
    FALSE,
    CREATE_NO_WINDOW,
    NULL,
    current_dir,
    &si,
    &pi
  );

  free(application_name);
  free(command_line);
  if (current_dir) free(current_dir);

  if (!success) {
    DWORD last = GetLastError();
    char buf[128];
    snprintf(buf, sizeof(buf), "CreateProcessW err=%lu", (unsigned long) last);
    appling__bootstrap_log("launch-direct-createprocess", buf);
    return -1;
  }

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD status;
  success = GetExitCodeProcess(pi.hProcess, &status);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  if (!success) {
    DWORD last = GetLastError();
    char buf[128];
    snprintf(buf, sizeof(buf), "GetExitCodeProcess err=%lu", (unsigned long) last);
    appling__bootstrap_log("launch-direct-exitcode", buf);
    return -1;
  }

  if (status != 0) {
    char buf[128];
    snprintf(buf, sizeof(buf), "pear-runtime exit=%lu", (unsigned long) status);
    appling__bootstrap_log("launch-direct-exit", buf);
  }

  return status == 0 ? 0 : -1;
}
#endif

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

#if defined(APPLING_OS_WIN32)
  err = appling__launch_direct(platform, app, link, name);
  if (err == 0) {
    return 0;
  }
  appling__bootstrap_log("launch-direct-fallback", "using launch.dll");
#endif

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
