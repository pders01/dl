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

#define DEFAULT_DEPTH    2
#define MAX_DEPTH       64
#define INITIAL_ROWS  1024
#define GIT_MAP_SIZE  4096

/* Hardcoded ignore list — always collapsed/hidden */
static const char *ignored_dirs[] = {
	".git", "node_modules", "__pycache__", ".tox", ".mypy_cache",
	".pytest_cache", ".venv", "venv", ".eggs", "*.egg-info",
	".next", ".nuxt", ".output", "dist", "build", ".cache",
	".parcel-cache", ".turbo", "target",       /* rust */
	"_build",                                   /* elixir/erlang */
	"deps",                                     /* elixir mix */
	".zig-cache", "zig-out",                    /* zig */
	NULL
};

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
	[FT_DIR]     = "directories",
	[FT_SOURCE]  = "source",
	[FT_CONFIG]  = "config",
	[FT_DOC]     = "docs",
	[FT_BUILD]   = "build",
	[FT_DATA]    = "data",
	[FT_MEDIA]   = "media",
	[FT_ARCHIVE] = "archive",
	[FT_OTHER]   = "other",
};

static int has_ext(const char *name, const char *ext)
{
	size_t nlen = strlen(name);
	size_t elen = strlen(ext);
	if (nlen <= elen) return 0;
	return strcasecmp(name + nlen - elen, ext) == 0;
}

static enum ftype classify(const char *name, int is_dir)
{
	if (is_dir) return FT_DIR;

	/* exact name matches */
	if (strcasecmp(name, "Makefile") == 0 ||
	    strcasecmp(name, "Dockerfile") == 0 ||
	    strcasecmp(name, "Justfile") == 0 ||
	    strcasecmp(name, "Taskfile") == 0 ||
	    strcasecmp(name, "Vagrantfile") == 0 ||
	    strcasecmp(name, "Rakefile") == 0 ||
	    strcasecmp(name, "Gemfile") == 0 ||
	    strcasecmp(name, "Procfile") == 0 ||
	    strcmp(name, ".editorconfig") == 0 ||
	    strcmp(name, ".gitignore") == 0 ||
	    strcmp(name, ".gitattributes") == 0 ||
	    strcmp(name, ".dockerignore") == 0)
		return FT_CONFIG;

	if (strcasecmp(name, "LICENSE") == 0 ||
	    strcasecmp(name, "LICENCE") == 0 ||
	    strcasecmp(name, "AUTHORS") == 0 ||
	    strcasecmp(name, "CONTRIBUTORS") == 0 ||
	    strcasecmp(name, "CHANGELOG") == 0 ||
	    strcasecmp(name, "CHANGES") == 0 ||
	    strcasecmp(name, "CONTRIBUTING") == 0)
		return FT_DOC;

	/* extension matches */
	/* source */
	static const char *src_exts[] = {
		".c", ".h", ".go", ".rs", ".py", ".js", ".ts", ".tsx",
		".jsx", ".java", ".rb", ".ex", ".exs", ".erl", ".hrl",
		".zig", ".swift", ".kt", ".kts", ".lua", ".sh", ".zsh",
		".bash", ".fish", ".pl", ".pm", ".php", ".cs", ".cpp",
		".cc", ".cxx", ".hpp", ".hxx", ".m", ".mm", ".scala",
		".clj", ".cljs", ".hs", ".ml", ".mli", ".r", ".jl",
		".dart", ".v", ".sv", ".nim", ".cr", ".d", ".f90",
		".f95", ".asm", ".s", ".sql", ".graphql", ".gql",
		".proto", ".thrift", ".vue", ".svelte", ".elm",
		NULL
	};
	for (int i = 0; src_exts[i]; i++)
		if (has_ext(name, src_exts[i])) return FT_SOURCE;

	/* config */
	static const char *cfg_exts[] = {
		".json", ".yaml", ".yml", ".toml", ".ini", ".cfg",
		".conf", ".env", ".xml", ".plist", ".properties",
		".eslintrc", ".prettierrc", ".babelrc",
		NULL
	};
	for (int i = 0; cfg_exts[i]; i++)
		if (has_ext(name, cfg_exts[i])) return FT_CONFIG;

