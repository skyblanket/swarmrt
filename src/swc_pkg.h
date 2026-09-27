/*
 * swc package manager — swarm.json manifest, swarm.lock lockfile, git and
 * path dependencies installed under <project>/.swarm/deps/<name>.
 *
 * See docs/PACKAGES.md for the file formats and the commands.
 *
 * otonomy.ai
 */

#ifndef SWC_PKG_H
#define SWC_PKG_H

#include <stddef.h>

#define SWC_PKG_MANIFEST  "swarm.json"
#define SWC_PKG_LOCKFILE  "swarm.lock"
#define SWC_PKG_DEPS_DIR  ".swarm/deps"

/* Is `cmd` one of the package commands (add, install, deps, update, remove)? */
int swc_pkg_is_command(const char *cmd);

/* Run a package command. argv[1] is the command; argv[2..] its arguments.
 * `lib_dir` is <swarmrt>/lib (used to warn when a dependency's module
 * shadows a bundled one). Returns the process exit code. */
int swc_pkg_main(int argc, char **argv, const char *lib_dir);

/* Nearest ancestor of `start_dir` (inclusive) that holds a swarm.json.
 * Writes its absolute path to `out` and returns 1, or returns 0 if none. */
int swc_pkg_find_root(const char *start_dir, char *out, size_t outsz);

/* The installed dependency package dirs of the project at `root`:
 * "<root>/.swarm/deps/<name>" for each entry, sorted by name. Returns the
 * count and a malloc'd array of malloc'd strings in *out (NULL when 0). */
int swc_pkg_dep_dirs(const char *root, char ***out);

#endif /* SWC_PKG_H */
