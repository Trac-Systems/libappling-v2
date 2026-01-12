#include <compact.h>
#include <fs.h>
#include <log.h>
#include <path.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf.h>
#include <uv.h>

#include "../include/appling.h"

static void
appling__bootstrap_log(const char *tag, const char *detail) {
  const char *log_path = getenv("PEAR_BOOTSTRAP_LOG");
  if (!log_path || !log_path[0]) return;

  FILE *fp = fopen(log_path, "a");
  if (!fp) return;

  fprintf(fp, "[%llu] %s: %s\n",
    (unsigned long long) uv_hrtime(),
    tag ? tag : "(null)",
    detail ? detail : ""
  );

  fclose(fp);
}

static void
appling_resolve__realpath(appling_resolve_t *req);

static void
appling_resolve__on_close(fs_close_t *fs_req, int status) {
  appling_resolve_t *req = (appling_resolve_t *) fs_req->data;

  if (!req) {
    appling__bootstrap_log("resolve-close", "req-null");
    return;
  }

  {
    char buf[128];
    snprintf(buf, sizeof(buf), "status=%d", status);
    appling__bootstrap_log("resolve-close", buf);
  }

  if (req->status < 0) status = req->status;

  if (status >= 0) {
    if (req->cb) req->cb(req, 0);
  } else {
    size_t i = ++req->candidate;

    if (appling_platform_candidates[i]) appling_resolve__realpath(req);
    else if (req->cb) req->cb(req, status);
  }
}

static void
appling_resolve__on_read(fs_read_t *fs_req, int status, size_t read) {
  int err;

  appling_resolve_t *req = (appling_resolve_t *) fs_req->data;

  if (!req) {
    appling__bootstrap_log("resolve-read", "req-null");
    return;
  }

  if (status >= 0) {
    compact_state_t state = {
      0,
      req->buf.len,
      (uint8_t *) req->buf.base,
    };

    uint8_t key[APPLING_KEY_LEN];
    err = compact_decode_fixed32(&state, key);

    if (err < 0) {
      req->status = err; // Propagate
      goto close;
    }

    uintmax_t length;
    err = compact_decode_uint(&state, &length);

    if (err < 0) {
      req->status = err; // Propagate
      goto close;
    }

    uintmax_t fork;
    err = compact_decode_uint(&state, &fork);

    if (err < 0) {
      req->status = err; // Propagate
      goto close;
    }

    if (memcmp(key, req->platform->key, APPLING_KEY_LEN) == 0) {
      if (length < req->platform->length || fork != req->platform->fork) {
        req->status = -1;
        goto close;
      }
    }

    utf8_string_view_t os;
    err = compact_decode_utf8(&state, &os);

    if (err < 0) {
      req->status = err; // Propagate
      goto close;
    }

    if (utf8_string_view_compare_literal(os, (const utf8_t *) APPLING_OS, -1) != 0) {
      req->status = -1;
      goto close;
    }

    utf8_string_view_t arch;
    err = compact_decode_utf8(&state, &arch);

    if (err < 0) {
      req->status = err; // Propagate
      goto close;
    }

    if (utf8_string_view_compare_literal(arch, (const utf8_t *) APPLING_ARCH, -1) != 0) {
      req->status = -1;
      goto close;
    }

    memcpy(req->platform->key, key, APPLING_KEY_LEN);

    req->platform->length = length;
    req->platform->fork = fork;

    req->status = 0; // Reset
  } else {
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "status=%d", status);
      appling__bootstrap_log("resolve-read", buf);
    }
    req->status = status; // Propagate
  }

close:
  free(req->buf.base);

  fs_close(req->loop, &req->close, req->file, appling_resolve__on_close);
}

static void
appling_resolve__on_stat(fs_stat_t *fs_req, int status, const uv_stat_t *stat) {
  appling_resolve_t *req = (appling_resolve_t *) fs_req->data;

  if (!req) {
    appling__bootstrap_log("resolve-stat", "req-null");
    return;
  }

  if (status >= 0) {
    size_t len = stat->st_size;

    req->buf = uv_buf_init(malloc(len), len);

    fs_read(req->loop, &req->read, req->file, &req->buf, 1, 0, appling_resolve__on_read);
  } else {
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "status=%d", status);
      appling__bootstrap_log("resolve-stat", buf);
    }
    req->status = status; // Propagate

    fs_close(req->loop, &req->close, req->file, appling_resolve__on_close);
  }
}

