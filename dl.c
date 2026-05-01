/*
 * dl — dense list. Project directory scanner.
 * Shows a shallow tree with file counts, sizes, and relative times.
 *
 * Architecture: collect all rows first, compute global column widths,
 * then print everything aligned.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── configuration ─────────────────────────────────────────────── */

#define DEFAULT_DEPTH 2
#define MAX_DEPTH 64
#define INITIAL_ROWS 1024
#define GIT_MAP_SIZE 4096

/* Hardcoded ignore list — always collapsed/hidden */
static const char *ignored_dirs[] = {
    ".git",       "node_modules",  "__pycache__",
    ".tox",       ".mypy_cache",   ".pytest_cache",
    ".venv",      "venv",          ".eggs",
    "*.egg-info", ".next",         ".nuxt",
    ".output",    "dist",          "build",
    ".cache",     ".parcel-cache", ".turbo",
    "target",                /* rust */
    "_build",                /* elixir/erlang */
    "deps",                  /* elixir mix */
    ".zig-cache", "zig-out", /* zig */
    NULL};

/* ── file type classification ───────────────────────────────────── */

enum ftype {
  FT_DIR = 0,
  FT_SOURCE,
  FT_CONFIG,
  FT_DOC,
  FT_BUILD,
  FT_DATA,
  FT_MEDIA,
  FT_ARCHIVE,
  FT_OTHER,
  FT_COUNT
};

static const char *ftype_labels[] = {
    [FT_DIR] = "directories", [FT_SOURCE] = "source",   [FT_CONFIG] = "config",
    [FT_DOC] = "docs",        [FT_BUILD] = "build",     [FT_DATA] = "data",
    [FT_MEDIA] = "media",     [FT_ARCHIVE] = "archive", [FT_OTHER] = "other",
};

static int has_ext(const char *name, const char *ext) {
  size_t nlen = strlen(name);
  size_t elen = strlen(ext);
  if (nlen <= elen)
    return 0;
  return strcasecmp(name + nlen - elen, ext) == 0;
}

static enum ftype classify(const char *name, int is_dir) {
  if (is_dir)
    return FT_DIR;

  /* exact name matches */
  if (strcasecmp(name, "Makefile") == 0 ||
      strcasecmp(name, "Dockerfile") == 0 ||
      strcasecmp(name, "Justfile") == 0 || strcasecmp(name, "Taskfile") == 0 ||
      strcasecmp(name, "Vagrantfile") == 0 ||
      strcasecmp(name, "Rakefile") == 0 || strcasecmp(name, "Gemfile") == 0 ||
      strcasecmp(name, "Procfile") == 0 || strcmp(name, ".editorconfig") == 0 ||
      strcmp(name, ".gitignore") == 0 || strcmp(name, ".gitattributes") == 0 ||
      strcmp(name, ".dockerignore") == 0)
    return FT_CONFIG;

  if (strcasecmp(name, "LICENSE") == 0 || strcasecmp(name, "LICENCE") == 0 ||
      strcasecmp(name, "AUTHORS") == 0 ||
      strcasecmp(name, "CONTRIBUTORS") == 0 ||
      strcasecmp(name, "CHANGELOG") == 0 || strcasecmp(name, "CHANGES") == 0 ||
      strcasecmp(name, "CONTRIBUTING") == 0)
    return FT_DOC;

  /* extension matches */
  /* source */
  static const char *src_exts[] = {
      ".c",     ".h",      ".go",    ".rs",     ".py",  ".js",      ".ts",
      ".tsx",   ".jsx",    ".java",  ".rb",     ".ex",  ".exs",     ".erl",
      ".hrl",   ".zig",    ".swift", ".kt",     ".kts", ".lua",     ".sh",
      ".zsh",   ".bash",   ".fish",  ".pl",     ".pm",  ".php",     ".cs",
      ".cpp",   ".cc",     ".cxx",   ".hpp",    ".hxx", ".m",       ".mm",
      ".scala", ".clj",    ".cljs",  ".hs",     ".ml",  ".mli",     ".r",
      ".jl",    ".dart",   ".v",     ".sv",     ".nim", ".cr",      ".d",
      ".f90",   ".f95",    ".asm",   ".s",      ".sql", ".graphql", ".gql",
      ".proto", ".thrift", ".vue",   ".svelte", ".elm", NULL};
  for (int i = 0; src_exts[i]; i++)
    if (has_ext(name, src_exts[i]))
      return FT_SOURCE;

  /* config */
  static const char *cfg_exts[] = {
      ".json",       ".yaml",     ".yml",        ".toml",    ".ini",
      ".cfg",        ".conf",     ".env",        ".xml",     ".plist",
      ".properties", ".eslintrc", ".prettierrc", ".babelrc", NULL};
  for (int i = 0; cfg_exts[i]; i++)
    if (has_ext(name, cfg_exts[i]))
      return FT_CONFIG;

  /* doc */
  static const char *doc_exts[] = {".md",  ".txt", ".rst", ".adoc", ".org",
                                   ".tex", ".man", ".1",   ".2",    ".3",
                                   ".5",   ".8",   NULL};
  for (int i = 0; doc_exts[i]; i++)
    if (has_ext(name, doc_exts[i]))
      return FT_DOC;

  /* build artifacts */
  static const char *bld_exts[] = {".o",    ".a",    ".so",    ".dylib", ".exe",
                                   ".dll",  ".wasm", ".class", ".pyc",   ".pyo",
                                   ".beam", ".lock", ".sum",   NULL};
  for (int i = 0; bld_exts[i]; i++)
    if (has_ext(name, bld_exts[i]))
      return FT_BUILD;

  /* data */
  static const char *dat_exts[] = {".csv",    ".tsv",  ".parquet",
                                   ".sqlite", ".db",   ".ndjson",
                                   ".jsonl",  ".avro", NULL};
  for (int i = 0; dat_exts[i]; i++)
    if (has_ext(name, dat_exts[i]))
      return FT_DATA;

  /* media */
  static const char *med_exts[] = {".png", ".jpg",  ".jpeg", ".gif",  ".svg",
                                   ".ico", ".webp", ".bmp",  ".tiff", ".mp3",
                                   ".mp4", ".wav",  ".ogg",  ".flac", ".avi",
                                   ".mkv", ".mov",  ".pdf",  NULL};
  for (int i = 0; med_exts[i]; i++)
    if (has_ext(name, med_exts[i]))
      return FT_MEDIA;

