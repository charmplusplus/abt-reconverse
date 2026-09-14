/* A process that has finalized Argobots forks; the child initializes Argobots
 * again and runs a ULT on a secondary xstream (the shape of Margo's unit-test
 * fixtures in --no-fork mode: the helper server is forked after an earlier
 * case's margo_finalize).  Native Argobots has no threads left after
 * ABT_finalize, so the child starts from nothing; the shim keeps its PE
 * threads alive across finalize, and a forked child inherits a runtime whose
 * threads do not exist. */
#include <abt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
static void ult(void *a) { *(int *)a = 42; }
static int cycle(const char *who) {
  ABT_xstream xs; ABT_pool pool; ABT_thread t; int v = 0;
  if (ABT_init(0, NULL) != ABT_SUCCESS) { fprintf(stderr, "%s: ABT_init failed\n", who); return 1; }
  if (ABT_xstream_create(ABT_SCHED_NULL, &xs) != ABT_SUCCESS) { fprintf(stderr, "%s: xstream_create failed\n", who); return 1; }
  ABT_xstream_get_main_pools(xs, 1, &pool);
  ABT_thread_create(pool, ult, &v, ABT_THREAD_ATTR_NULL, &t);
  ABT_thread_join(t); ABT_thread_free(&t);
  ABT_xstream_join(xs); ABT_xstream_free(&xs);
  ABT_finalize();
  if (v != 42) { fprintf(stderr, "%s: ULT did not run\n", who); return 1; }
  printf("%s: cycle ok\n", who); fflush(stdout);
  return 0;
}
int main(void) {
  if (cycle("parent#1")) return 1;
  pid_t p = fork();
  if (p == 0) { int rc = cycle("child"); exit(rc); } /* normal exit: the atexit runtime shutdown runs in the child too */
  int st = 0; waitpid(p, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { fprintf(stderr, "child failed (status %d)\n", st); return 1; }
  if (cycle("parent#2")) return 1;
  printf("fork_reinit ok\n");
  return 0;
}