static void
appling_resolve__on_open(fs_open_t *fs_req, int status, uv_file file) {
  appling_resolve_t *req = (appling_resolve_t *) fs_req->data;

  if (!req) {
    appling__bootstrap_log("resolve-open", "req-null");
    return;
  }

  if (status >= 0) {
    req->file = file;

    fs_stat(req->loop, &req->stat, req->file, appling_resolve__on_stat);
  } else {
    appling_path_t path;
    size_t path_len = sizeof(appling_path_t);
    path_join(
      (const char *[]) {req->platform->path, "..", "..", "checkout", NULL},
      path,
      &path_len,
      path_behavior_system
    );
    {
      char buf[256];
      snprintf(buf, sizeof(buf), "status=%d path=%s", status, path);
      appling__bootstrap_log("resolve-open", buf);
    }
    if (req->cb) req->cb(req, status);
  }
}

static void
appling_resolve__open(appling_resolve_t *req) {
  appling_path_t path;
  size_t path_len = sizeof(appling_path_t);

  path_join(
    (const char *[]) {req->platform->path, "..", "..", "checkout", NULL},
    path,
    &path_len,
    path_behavior_system
  );

  log_debug("appling_resolve() opening checkout file at %s", path);

  fs_open(req->loop, &req->open, path, 0, UV_FS_READ, appling_resolve__on_open);
}

static void
appling_resolve__on_realpath(fs_realpath_t *fs_req, int status, const char *path) {
  appling_resolve_t *req = (appling_resolve_t *) fs_req->data;

  if (!req) {
    appling__bootstrap_log("resolve-realpath", "req-null");
    return;
  }

  if (status >= 0) {
    strcpy(req->platform->path, path);

    appling_resolve__open(req);
  } else {
    {
      appling_path_t candidate_path;
      size_t candidate_len = sizeof(appling_path_t);
      size_t i = req->candidate;
      if (appling_platform_candidates[i]) {
        path_join(
          (const char *[]) {req->path, appling_platform_candidates[i], NULL},
          candidate_path,
          &candidate_len,
          path_behavior_system
        );
        char buf[256];
        snprintf(buf, sizeof(buf), "status=%d path=%s", status, candidate_path);
        appling__bootstrap_log("resolve-realpath", buf);
      }
    }
    size_t i = ++req->candidate;

    if (appling_platform_candidates[i]) appling_resolve__realpath(req);
    else if (req->cb) req->cb(req, status);
  }
}

static void
appling_resolve__realpath(appling_resolve_t *req) {
  size_t i = req->candidate;

  appling_path_t path;
  size_t path_len = sizeof(appling_path_t);

  path_join(
    (const char *[]) {req->path, appling_platform_candidates[i], NULL},
    path,
    &path_len,
    path_behavior_system
  );

  {
    char buf[256];
    snprintf(buf, sizeof(buf), "candidate=%zu path=%s", i, path);
    appling__bootstrap_log("resolve-candidate", buf);
  }

  log_debug("appling_resolve() accessing platform at %s", path);

  fs_realpath(req->loop, &req->realpath, path, appling_resolve__on_realpath);
}

int
appling_resolve(uv_loop_t *loop, appling_resolve_t *req, const char *dir, appling_platform_t *platform, appling_resolve_cb cb) {
  int err;

  req->loop = loop;
  req->cb = cb;
  req->platform = platform;
  req->candidate = 0;
  req->status = 0;
  req->realpath.data = (void *) req;
  req->open.data = (void *) req;
  req->stat.data = (void *) req;
  req->read.data = (void *) req;
  req->close.data = (void *) req;

  if (dir && path_is_absolute(dir, path_behavior_system)) strcpy(req->path, dir);
  else if (dir) {
    appling_path_t cwd;
    size_t path_len = sizeof(appling_path_t);

    err = uv_cwd(cwd, &path_len);
    if (err < 0) return err;

    path_len = sizeof(appling_path_t);

    path_join(
      (const char *[]) {cwd, dir, NULL},
      req->path,
      &path_len,
      path_behavior_system
    );
  } else {
    appling_path_t homedir;
    size_t path_len = sizeof(appling_path_t);

    err = uv_os_homedir(homedir, &path_len);
    if (err < 0) return err;

    path_len = sizeof(appling_path_t);

    path_join(
      (const char *[]) {homedir, appling_platform_dir, NULL},
      req->path,
      &path_len,
      path_behavior_system
    );
  }

  appling__bootstrap_log("resolve-root", req->path);

  appling_resolve__realpath(req);

  return 0;
}
