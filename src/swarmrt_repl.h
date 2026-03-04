/*
 * SwarmRT REPL: Interactive expression evaluator
 *
 * Usage: swc repl
 *
 * Meta-commands: :help, :quit, :load <file>, :env, :reset
 *
 * otonomy.ai
 */

#ifndef SWARMRT_REPL_H
#define SWARMRT_REPL_H

/* Start the interactive REPL. Returns 0 on clean exit. */
int sw_repl_start(void);

#endif /* SWARMRT_REPL_H */