	/* doc */
	static const char *doc_exts[] = {
		".md", ".txt", ".rst", ".adoc", ".org", ".tex",
		".man", ".1", ".2", ".3", ".5", ".8",
		NULL
	};
	for (int i = 0; doc_exts[i]; i++)
		if (has_ext(name, doc_exts[i])) return FT_DOC;

	/* build artifacts */
	static const char *bld_exts[] = {
		".o", ".a", ".so", ".dylib", ".exe", ".dll", ".wasm",
		".class", ".pyc", ".pyo", ".beam", ".lock", ".sum",
		NULL
	};
	for (int i = 0; bld_exts[i]; i++)
		if (has_ext(name, bld_exts[i])) return FT_BUILD;

	/* data */
	static const char *dat_exts[] = {
		".csv", ".tsv", ".parquet", ".sqlite", ".db",
		".ndjson", ".jsonl", ".avro",
		NULL
	};
	for (int i = 0; dat_exts[i]; i++)
		if (has_ext(name, dat_exts[i])) return FT_DATA;

	/* media */
	static const char *med_exts[] = {
		".png", ".jpg", ".jpeg", ".gif", ".svg", ".ico",
		".webp", ".bmp", ".tiff", ".mp3", ".mp4", ".wav",
		".ogg", ".flac", ".avi", ".mkv", ".mov", ".pdf",
		NULL
	};
	for (int i = 0; med_exts[i]; i++)
		if (has_ext(name, med_exts[i])) return FT_MEDIA;

	/* archive */
	static const char *arc_exts[] = {
		".tar", ".gz", ".tgz", ".zip", ".bz2", ".xz",
		".7z", ".rar", ".zst", ".lz4",
		NULL
	};
	for (int i = 0; arc_exts[i]; i++)
		if (has_ext(name, arc_exts[i])) return FT_ARCHIVE;

	return FT_OTHER;
}

/* ── types ─────────────────────────────────────────────────────── */

typedef struct {
	char name[300];     /* display name with suffix */
	char perms[12];     /* drwxrwxrwx */
	char owner[32];     /* username if differs from current user, else empty */
	char col_a[32];     /* "N files" for dirs, size for files */
	char col_b[32];     /* "M dirs" for dirs, blank for files */
	char col_size[16];  /* total subtree size for dirs, empty for files */
	char git[4];        /* git status char: M, A, ?, D, R, or empty */
	char time[16];      /* relative time */
	enum ftype type;    /* file type category */
	int  depth;
	int  is_last;
	int  is_ignored;
	int  name_len;
	int  owner_len;
	int  col_a_len;
	int  col_b_len;
	int  col_size_len;
	int  git_len;
	int  time_len;
} row_t;

typedef struct {
	row_t *rows;
	int    count;
	int    capacity;
} rowlist_t;

typedef struct {
	int  depth;
	int  show_all;      /* -a: include dotfiles */
	int  flat;          /* -f: flat list, no tree, no recurse */
	int  show_git;      /* -g: show git status column */
	int  use_gitignore; /* -G: also read .gitignore */
	int  group_type;    /* -t: group by file type */
} options_t;

/* ── git status map (simple linear scan, fine for project sizes) ── */

typedef struct {
	char path[512];
	char status;        /* M, A, ?, D, R, C, U */
} git_entry_t;

typedef struct {
	git_entry_t entries[GIT_MAP_SIZE];
	int         count;
	int         active;  /* whether git is available */
	char        root[512]; /* git repo root */
} git_map_t;

/* ── globals ───────────────────────────────────────────────────── */

static time_t now;
static uid_t  current_uid;
static git_map_t git_map;

/* ── helpers ───────────────────────────────────────────────────── */

static int is_ignored(const char *name)
{
	for (int i = 0; ignored_dirs[i]; i++) {
		if (strcmp(name, ignored_dirs[i]) == 0)
			return 1;
	}
	return 0;
}