  /* archive */
  static const char *arc_exts[] = {".tar", ".gz",  ".tgz", ".zip",
                                   ".bz2", ".xz",  ".7z",  ".rar",
                                   ".zst", ".lz4", NULL};
  for (int i = 0; arc_exts[i]; i++)
    if (has_ext(name, arc_exts[i]))
      return FT_ARCHIVE;

  return FT_OTHER;
}

/* ── types ─────────────────────────────────────────────────────── */

typedef struct {
  char name[300];    /* display name with suffix */
  char perms[12];    /* drwxrwxrwx */
  char owner[32];    /* username if differs from current user, else empty */
  char col_a[32];    /* "N files" for dirs, size for files */
  char col_b[512];   /* "M dirs", git subject, or symlink target */
  char col_size[16]; /* total subtree size for dirs, empty for files */
  char git[4];       /* git status char: M, A, ?, D, R, or empty */
  char time[16];     /* relative time */
  enum ftype type;   /* file type category */
  int depth;
  int is_last;
  int is_ignored;
  int name_len;
  int owner_len;
  int col_a_len;
  int col_b_len;
  int col_size_len;
  int git_len;
  int time_len;
} row_t;

typedef struct {
  row_t *rows;
  int count;
  int capacity;
} rowlist_t;

typedef struct {
  int depth;
  int show_all;      /* -a: include dotfiles */
  int flat;          /* -f: flat list, no tree, no recurse */
  int show_git;      /* -g: show git status column */
  int use_gitignore; /* -G: also read .gitignore */
  int group_type;    /* -t: group by file type */
  const char
      *target_path; /* current listing target, for path-relative git ops */
} options_t;

/* ── git status map (simple linear scan, fine for project sizes) ── */

typedef struct {
  char path[512];
  char status; /* M, A, ?, D, R, C, U */
} git_entry_t;

typedef struct {
  git_entry_t entries[GIT_MAP_SIZE];
  int count;
  int active;       /* whether git is available */
  char root[512];   /* git repo root */
  char prefix[512]; /* listing target offset within repo, no trailing slash */

  /* banner data */
  char branch[128];
  char head_hash[16];
  char head_subject[256];
  int ahead;
  int behind;
  int has_upstream;

  /* footer counters */
  int n_modified;
  int n_untracked;
  int n_added;
  int n_deleted;
  int n_renamed;
  int n_unmerged;
} git_map_t;

/* ── gitignore set (entries hidden by `-G`) ─────────────────────── */

typedef struct {
  char paths[GIT_MAP_SIZE][512];
  int count;
  int active;
  char prefix[512]; /* listing target offset within repo */
} gitignore_set_t;

/* ── globals ───────────────────────────────────────────────────── */

static time_t now;
static uid_t current_uid;
static git_map_t git_map;
static gitignore_set_t gi_set;

/* ── helpers ───────────────────────────────────────────────────── */

static int is_ignored(const char *name) {
  for (int i = 0; ignored_dirs[i]; i++) {
    if (strcmp(name, ignored_dirs[i]) == 0)
      return 1;
  }
  return 0;
}

static void fmt_size(off_t bytes, char *buf, size_t bufsz) {
  static const char *units[] = {"B", "K", "M", "G", "T"};
  double sz = (double)bytes;
  int u = 0;

  while (sz >= 1000.0 && u < 4) {
    sz /= 1024.0;
    u++;
  }

  if (u == 0)
    snprintf(buf, bufsz, "%lldB", (long long)bytes);
  else if (sz >= 100.0)
    snprintf(buf, bufsz, "%.0f%s", sz, units[u]);
  else if (sz >= 10.0)
    snprintf(buf, bufsz, "%.1f%s", sz, units[u]);
  else
    snprintf(buf, bufsz, "%.2f%s", sz, units[u]);
}

static void fmt_reltime(time_t mtime, char *buf, size_t bufsz) {
  long diff = (long)(now - mtime);
  if (diff < 0)
    diff = 0;

  if (diff < 60)
    snprintf(buf, bufsz, "%lds", diff);
  else if (diff < 3600)
    snprintf(buf, bufsz, "%ldm", diff / 60);
  else if (diff < 86400)
    snprintf(buf, bufsz, "%ldh", diff / 3600);
  else if (diff < 86400 * 30)
    snprintf(buf, bufsz, "%ldd", diff / 86400);
  else if (diff < 86400 * 365)
    snprintf(buf, bufsz, "%ldmo", diff / (86400 * 30));
  else
    snprintf(buf, bufsz, "%ldy", diff / (86400 * 365));
}

/* clamp snprintf return value to chars actually stored (cap-1 on overflow) */
static int sn_stored_len(int n, size_t cap) {
  if (n < 0)
    return 0;
  return (size_t)n >= cap ? (int)cap - 1 : n;
}

