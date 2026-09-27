/*
 * swc new + locating the swc install.
 *
 * otonomy.ai
 */

#ifndef SWARMRT_NEW_H
#define SWARMRT_NEW_H

/* Absolute path of the running swc binary. argv[0] is only a name when swc
 * was found on PATH ("swc"), and every install-relative lookup (headers,
 * libswarmrt.a, lib/, templates/) used to resolve against the current
 * directory then. Falls back to argv0 when nothing better is known. The
 * result is static storage. */
const char *sw_swc_self_path(const char *argv0);

/* `swc new <name> [--template <t>]`: create a project from
 * <install>/templates/<t> (default "agent"). Returns an exit code. */
int sw_new_main(int argc, char **argv, const char *swc_path);

#endif /* SWARMRT_NEW_H */