static void fmt_size(off_t bytes, char *buf, size_t bufsz)
{
	const char *units[] = {"B", "K", "M", "G", "T"};
	int u = 0;
	double sz = (double)bytes;

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

static void fmt_reltime(time_t mtime, char *buf, size_t bufsz)
{
	long diff = (long)(now - mtime);
	if (diff < 0) diff = 0;

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

static void fmt_perms(mode_t mode, char *buf)
{
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

static void git_map_init(void)
{
	memset(&git_map, 0, sizeof(git_map));

	/* find git root */
	FILE *fp = popen("git rev-parse --show-toplevel 2>/dev/null", "r");
	if (!fp) return;

	if (fgets(git_map.root, sizeof(git_map.root), fp) == NULL) {
		pclose(fp);
		return;
	}
	pclose(fp);

	/* strip newline */
	size_t len = strlen(git_map.root);
	if (len > 0 && git_map.root[len - 1] == '\n')
		git_map.root[len - 1] = '\0';

	/* get status */
	fp = popen("git status --porcelain 2>/dev/null", "r");
	if (!fp) return;

	git_map.active = 1;

	char line[1024];
	while (fgets(line, sizeof(line), fp) && git_map.count < GIT_MAP_SIZE) {
		if (strlen(line) < 4) continue;

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

		/* map to single meaningful char */
		switch (ge->status) {
		case '?': ge->status = '?'; break;
		case 'M': ge->status = 'M'; break;
		case 'A': ge->status = 'A'; break;
		case 'D': ge->status = 'D'; break;
		case 'R': ge->status = 'R'; break;
		case 'C': ge->status = 'C'; break;
		case 'U': ge->status = 'U'; break;
		default:  ge->status = '~'; break;
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
static char git_status_for(const char *relpath)
{
	if (!git_map.active) return 0;

	for (int i = 0; i < git_map.count; i++) {
		/* exact match */
		if (strcmp(git_map.entries[i].path, relpath) == 0)
			return git_map.entries[i].status;
		/* directory prefix match (any file under this dir is dirty) */
		size_t rlen = strlen(relpath);
		if (strncmp(git_map.entries[i].path, relpath, rlen) == 0 &&
		    git_map.entries[i].path[rlen] == '/')
			return git_map.entries[i].status;
	}
	return 0;
}

/* ── owner lookup ──────────────────────────────────────────────── */

static const char *owner_name(uid_t uid)
{
	if (uid == current_uid) return NULL;
	struct passwd *pw = getpwuid(uid);
	return pw ? pw->pw_name : "?";
}

/* ── row list management ───────────────────────────────────────── */

static void rowlist_init(rowlist_t *rl)
{
	rl->capacity = INITIAL_ROWS;
	rl->count = 0;
	rl->rows = malloc(rl->capacity * sizeof(row_t));
	if (!rl->rows) {
		fprintf(stderr, "dl: out of memory\n");
		exit(1);
	}
}

static row_t *rowlist_add(rowlist_t *rl)
{
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

static void rowlist_free(rowlist_t *rl)
{
	free(rl->rows);
}

/* ── entry sorting ─────────────────────────────────────────────── */

typedef struct {
	char      name[256];
	mode_t    mode;
	uid_t     uid;
	off_t     size;
	off_t     subtree_size;   /* total bytes under dir */
	time_t    mtime;
	enum ftype type;
	int       is_dir;
	int       is_link;
	int       is_ignored;
	int       nfiles;
	int       ndirs;
	char      link_target[4096];
} entry_t;

/* default sort: dirs first, then alpha */
static int entry_cmp(const void *a, const void *b)
{
	const entry_t *ea = (const entry_t *)a;
	const entry_t *eb = (const entry_t *)b;

	if (ea->is_dir != eb->is_dir)
		return eb->is_dir - ea->is_dir;

	return strcasecmp(ea->name, eb->name);
}

/* type-grouped sort: by type category, then alpha within each */
static int entry_cmp_type(const void *a, const void *b)
{
	const entry_t *ea = (const entry_t *)a;
	const entry_t *eb = (const entry_t *)b;

	if (ea->type != eb->type)
		return (int)ea->type - (int)eb->type;

	return strcasecmp(ea->name, eb->name);
}

/* ── directory scanning ────────────────────────────────────────── */

/*
 * Count children and compute subtree size.
 */
static void scan_children(int parent_fd, const char *name,
                          int *nfiles, int *ndirs, off_t *total_size)
{
	*nfiles = 0;
	*ndirs = 0;
	*total_size = 0;

	int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return;

	DIR *d = fdopendir(fd);
	if (!d) { close(fd); return; }

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;

		struct stat st;
		if (fstatat(fd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
			if (S_ISDIR(st.st_mode)) {
				(*ndirs)++;
				/* recurse for subtree size */
				int sub_nf, sub_nd;
				off_t sub_sz;
				scan_children(fd, de->d_name,
				              &sub_nf, &sub_nd, &sub_sz);
				*total_size += sub_sz;
			} else {
				(*nfiles)++;
				*total_size += st.st_size;
			}
		}
	}

	closedir(d);
}

static void collect(int parent_fd, const char *dirname,
                    int depth, int max_depth,
                    const options_t *opts,
                    rowlist_t *rl,
                    const char *relpath)  /* path relative to start dir */
{
	int fd = openat(parent_fd, dirname, O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		fprintf(stderr, "dl: %s: %s\n", dirname, strerror(errno));
		return;
	}

	int fd2 = dup(fd);
	if (fd2 < 0) { close(fd); return; }

	DIR *d = fdopendir(fd);
	if (!d) { close(fd); close(fd2); return; }

	entry_t *entries = malloc(8192 * sizeof(entry_t));
	if (!entries) { closedir(d); close(fd2); return; }

	int n = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && n < 8192) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;

		if (!opts->show_all && de->d_name[0] == '.')
			continue;

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
			ssize_t len = readlinkat(fd2, de->d_name,
			                         e->link_target,
			                         sizeof(e->link_target) - 1);
			if (len > 0) e->link_target[len] = '\0';
		}

		e->type = classify(de->d_name, e->is_dir);

		if (e->is_dir) {
			e->is_ignored = is_ignored(de->d_name);
			if (!e->is_ignored)
				scan_children(fd2, de->d_name,
				              &e->nfiles, &e->ndirs,
				              &e->subtree_size);
		}

		n++;
	}

	closedir(d);

	if (opts->group_type)
		qsort(entries, n, sizeof(entry_t), entry_cmp_type);
	else
		qsort(entries, n, sizeof(entry_t), entry_cmp);

	for (int i = 0; i < n; i++) {
		entry_t *e = &entries[i];
		row_t *r = rowlist_add(rl);

		r->depth = opts->flat ? 0 : depth;
		r->is_last = (i == n - 1);
		r->is_ignored = e->is_ignored;
		r->type = e->type;

		/* name with type suffix */
		if (e->is_dir)
			r->name_len = snprintf(r->name, sizeof(r->name),
			                       "%s/", e->name);
		else if (e->is_link)
			r->name_len = snprintf(r->name, sizeof(r->name),
			                       "%s@", e->name);
		else if (e->mode & S_IXUSR)
			r->name_len = snprintf(r->name, sizeof(r->name),
			                       "%s*", e->name);
		else
			r->name_len = snprintf(r->name, sizeof(r->name),
			                       "%s", e->name);

		/* permissions */
		fmt_perms(e->mode, r->perms);

		/* owner (only if different from current user) */
		const char *own = owner_name(e->uid);
		if (own) {
			r->owner_len = snprintf(r->owner, sizeof(r->owner),
			                        "%s", own);
		}

		/* col_a: files count OR size */
		if (e->is_dir) {
			if (e->is_ignored) {
				r->col_a_len = snprintf(r->col_a,
				                        sizeof(r->col_a),
				                        "ignored");
			} else if (e->nfiles > 0) {
				r->col_a_len = snprintf(r->col_a,
				                        sizeof(r->col_a),
				                        "%d files", e->nfiles);
			}
		} else {
			fmt_size(e->size, r->col_a, sizeof(r->col_a));
			r->col_a_len = (int)strlen(r->col_a);
		}

		/* col_b: dirs count */
		if (e->is_dir && !e->is_ignored && e->ndirs > 0) {
			r->col_b_len = snprintf(r->col_b, sizeof(r->col_b),
			                        "%d dirs", e->ndirs);
		} else if (e->is_link && e->link_target[0]) {
			r->col_b_len = snprintf(r->col_b, sizeof(r->col_b),
			                        "-> %s", e->link_target);
		}

		/* subtree size for dirs */
		if (e->is_dir && !e->is_ignored && e->subtree_size > 0) {
			fmt_size(e->subtree_size, r->col_size,
			         sizeof(r->col_size));
			r->col_size_len = (int)strlen(r->col_size);
		}

		/* git status */
		if (opts->show_git && !e->is_ignored) {
			char entry_relpath[1024];
			if (relpath[0])
				snprintf(entry_relpath, sizeof(entry_relpath),
				         "%s/%s", relpath, e->name);
			else
				snprintf(entry_relpath, sizeof(entry_relpath),
				         "%s", e->name);

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

		/* recurse (skip in flat mode) */
		if (!opts->flat && e->is_dir && !e->is_ignored &&
		    depth + 1 < max_depth) {
			char child_relpath[1024];
			if (relpath[0])
				snprintf(child_relpath, sizeof(child_relpath),
				         "%s/%s", relpath, e->name);
			else
				snprintf(child_relpath, sizeof(child_relpath),
				         "%s", e->name);

			collect(fd2, e->name, depth + 1, max_depth,
			        opts, rl, child_relpath);
		}
	}

	free(entries);
	close(fd2);
}

/* ── output ────────────────────────────────────────────────────── */

static int get_term_height(void)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
		return ws.ws_row;
	return 24; /* fallback */
}

/*
 * Tab-separated output for piping.
 * Fields: name, perms, owner, git, files_or_size, dirs, subtree_size, time
 */
static void print_tsv(const rowlist_t *rl, const options_t *opts)
{
	/* ignore SIGPIPE — downstream may close early */
	signal(SIGPIPE, SIG_IGN);

	for (int i = 0; i < rl->count; i++) {
		const row_t *r = &rl->rows[i];

		fprintf(stdout, "%s\t%s\t%s",
		        r->name, r->perms, r->owner);

		if (opts->show_git)
			fprintf(stdout, "\t%s", r->git);

		fprintf(stdout, "\t%s\t%s\t%s\t%s\n",
		        r->col_a, r->col_b, r->col_size,
		        r->is_ignored ? "" : r->time);
	}
}

static void print_pretty(const rowlist_t *rl, const options_t *opts)
{
	if (rl->count == 0) return;

	#define INDENT_COLS(d) ((d) == 0 ? 0 : 3 + ((d) - 1) * 4)

	/* compute global column widths */
	int w_name = 0;
	int w_owner = 0;
	int w_col_a = 0;
	int w_col_b = 0;
	int w_col_size = 0;
	int w_git = 0;
	int w_time = 0;

	for (int i = 0; i < rl->count; i++) {
		const row_t *r = &rl->rows[i];
		int col1 = INDENT_COLS(r->depth) + r->name_len;
		if (col1 > w_name) w_name = col1;
		if (r->owner_len > w_owner) w_owner = r->owner_len;
		if (r->col_a_len > w_col_a) w_col_a = r->col_a_len;
		if (r->col_b_len > w_col_b) w_col_b = r->col_b_len;
		if (r->col_size_len > w_col_size) w_col_size = r->col_size_len;
		if (r->git_len > w_git) w_git = r->git_len;
		if (r->time_len > w_time) w_time = r->time_len;
	}

	/* decide whether to page */
	FILE *out = stdout;
	FILE *pager = NULL;

	if (rl->count > get_term_height()) {
		const char *pager_cmd = getenv("PAGER");
		if (!pager_cmd || !pager_cmd[0])
			pager_cmd = "less -R";

		signal(SIGPIPE, SIG_IGN);

		pager = popen(pager_cmd, "w");
		if (pager)
			out = pager;
	}

	/* print */
	int tree_continues[MAX_DEPTH] = {0};
	enum ftype last_type = FT_COUNT; /* sentinel */

	for (int i = 0; i < rl->count; i++) {
		const row_t *r = &rl->rows[i];

		/* type group header when -t is active */
		if (opts->group_type && r->depth == 0 &&
		    r->type != last_type) {
			if (last_type != FT_COUNT)
				fprintf(out, "\n");
			fprintf(out, "── %s ──\n", ftype_labels[r->type]);
			last_type = r->type;
		}

		if (!opts->flat) {
			if (r->depth > 0)
				tree_continues[r->depth - 1] = !r->is_last;

			/* tree prefix */
			for (int d = 0; d < r->depth; d++) {
				if (d == r->depth - 1)
					fprintf(out, "%s ",
					        r->is_last ? "└─" : "├─");
				else
					fprintf(out, "%s   ",
					        tree_continues[d] ? "│" : " ");
			}
		}

		int indent_cols = opts->flat ? 0 : INDENT_COLS(r->depth);

		/* name */
		int name_pad = w_name - indent_cols - r->name_len;
		fprintf(out, "%s%*s", r->name,
		        name_pad > 0 ? name_pad : 0, "");

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

		/* col_b: dirs count */
		if (w_col_b > 0)
			fprintf(out, "  %*s", w_col_b, r->col_b);

		/* subtree size for dirs */
		if (w_col_size > 0)
			fprintf(out, "  %*s", w_col_size, r->col_size);

		/* time */
		if (r->is_ignored)
			fprintf(out, "\n");
		else
			fprintf(out, "  %*s ago\n", w_time, r->time);
	}

	if (pager)
		pclose(pager);
}

static void print_all(const rowlist_t *rl, const options_t *opts)
{
	if (rl->count == 0) return;

	if (isatty(STDOUT_FILENO))
		print_pretty(rl, opts);
	else
		print_tsv(rl, opts);
}

/* ── main ──────────────────────────────────────────────────────── */

static void usage(void)
{
	fprintf(stderr,
		"usage: dl [-a] [-d depth] [-f] [-g] [-G] [-t] [directory ...]\n"
		"\n"
		"  -a        show dotfiles\n"
		"  -d N      depth (default: 2)\n"
		"  -f        flat list, no tree, no recurse\n"
		"  -g        show git status column\n"
		"  -G        also hide .gitignore'd entries\n"
		"  -t        group by file type\n"
		"  -h        show this help\n"
	);
	exit(1);
}

int main(int argc, char **argv)
{
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
		case 'a': opts.show_all = 1; break;
		case 'd': opts.depth = atoi(optarg); break;
		case 'f': opts.flat = 1; break;
		case 'g': opts.show_git = 1; break;
		case 'G': opts.use_gitignore = 1; break;
		case 't': opts.group_type = 1; break;
		case 'h': /* fallthrough */
		default:  usage();
		}
	}

	argc -= optind;
	argv += optind;

	now = time(NULL);
	current_uid = getuid();

	if (opts.show_git)
		git_map_init();

	rowlist_t rl;
	rowlist_init(&rl);

	if (argc == 0) {
		collect(AT_FDCWD, ".", 0, opts.depth, &opts, &rl, "");
	} else {
		for (int i = 0; i < argc; i++) {
			if (argc > 1)
				printf("%s:\n", argv[i]);

			int fd = open(argv[i], O_RDONLY | O_DIRECTORY);
			if (fd < 0) {
				fprintf(stderr, "dl: %s: %s\n",
				        argv[i], strerror(errno));
				continue;
			}
			collect(fd, ".", 0, opts.depth, &opts, &rl, "");
			close(fd);
		}
	}

	print_all(&rl, &opts);
	rowlist_free(&rl);

	return 0;
}