static void fmt_perms(mode_t mode, char *buf) {
  buf[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
  buf[1] = (mode & S_IRUSR) ? 'r' : '-';
  buf[2] = (mode & S_IWUSR) ? 'w' : '-';
  buf[3] = (mode & S_IXUSR) ? 'x' : '-';
  buf[4] = (mode & S_IRGRP) ? 'r' : '-';
  buf[5] = (mode & S_IWGRP) ? 'w' : '-';
  buf[6] = (mode & S_IXGRP) ? 'x' : '-';
  buf[7] = (mode & S_IROTH) ? 'r' : '-';
  buf[8] = (mode & S_IWOTH) ? 'w' : '-';
  buf[9] = (mode & S_IXOTH) ? 'x' : '-';
  buf[10] = '\0';
}

/* ── git status ────────────────────────────────────────────────── */

/* shell-escape target path for popen. Single-quote, escape embedded quotes. */
static void shq(const char *in, char *out, size_t cap) {
  size_t o = 0;
  if (o < cap)
    out[o++] = '\'';
  for (size_t i = 0; in[i] && o + 4 < cap; i++) {
    if (in[i] == '\'') {
      out[o++] = '\'';
      out[o++] = '\\';
      out[o++] = '\'';
      out[o++] = '\'';
    } else {
      out[o++] = in[i];
    }
  }
  if (o < cap)
    out[o++] = '\'';
  if (o < cap)
    out[o] = '\0';
  else
    out[cap - 1] = '\0';
}

static void git_map_init(const char *target) {
  memset(&git_map, 0, sizeof(git_map));

  char qtarget[1100];
  shq(target, qtarget, sizeof(qtarget));

  char cmd[2048];

  /* find git root (run from target, not cwd) */
  snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --show-toplevel 2>/dev/null",
           qtarget);
  FILE *fp = popen(cmd, "r");
  if (!fp)
    return;

  if (fgets(git_map.root, sizeof(git_map.root), fp) == NULL) {
    pclose(fp);
    return;
  }
  pclose(fp);

  /* strip newline */
  size_t len = strlen(git_map.root);
  if (len > 0 && git_map.root[len - 1] == '\n')
    git_map.root[len - 1] = '\0';

  /* offset from repo root to listing target (porcelain paths are repo-rooted)
   */
  snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --show-prefix 2>/dev/null",
           qtarget);
  fp = popen(cmd, "r");
  if (fp) {
    if (fgets(git_map.prefix, sizeof(git_map.prefix), fp) != NULL) {
      size_t plen = strlen(git_map.prefix);
      if (plen > 0 && git_map.prefix[plen - 1] == '\n') {
        git_map.prefix[--plen] = '\0';
      }
      /* drop trailing slash for uniform join in lookup */
      if (plen > 0 && git_map.prefix[plen - 1] == '/')
        git_map.prefix[plen - 1] = '\0';
    }
    pclose(fp);
  }

  /* branch */
  snprintf(cmd, sizeof(cmd),
           "git -C %s rev-parse --abbrev-ref HEAD 2>/dev/null", qtarget);
  fp = popen(cmd, "r");
  if (fp) {
    if (fgets(git_map.branch, sizeof(git_map.branch), fp) != NULL) {
      size_t blen = strlen(git_map.branch);
      if (blen > 0 && git_map.branch[blen - 1] == '\n')
        git_map.branch[blen - 1] = '\0';
    }
    pclose(fp);
  }

  /* HEAD hash + subject */
  snprintf(cmd, sizeof(cmd),
           "git -C %s log -1 --pretty='%%h%%x09%%s' 2>/dev/null", qtarget);
  fp = popen(cmd, "r");
  if (fp) {
    char buf[512];
    if (fgets(buf, sizeof(buf), fp) != NULL) {
      size_t blen = strlen(buf);
      if (blen > 0 && buf[blen - 1] == '\n')
        buf[blen - 1] = '\0';
      char *tab = strchr(buf, '\t');
      if (tab) {
        *tab = '\0';
        snprintf(git_map.head_hash, sizeof(git_map.head_hash), "%s", buf);
        snprintf(git_map.head_subject, sizeof(git_map.head_subject), "%s",
                 tab + 1);
      }
    }
    pclose(fp);
  }

  /* ahead / behind upstream */
  snprintf(cmd, sizeof(cmd),
           "git -C %s rev-list --left-right --count "
           "HEAD...@{u} 2>/dev/null",
           qtarget);
  fp = popen(cmd, "r");
  if (fp) {
    char buf[64];
    if (fgets(buf, sizeof(buf), fp) != NULL) {
      if (sscanf(buf, "%d %d", &git_map.ahead, &git_map.behind) == 2)
        git_map.has_upstream = 1;
    }
    pclose(fp);
  }

  /* get status */
  snprintf(cmd, sizeof(cmd), "git -C %s status --porcelain 2>/dev/null",
           qtarget);
  fp = popen(cmd, "r");
  if (!fp)
    return;

  git_map.active = 1;

  char line[1024];
  while (fgets(line, sizeof(line), fp) && git_map.count < GIT_MAP_SIZE) {
    if (strlen(line) < 4)
      continue;

    git_entry_t *ge = &git_map.entries[git_map.count];

    /* porcelain format: XY filename
     * X = index status, Y = worktree status
     * prefer worktree status (Y) unless it's space, then use X */
    char x = line[0];
    char y = line[1];

    if (y != ' ' && y != '\n')
      ge->status = y;
    else if (x != ' ')
      ge->status = x;
    else
      continue;

    /* map to single meaningful char + tally */
    switch (ge->status) {
    case '?':
      ge->status = '?';
      git_map.n_untracked++;
      break;
    case 'M':
      ge->status = 'M';
      git_map.n_modified++;
      break;
    case 'A':
      ge->status = 'A';
      git_map.n_added++;
      break;
    case 'D':
      ge->status = 'D';
      git_map.n_deleted++;
      break;
    case 'R':
      ge->status = 'R';
      git_map.n_renamed++;
      break;
    case 'C':
      ge->status = 'C';
      git_map.n_added++;
      break;
    case 'U':
      ge->status = 'U';
      git_map.n_unmerged++;
      break;
    default:
      ge->status = '~';
      break;
    }

    /* extract path (skip "XY ") */
    char *path = line + 3;
    size_t plen = strlen(path);
    if (plen > 0 && path[plen - 1] == '\n')
      path[plen - 1] = '\0';

    /* strip quotes if present */
    if (path[0] == '"') {
      path++;
      plen = strlen(path);
      if (plen > 0 && path[plen - 1] == '"')
        path[plen - 1] = '\0';
    }

    strncpy(ge->path, path, sizeof(ge->path) - 1);
    git_map.count++;
  }

  pclose(fp);
}

/*
 * Look up git status for a path relative to cwd.
 * Returns status char or 0 if clean/unknown.
 */
/*
 * Fetch HEAD subject of repo at `path` into `out`. Returns 1 on success.
 * Format: "<short-hash> <subject>". Empty repo / no HEAD → returns 0.
 */
static int git_repo_subject(const char *path, char *out, size_t cap) {
  char qpath[1100];
  shq(path, qpath, sizeof(qpath));

  char cmd[2048];
  snprintf(cmd, sizeof(cmd), "git -C %s log -1 --pretty='%%h %%s' 2>/dev/null",
           qpath);

  FILE *fp = popen(cmd, "r");
  if (!fp)
    return 0;

  int ok = 0;
  if (fgets(out, cap, fp) != NULL) {
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == '\n')
      out[len - 1] = '\0';
    if (out[0])
      ok = 1;
  }
  pclose(fp);
  return ok;
}

