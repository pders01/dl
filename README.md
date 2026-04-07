# dl — dense list

A project directory scanner written in C. Shows a shallow tree with file counts, sizes, permissions, and relative timestamps — everything you need to grok a project at a glance.

Not a replacement for `ls`. A complementary view for when you want to scan a project directory fast.

## Usage

```
dl [options] [directory ...]
```

## Options

- `-a` — show dotfiles
- `-d N` — tree depth (default: 2)
- `-f` — flat list, no tree, no recursion
- `-g` — show git status column (M/A/?/D)
- `-G` — hide .gitignore'd entries (not yet implemented)
- `-t` — group files by type (source, config, doc, build, data, media, archive)
- `-h` — help

## Output

```
build/       drwxr-xr-x  ignored
src/         drwxr-xr-x  3 files  1 dirs  9.02K   7m ago
├─ include/  drwxr-xr-x  4 files          3.03K   5d ago
├─ main.c    -rw-r--r--    3.32K                  7m ago
├─ parse.c   -rw-r--r--    1.76K                  2d ago
└─ util.c    -rw-r--r--     940B                  5d ago
Makefile     -rw-r--r--     320B                  7m ago
README.md    -rw-r--r--    1.17K                  5d ago
```

In a terminal, tree lines render as `├─`, `└─`, and `│`.

Columns: name, permissions, owner (only if different), files/size, dirs, subtree size, relative time.

## Pipe output

When stdout is not a TTY, `dl` outputs tab-separated fields for easy processing with `awk`, `cut`, `sort`, etc.

```
dl src/ | awk -F'\t' '{print $1, $4}'
dl . | cut -f1,4 | sort -t$'\t' -k2
```

## Ignored directories

The following directories are collapsed by default: `.git`, `node_modules`, `__pycache__`, `.venv`, `venv`, `dist`, `build`, `target`, `_build`, `deps`, `.cache`, `.next`, `.nuxt`, and others.

## Pager

When output exceeds terminal height, `dl` pages through `$PAGER` (defaults to `less -R`). Piped output is never paged.

## Build

```
make
make install          # installs to /usr/local/bin
make install PREFIX=~/.local
```

Requires a C11 compiler. No dependencies beyond POSIX.

## License

MIT