static char git_status_for(const char *relpath) {
  if (!git_map.active)
    return 0;

  /* porcelain paths are repo-root relative; relpath is target-relative.
   * Join target offset (prefix) with relpath to form a repo-rooted key. */
  char key[1024];
  if (git_map.prefix[0] && relpath[0])
    snprintf(key, sizeof(key), "%s/%s", git_map.prefix, relpath);
  else if (git_map.prefix[0])
    snprintf(key, sizeof(key), "%s", git_map.prefix);
  else
    snprintf(key, sizeof(key), "%s", relpath);

  size_t klen = strlen(key);
  for (int i = 0; i < git_map.count; i++) {
    if (strcmp(git_map.entries[i].path, key) == 0)
      return git_map.entries[i].status;
    /* directory prefix match (any file under this dir is dirty) */
    if (klen > 0 && strncmp(git_map.entries[i].path, key, klen) == 0 &&
        git_map.entries[i].path[klen] == '/')
      return git_map.entries[i].status;
  }
  return 0;
}

/* ── gitignore set ─────────────────────────────────────────────── */

/*
 * Populate `gi_set` with paths git would consider ignored under `target`.
 * Uses `git ls-files -o -i --exclude-standard --directory` (untracked +
 * ignored, dirs collapsed). Tracked files matching .gitignore are not
 * reported — git does not consider them ignored.
 */
static void gitignore_init(const char *target) {
  memset(&gi_set, 0, sizeof(gi_set));

  char qtarget[1100];
  shq(target, qtarget, sizeof(qtarget));

  char cmd[2048];

  /* prefix (target offset within repo) for lookup-key joining */
  snprintf(cmd, sizeof(cmd), "git -C %s rev-parse --show-prefix 2>/dev/null",
           qtarget);
  FILE *fp = popen(cmd, "r");
  if (fp) {
    if (fgets(gi_set.prefix, sizeof(gi_set.prefix), fp) != NULL) {
      size_t plen = strlen(gi_set.prefix);
      if (plen > 0 && gi_set.prefix[plen - 1] == '\n')
        gi_set.prefix[--plen] = '\0';
      if (plen > 0 && gi_set.prefix[plen - 1] == '/')
        gi_set.prefix[plen - 1] = '\0';
    }
    pclose(fp);
  }

  snprintf(cmd, sizeof(cmd),
           "git -C %s ls-files -o -i --exclude-standard --directory "
           "2>/dev/null",
           qtarget);
  fp = popen(cmd, "r");
  if (!fp)
    return;

  char line[1024];
  while (fgets(line, sizeof(line), fp) && gi_set.count < GIT_MAP_SIZE) {
    size_t llen = strlen(line);
    if (llen == 0)
      continue;
    if (line[llen - 1] == '\n')
      line[--llen] = '\0';
    if (llen == 0)
      continue;
    /* git emits collapsed dirs with trailing slash — strip for uniform match */
    if (line[llen - 1] == '/')
      line[--llen] = '\0';
    if (llen == 0)
      continue;
    strncpy(gi_set.paths[gi_set.count], line,
            sizeof(gi_set.paths[gi_set.count]) - 1);
    gi_set.count++;
    gi_set.active = 1;
  }
  pclose(fp);
}

/*
 * Return 1 if `relpath` (target-relative) matches an ignored path or sits
 * under one. Lookup key joined with target prefix to match repo-rooted
 * paths from `git ls-files`.
 */
static int is_gitignored(const char *relpath) {
  if (!gi_set.active)
    return 0;

  char key[1024];
  if (gi_set.prefix[0] && relpath[0])
    snprintf(key, sizeof(key), "%s/%s", gi_set.prefix, relpath);
  else if (gi_set.prefix[0])
    snprintf(key, sizeof(key), "%s", gi_set.prefix);
  else
    snprintf(key, sizeof(key), "%s", relpath);

  size_t klen = strlen(key);
  for (int i = 0; i < gi_set.count; i++) {
    const char *p = gi_set.paths[i];
    size_t plen = strlen(p);
    if (plen == klen && strcmp(p, key) == 0)
      return 1;
    /* key sits under an ignored ancestor */
    if (plen < klen && strncmp(p, key, plen) == 0 && key[plen] == '/')
      return 1;
  }
  return 0;
}

/* ── owner lookup ──────────────────────────────────────────────── */

static const char *owner_name(uid_t uid) {
  if (uid == current_uid)
    return NULL;
  struct passwd *pw = getpwuid(uid);
  return pw ? pw->pw_name : "?";
}

/* ── row list management ───────────────────────────────────────── */

static void rowlist_init(rowlist_t *rl) {
  rl->capacity = INITIAL_ROWS;
  rl->count = 0;
  rl->rows = malloc(rl->capacity * sizeof(row_t));
  if (!rl->rows) {
    fprintf(stderr, "dl: out of memory\n");
    exit(1);
  }
}

static row_t *rowlist_add(rowlist_t *rl) {
  if (rl->count >= rl->capacity) {
    rl->capacity *= 2;
    rl->rows = realloc(rl->rows, rl->capacity * sizeof(row_t));
    if (!rl->rows) {
      fprintf(stderr, "dl: out of memory\n");
      exit(1);
    }
  }
  row_t *r = &rl->rows[rl->count++];
  memset(r, 0, sizeof(*r));
  return r;
}

static void rowlist_free(rowlist_t *rl) { free(rl->rows); }

static void rowlist_reset(rowlist_t *rl) { rl->count = 0; }

/* ── entry sorting ─────────────────────────────────────────────── */

typedef struct {
  char name[256];
  mode_t mode;
  uid_t uid;
  off_t size;
  off_t subtree_size; /* total bytes under dir */
  time_t mtime;
  enum ftype type;
  int is_dir;
  int is_link;
  int is_ignored;
  int nfiles;
  int ndirs;
  char link_target[4096];
} entry_t;

/* default sort: dirs first, then alpha */
static int entry_cmp(const void *a, const void *b) {
  const entry_t *ea = (const entry_t *)a;
  const entry_t *eb = (const entry_t *)b;

  if (ea->is_dir != eb->is_dir)
    return eb->is_dir - ea->is_dir;

  return strcasecmp(ea->name, eb->name);
}

/* type-grouped sort: by type category, then alpha within each */
static int entry_cmp_type(const void *a, const void *b) {
  const entry_t *ea = (const entry_t *)a;
  const entry_t *eb = (const entry_t *)b;

  if (ea->type != eb->type)
    return (int)ea->type - (int)eb->type;

  return strcasecmp(ea->name, eb->name);
}

/* ── directory scanning ────────────────────────────────────────── */

/*
 * Count immediate children and compute recursive subtree size.
 *
 * Honors `ignored_dirs`: ignored subdirectories still count toward
 * the parent's `ndirs` (so "5 dirs" means what you see), but their
 * contents are NOT walked and their size is NOT added. Without this,
 * a single nested node_modules dominates every scan.
 *
 * Used only for directories we will NOT emit rows for — i.e. past
 * max_depth. Within max_depth, `collect` recurses and reuses the
 * stats it computes, avoiding a second walk of the same subtree.
 */
static void scan_stats(int parent_fd, const char *name, int *nfiles, int *ndirs,
                       off_t *total_size) {
  *nfiles = 0;
  *ndirs = 0;
  *total_size = 0;

  int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return;

  DIR *d = fdopendir(fd);
  if (!d) {
    close(fd);
    return;
  }

  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.' &&
        (de->d_name[1] == '\0' ||
         (de->d_name[1] == '.' && de->d_name[2] == '\0')))
      continue;

    struct stat st;
    if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      (*ndirs)++;
      if (is_ignored(de->d_name))
        continue;
      int sub_nf, sub_nd;
      off_t sub_sz;
      scan_stats(fd, de->d_name, &sub_nf, &sub_nd, &sub_sz);
      *total_size += sub_sz;
    } else {
      (*nfiles)++;
      *total_size += st.st_size;
    }
  }

  closedir(d);
}

/*
 * Recursively scan `dirname`, appending rows to `rl` for every entry
 * within `max_depth`. Returns the immediate file/dir counts and the
 * recursive total byte size via out params, so callers that already
 * recursed don't need a second walk to produce those numbers.
 */
static void collect(int parent_fd, const char *dirname, int depth,
                    int max_depth, const options_t *opts, rowlist_t *rl,
                    const char *relpath, /* path relative to start dir */
                    int *out_nfiles, int *out_ndirs, off_t *out_size) {
  *out_nfiles = 0;
  *out_ndirs = 0;
  *out_size = 0;

  int fd = openat(parent_fd, dirname, O_RDONLY | O_DIRECTORY);
  if (fd < 0) {
    fprintf(stderr, "dl: %s: %s\n", dirname, strerror(errno));
    return;
  }

  int fd2 = dup(fd);
  if (fd2 < 0) {
    close(fd);
    return;
  }

  DIR *d = fdopendir(fd);
  if (!d) {
    close(fd);
    close(fd2);
    return;
  }

  entry_t *entries = malloc(8192 * sizeof(entry_t));
  if (!entries) {
    closedir(d);
    close(fd2);
    return;
  }

  int n = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL && n < 8192) {
    if (de->d_name[0] == '.' &&
        (de->d_name[1] == '\0' ||
         (de->d_name[1] == '.' && de->d_name[2] == '\0')))
      continue;

    if (!opts->show_all && de->d_name[0] == '.')
      continue;

    if (opts->use_gitignore) {
      char entry_relpath[1024];
      if (relpath[0])
        snprintf(entry_relpath, sizeof(entry_relpath), "%s/%s", relpath,
                 de->d_name);
      else
        snprintf(entry_relpath, sizeof(entry_relpath), "%s", de->d_name);
      if (is_gitignored(entry_relpath))
        continue;
    }

    entry_t *e = &entries[n];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, de->d_name, sizeof(e->name) - 1);

    struct stat st;
    if (fstatat(fd2, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
      continue;

    e->mode = st.st_mode;
    e->uid = st.st_uid;
    e->size = st.st_size;
    e->is_dir = S_ISDIR(st.st_mode);
    e->is_link = S_ISLNK(st.st_mode);
    e->mtime = st.st_mtime;

    if (e->is_link) {
      ssize_t len = readlinkat(fd2, de->d_name, e->link_target,
                               sizeof(e->link_target) - 1);
      if (len > 0)
        e->link_target[len] = '\0';
    }

    e->type = classify(de->d_name, e->is_dir);

    if (e->is_dir)
      e->is_ignored = is_ignored(de->d_name);

    n++;
  }

  closedir(d);

  if (opts->group_type)
    qsort(entries, n, sizeof(entry_t), entry_cmp_type);
  else
    qsort(entries, n, sizeof(entry_t), entry_cmp);

  for (int i = 0; i < n; i++) {
    entry_t *e = &entries[i];
    int row_idx = rl->count;
    row_t *r = rowlist_add(rl);

    r->depth = opts->flat ? 0 : depth;
    r->is_last = (i == n - 1);
    r->is_ignored = e->is_ignored;
    r->type = e->type;

    /* name with type suffix */
    if (e->is_dir)
      r->name_len = snprintf(r->name, sizeof(r->name), "%s/", e->name);
    else if (e->is_link)
      r->name_len = snprintf(r->name, sizeof(r->name), "%s@", e->name);
    else if (e->mode & S_IXUSR)
      r->name_len = snprintf(r->name, sizeof(r->name), "%s*", e->name);
    else
      r->name_len = snprintf(r->name, sizeof(r->name), "%s", e->name);

    /* permissions */
    fmt_perms(e->mode, r->perms);

    /* owner (only if different from current user) */
    const char *own = owner_name(e->uid);
    if (own) {
      r->owner_len = snprintf(r->owner, sizeof(r->owner), "%s", own);
    }

    /* file col_a = size (directory columns are filled below,
     * after we know the subtree stats). */
    if (!e->is_dir) {
      fmt_size(e->size, r->col_a, sizeof(r->col_a));
      r->col_a_len = (int)strlen(r->col_a);
      (*out_nfiles)++;
      (*out_size) += e->size;
    }

    if (e->is_link && e->link_target[0]) {
      int n = snprintf(r->col_b, sizeof(r->col_b), "-> %s", e->link_target);
      r->col_b_len = sn_stored_len(n, sizeof(r->col_b));
    }

    /* git status */
    if (opts->show_git && !e->is_ignored) {
      char entry_relpath[1024];
      if (relpath[0])
        snprintf(entry_relpath, sizeof(entry_relpath), "%s/%s", relpath,
                 e->name);
      else
        snprintf(entry_relpath, sizeof(entry_relpath), "%s", e->name);

      char gs = git_status_for(entry_relpath);
      if (gs) {
        r->git[0] = gs;
        r->git[1] = '\0';
        r->git_len = 1;
      }
    }

    /* time */
    if (!e->is_ignored) {
      fmt_reltime(e->mtime, r->time, sizeof(r->time));
      r->time_len = (int)strlen(r->time);
    }

    if (!e->is_dir)
      continue;

    (*out_ndirs)++;

    if (e->is_ignored) {
      /* r may be stale after earlier rowlist_add calls in
       * the loop — but no recursion has happened for this
       * entry yet, so `r` is still valid here. */
      r->col_a_len = snprintf(r->col_a, sizeof(r->col_a), "ignored");
      continue;
    }

    /* Compute the directory's stats. Either we recurse (emitting
     * rows for its contents and getting the stats for free) or we
     * fall back to scan_stats for dirs past max_depth. This is the
     * whole point of the refactor: no subtree is walked twice. */
    int sub_nf = 0, sub_nd = 0;
    off_t sub_sz = 0;

    if (!opts->flat && depth + 1 < max_depth) {
      char child_relpath[1024];
      if (relpath[0])
        snprintf(child_relpath, sizeof(child_relpath), "%s/%s", relpath,
                 e->name);
      else
        snprintf(child_relpath, sizeof(child_relpath), "%s", e->name);

      collect(fd2, e->name, depth + 1, max_depth, opts, rl, child_relpath,
              &sub_nf, &sub_nd, &sub_sz);
    } else {
      scan_stats(fd2, e->name, &sub_nf, &sub_nd, &sub_sz);
    }

    /* Re-fetch the row pointer — recursive collect calls may
     * have realloc'd rl->rows, invalidating the earlier `r`. */
    r = &rl->rows[row_idx];

    if (sub_nf > 0) {
      r->col_a_len = snprintf(r->col_a, sizeof(r->col_a), "%d files", sub_nf);
    }
    if (sub_nd > 0) {
      int n = snprintf(r->col_b, sizeof(r->col_b), "%d dirs", sub_nd);
      r->col_b_len = sn_stored_len(n, sizeof(r->col_b));
    }
    if (sub_sz > 0) {
      fmt_size(sub_sz, r->col_size, sizeof(r->col_size));
      r->col_size_len = (int)strlen(r->col_size);
    }

    /* if entry is a repo root, replace col_b with HEAD subject.
     * Detect via .git existing inside (dir for normal repos,
     * file for submodules and worktrees). */
    if (opts->show_git && !e->is_ignored) {
      struct stat gst;
      char probe[512];
      snprintf(probe, sizeof(probe), "%s/.git", e->name);
      if (fstatat(fd2, probe, &gst, 0) == 0) {
        char fullpath[2048];
        const char *base = opts->target_path ? opts->target_path : ".";
        if (relpath[0])
          snprintf(fullpath, sizeof(fullpath), "%s/%s/%s", base, relpath,
                   e->name);
        else
          snprintf(fullpath, sizeof(fullpath), "%s/%s", base, e->name);

        char subj[512];
        if (git_repo_subject(fullpath, subj, sizeof(subj))) {
          int n = snprintf(r->col_b, sizeof(r->col_b), "%s", subj);
          r->col_b_len = sn_stored_len(n, sizeof(r->col_b));
        }
      }
    }

    (*out_size) += sub_sz;
  }

  free(entries);
  close(fd2);
}

/* ── output ────────────────────────────────────────────────────── */

static int get_term_height(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return ws.ws_row;
  return 24; /* fallback */
}

static int get_term_width(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return ws.ws_col;
  return 80; /* fallback */
}

/*
 * Tab-separated output for piping.
 * Fields: name, perms, owner, git, files_or_size, dirs, subtree_size, time
 */
static void print_tsv(const rowlist_t *rl, const options_t *opts) {
  /* ignore SIGPIPE — downstream may close early */
  signal(SIGPIPE, SIG_IGN);

  for (int i = 0; i < rl->count; i++) {
    const row_t *r = &rl->rows[i];

    fprintf(stdout, "%s\t%s\t%s", r->name, r->perms, r->owner);

    if (opts->show_git)
      fprintf(stdout, "\t%s", r->git);

    fprintf(stdout, "\t%s\t%s\t%s\t%s\n", r->col_a, r->col_b, r->col_size,
            r->is_ignored ? "" : r->time);
  }
}

/*
 * Print col_b honoring width budget: right-align if fits, else truncate
 * with "..." suffix. Leading two spaces are the column separator.
 */
static void print_col_b(FILE *out, const row_t *r, int budget) {
  if (r->col_b_len <= budget)
    fprintf(out, "  %*s", budget, r->col_b);
  else if (budget >= 3)
    fprintf(out, "  %.*s...", budget - 3, r->col_b);
  else
    fprintf(out, "  %.*s", budget, "...");
}

static void print_git_banner(FILE *out) {
  if (!git_map.active)
    return;

  const char *base = git_map.root;
  const char *slash = strrchr(git_map.root, '/');
  if (slash && slash[1])
    base = slash + 1;

  fprintf(out, "repo %s", base);
  if (git_map.branch[0])
    fprintf(out, " · %s", git_map.branch);
  if (git_map.has_upstream && (git_map.ahead || git_map.behind)) {
    fprintf(out, " [");
    int wrote = 0;
    if (git_map.ahead) {
      fprintf(out, "↑%d", git_map.ahead);
      wrote = 1;
    }
    if (git_map.behind) {
      fprintf(out, "%s↓%d", wrote ? " " : "", git_map.behind);
    }
    fprintf(out, "]");
  }
  if (git_map.head_hash[0])
    fprintf(out, " · %s %s", git_map.head_hash, git_map.head_subject);
  fprintf(out, "\n");
}

static void print_git_footer(FILE *out) {
  if (!git_map.active)
    return;

  int total = git_map.n_modified + git_map.n_untracked + git_map.n_added +
              git_map.n_deleted + git_map.n_renamed + git_map.n_unmerged;
  if (total == 0 &&
      !(git_map.has_upstream && (git_map.ahead || git_map.behind)))
    return;

  fprintf(out, "\n");
  int wrote = 0;
#define EMIT(label, n)                                                         \
  do {                                                                         \
    if ((n) > 0) {                                                             \
      fprintf(out, "%s%d %s", wrote ? ", " : "", (n), label);                  \
      wrote = 1;                                                               \
    }                                                                          \
  } while (0)

  EMIT("modified", git_map.n_modified);
  EMIT("added", git_map.n_added);
  EMIT("deleted", git_map.n_deleted);
  EMIT("renamed", git_map.n_renamed);
  EMIT("untracked", git_map.n_untracked);
  EMIT("unmerged", git_map.n_unmerged);
#undef EMIT

  if (git_map.has_upstream && git_map.ahead)
    fprintf(out, "%sahead %d", wrote ? ", " : "", git_map.ahead);
  if (git_map.has_upstream && git_map.behind)
    fprintf(out, ", behind %d", git_map.behind);

  fprintf(out, "\n");
}

static void print_pretty(const rowlist_t *rl, const options_t *opts) {
  if (rl->count == 0)
    return;

#define INDENT_COLS(d) ((d) == 0 ? 0 : 3 + ((d) - 1) * 4)

  /* compute global column widths */
  int w_name = 0;
  int w_owner = 0;
  int w_col_a = 0;
  int w_col_b = 0;
  int w_col_size = 0;
  int w_git = 0;
  int w_time = 0;
  int max_depth_seen = 0;

  for (int i = 0; i < rl->count; i++) {
    const row_t *r = &rl->rows[i];
    int col1 = INDENT_COLS(r->depth) + r->name_len;
    if (col1 > w_name)
      w_name = col1;
    if (r->owner_len > w_owner)
      w_owner = r->owner_len;
    if (r->col_a_len > w_col_a)
      w_col_a = r->col_a_len;
    if (r->col_b_len > w_col_b)
      w_col_b = r->col_b_len;
    if (r->col_size_len > w_col_size)
      w_col_size = r->col_size_len;
    if (r->git_len > w_git)
      w_git = r->git_len;
    if (r->time_len > w_time)
      w_time = r->time_len;
    if (r->depth > max_depth_seen)
      max_depth_seen = r->depth;
  }

  /*
   * Metadata-only width (everything after the name column, minus col_b).
   * Reused for single-line col_b budget and two-line layout decision.
   */
  int meta_fixed = 2 + 10; /* two spaces + perms */
  if (w_owner > 0)
    meta_fixed += 2 + w_owner;
  if (opts->show_git)
    meta_fixed += (w_git > 0) ? 2 + w_git : 3;
  meta_fixed += 2 + w_col_a;
  if (w_col_size > 0)
    meta_fixed += 2 + w_col_size;
  meta_fixed += 2 + w_time + 4; /* two spaces + time + " ago" */

  int term_w = get_term_width();

  /*
   * Adaptive layout: fall back to two-line rendering when a single row
   * would overflow the terminal. Content-aware — no fixed threshold.
   */
  int single_required = w_name + meta_fixed;
  if (w_col_b > 0)
    single_required += 2 + w_col_b;
  int two_line = single_required > term_w;

  /*
   * Cap w_col_b to available budget in the active layout. col_b can
   * hold "-> <symlink>" which is unbounded and would otherwise wrap.
   */
  if (w_col_b > 0) {
    int prefix;
    if (two_line) {
      /* line 2: tree continuation (4 cols per level) or 2-col indent */
      prefix = (opts->flat || max_depth_seen == 0) ? 2 : max_depth_seen * 4;
    } else {
      prefix = w_name;
    }
    int budget = term_w - prefix - meta_fixed - 2; /* 2 = col_b separator */
    if (budget < 4)
      budget = 4;
    if (w_col_b > budget)
      w_col_b = budget;
  }

  /* decide whether to page (two-line doubles effective row count) */
  FILE *out = stdout;
  FILE *pager = NULL;

  int effective_rows = two_line ? rl->count * 2 : rl->count;
  if (effective_rows > get_term_height()) {
    const char *pager_cmd = getenv("PAGER");
    if (!pager_cmd || !pager_cmd[0])
      pager_cmd = "less -R";

    signal(SIGPIPE, SIG_IGN);

    pager = popen(pager_cmd, "w");
    if (pager)
      out = pager;
  }

  if (opts->show_git)
    print_git_banner(out);

  /* print */
  int tree_continues[MAX_DEPTH] = {0};
  enum ftype last_type = FT_COUNT; /* sentinel */

  /* line-2 prefix width when two_line is active (aligns metadata across rows)
   */
  int line2_prefix_w =
      (opts->flat || max_depth_seen == 0) ? 2 : max_depth_seen * 4;

  /*
   * Right-align the time column to the terminal edge. Without this,
   * leaf rows (no col_b, no col_size) leave a gap where the reserved
   * slots stay empty, pushing the time column oddly inward.
   */
  int meta_content = 10; /* perms */
  if (w_owner > 0)
    meta_content += 2 + w_owner;
  if (opts->show_git)
    meta_content += (w_git > 0) ? 2 + w_git : 3;
  meta_content += 2 + w_col_a;
  if (w_col_b > 0)
    meta_content += 2 + w_col_b;
  if (w_col_size > 0)
    meta_content += 2 + w_col_size;

  int time_field = 2 + w_time + 4; /* "  " + time + " ago" */
  /* single-line writes "  " before perms; meta_content starts at perms (10),
   * so subtract that separator here or the row overflows by 2 cols. */
  int single_time_pad = term_w - w_name - 2 - meta_content - time_field;
  int two_time_pad = term_w - line2_prefix_w - meta_content - time_field;
  if (single_time_pad < 0)
    single_time_pad = 0;
  if (two_time_pad < 0)
    two_time_pad = 0;

  for (int i = 0; i < rl->count; i++) {
    const row_t *r = &rl->rows[i];

    /* type group header when -t is active */
    if (opts->group_type && r->depth == 0 && r->type != last_type) {
      if (last_type != FT_COUNT)
        fprintf(out, "\n");
      fprintf(out, "── %s ──\n", ftype_labels[r->type]);
      last_type = r->type;
    }

    if (!opts->flat) {
      if (r->depth > 0)
        tree_continues[r->depth - 1] = !r->is_last;

      /* tree prefix on line 1 */
      for (int d = 0; d < r->depth; d++) {
        if (d == r->depth - 1)
          fprintf(out, "%s ", r->is_last ? "└─" : "├─");
        else
          fprintf(out, "%s   ", tree_continues[d] ? "│" : " ");
      }
    }

    int indent_cols = opts->flat ? 0 : INDENT_COLS(r->depth);

    if (two_line) {
      /* line 1: name only (allowed to use remaining width) */
      fprintf(out, "%s\n", r->name);

      /* line 2: tree continuation + metadata */
      int written = 0;
      if (opts->flat || r->depth == 0) {
        fprintf(out, "  ");
        written = 2;
      } else {
        for (int d = 0; d < r->depth; d++) {
          fprintf(out, "%s   ", tree_continues[d] ? "│" : " ");
          written += 4;
        }
      }
      /* pad to uniform line2_prefix_w so metadata aligns */
      if (written < line2_prefix_w)
        fprintf(out, "%*s", line2_prefix_w - written, "");

      fprintf(out, "%s", r->perms);
      if (w_owner > 0)
        fprintf(out, "  %-*s", w_owner, r->owner);
      if (opts->show_git) {
        if (w_git > 0)
          fprintf(out, "  %*s", w_git, r->git);
        else
          fprintf(out, "   ");
      }
      fprintf(out, "  %*s", w_col_a, r->col_a);
      if (w_col_b > 0)
        print_col_b(out, r, w_col_b);
      if (w_col_size > 0)
        fprintf(out, "  %*s", w_col_size, r->col_size);
      if (r->is_ignored)
        fprintf(out, "\n");
      else
        fprintf(out, "%*s  %*s ago\n", two_time_pad, "", w_time, r->time);

      continue;
    }

    /* single-line layout */

    /* name */
    int name_pad = w_name - indent_cols - r->name_len;
    fprintf(out, "%s%*s", r->name, name_pad > 0 ? name_pad : 0, "");

    /* permissions */
    fprintf(out, "  %s", r->perms);

    /* owner (only shown if any entry has a different owner) */
    if (w_owner > 0)
      fprintf(out, "  %-*s", w_owner, r->owner);

    /* git status */
    if (opts->show_git) {
      if (w_git > 0)
        fprintf(out, "  %*s", w_git, r->git);
      else
        fprintf(out, "   ");
    }

    /* col_a: files count or size */
    fprintf(out, "  %*s", w_col_a, r->col_a);

    /* col_b: dirs count, or truncated symlink target */
    if (w_col_b > 0)
      print_col_b(out, r, w_col_b);

    /* subtree size for dirs */
    if (w_col_size > 0)
      fprintf(out, "  %*s", w_col_size, r->col_size);

    /* time */
    if (r->is_ignored)
      fprintf(out, "\n");
    else
      fprintf(out, "%*s  %*s ago\n", single_time_pad, "", w_time, r->time);
  }

  if (opts->show_git)
    print_git_footer(out);

  if (pager)
    pclose(pager);
}

static void print_all(const rowlist_t *rl, const options_t *opts) {
  if (rl->count == 0)
    return;

  if (isatty(STDOUT_FILENO))
    print_pretty(rl, opts);
  else
    print_tsv(rl, opts);
}

/* ── main ──────────────────────────────────────────────────────── */

static void usage(int code) {
  FILE *out = code == 0 ? stdout : stderr;
  fprintf(out, "usage: dl [-a] [-d depth] [-f] [-g] [-G] [-t] [directory ...]\n"
               "\n"
               "  -a        show dotfiles\n"
               "  -d N      depth (default: 2, min: 1)\n"
               "  -f        flat list, no tree, no recurse\n"
               "  -g        show git status column\n"
               "  -G        also hide .gitignore'd entries\n"
               "  -t        group by file type\n"
               "  -h        show this help\n");
  exit(code);
}

int main(int argc, char **argv) {
  options_t opts = {
      .depth = DEFAULT_DEPTH,
      .show_all = 0,
      .flat = 0,
      .show_git = 0,
      .use_gitignore = 0,
      .group_type = 0,
  };

  int ch;
  while ((ch = getopt(argc, argv, "ad:fgGht")) != -1) {
    switch (ch) {
    case 'a':
      opts.show_all = 1;
      break;
    case 'd': {
      char *end;
      long n = strtol(optarg, &end, 10);
      if (end == optarg || *end != '\0' || n < 1 || n > MAX_DEPTH) {
        fprintf(stderr, "dl: -d requires an integer in 1..%d, got: %s\n",
                MAX_DEPTH, optarg);
        exit(1);
      }
      opts.depth = (int)n;
      break;
    }
    case 'f':
      opts.flat = 1;
      break;
    case 'g':
      opts.show_git = 1;
      break;
    case 'G':
      opts.use_gitignore = 1;
      break;
    case 't':
      opts.group_type = 1;
      break;
    case 'h':
      usage(0);
    default:
      usage(1);
    }
  }

  argc -= optind;
  argv += optind;

  now = time(NULL);
  current_uid = getuid();

  rowlist_t rl;
  rowlist_init(&rl);

  int root_nf, root_nd;
  off_t root_sz;

  if (argc == 0) {
    opts.target_path = ".";
    if (opts.show_git)
      git_map_init(".");
    if (opts.use_gitignore)
      gitignore_init(".");
    collect(AT_FDCWD, ".", 0, opts.depth, &opts, &rl, "", &root_nf, &root_nd,
            &root_sz);
    print_all(&rl, &opts);
  } else {
    for (int i = 0; i < argc; i++) {
      int fd = open(argv[i], O_RDONLY | O_DIRECTORY);
      if (fd < 0) {
        fprintf(stderr, "dl: %s: %s\n", argv[i], strerror(errno));
        continue;
      }
      opts.target_path = argv[i];
      if (opts.show_git)
        git_map_init(argv[i]);
      if (opts.use_gitignore)
        gitignore_init(argv[i]);

      rowlist_reset(&rl);
      collect(fd, ".", 0, opts.depth, &opts, &rl, "", &root_nf, &root_nd,
              &root_sz);
      close(fd);

      if (argc > 1) {
        if (i > 0)
          printf("\n");
        printf("%s:\n", argv[i]);
      }
      print_all(&rl, &opts);
    }
  }

  rowlist_free(&rl);

  return 0;
}
