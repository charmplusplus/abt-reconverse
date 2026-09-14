/*
 * abt-reconverse: an Argobots (ABT) C ABI implemented on top of Reconverse.
 * Internal header included first by every .cpp.
 */
#ifndef ABTI_H_INCLUDED
#define ABTI_H_INCLUDED

#if !defined(__cplusplus) || __cplusplus < 201703L
#error "abt-reconverse must be compiled as C++17"
#endif

#include "abt.h"      /* the public C ABI; already wrapped in extern "C" */
#include "converse.h" /* the Reconverse runtime this shim is built on */

#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

/* ---- debug trace: ABT_RECONVERSE_DEBUG=1 prints key runtime events to stderr ---- */
extern int ABTI_debug;
#define ABTI_DBG(...) do { if (ABTI_debug) { fprintf(stderr, "[abt pe%d] ", CmiIsPeThread() ? CmiMyRank() : -1); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- unimplemented-call bookkeeping (kept from the skeleton) ---- */
int ABTI_note_unimplemented(const char *name) noexcept;
const char *ABTI_first_unimplemented(void) noexcept;
unsigned long ABTI_unimplemented_count(void) noexcept;
#define ABTI_UNIMPLEMENTED(name) return ABTI_note_unimplemented(name)

/* ---- objects ------------------------------------------------------------
 * Handles are pointers to these structs (ABT_pool = struct ABT_pool_opaque*,
 * cast both ways). A thread's token (a Converse message) is what pools hold
 * conceptually; we store the ABTI_thread* and fetch the token at pop time. */

struct ABTI_thread;
struct ABTI_sched;
struct ABTI_xstream;

struct ABTI_pool {
  ABT_pool_access access;
  ABT_pool_kind kind;
  ABT_bool automatic;
  uint64_t id;
  bool user;                  /* user-defined (ABT_pool_def) pool */
  ABT_pool_def def;           /* copy of the user's definition */
  void *data;                 /* ABT_pool_set_data */
  /* built-in storage: one mutex + deque for every access mode (Argobots
   * likewise uses a single spinlocked implementation for all shared modes) */
  std::mutex m;
  std::deque<ABTI_thread *> q;
  /* waiting pops (basic_wait idle hook): signaled by every push */
  std::condition_variable cv;
  std::atomic<int> waiters{0};
  std::atomic<long> num_blocked{0}; /* ULTs of this pool currently blocked */
  std::atomic<int> num_scheds{0};   /* schedulers holding this pool */
  /* PEs parked in CsdIdleWait waiting for this pool (basic_wait idle hook) */
  std::mutex sm;
  std::vector<int> sleepers;
  void add_sleeper(int rank);
  void remove_sleeper(int rank);

  void push(ABTI_thread *t);            /* legal from any thread */
  ABTI_thread *pop();                   /* NULL when empty */
  ABTI_thread *pop_timedwait(double abs_deadline);
  size_t size();
};

struct ABTI_sched {
  ABT_sched_predef predef;
  bool user_def;              /* ABT_sched_create with an ABT_sched_def (unsupported) */
  std::vector<ABTI_pool *> pools;
  ABT_bool automatic;
  int event_freq;
  ABTI_xstream *used_by;      /* the xstream running this scheduler, or NULL */
  void *data;
  std::atomic<int> request{0}; /* ABTI_SCHED_REQ_* */
  struct PrioCtx { ABTI_sched *sched; int idx; };
  std::vector<PrioCtx> prio_ctx; /* poll contexts for ABT_SCHED_PRIO tables */
  /* user-defined scheduler (ABT_sched_create with an ABT_sched_def): its run
   * loop executes on a runner ULT that owns the PE; ULTs it resumes come
   * back to it through ABTI_choose_fn */
  ABT_sched_def def;
  ABT_sched_config config;
  CthThread runner;
};
enum { ABTI_SCHED_REQ_FINISH = 1, ABTI_SCHED_REQ_EXIT = 2 };

struct ABTI_xstream {
  int rank;                   /* == reconverse PE rank */
  std::atomic<int> state;     /* ABT_xstream_state */
  ABTI_sched *main_sched;
  bool primary;
  int cpubind;                /* -1 = unset */
  std::vector<int> affinity;
  pthread_t thread{};         /* the PE pthread, recorded by the lease handler */
  std::atomic<int> thread_known{0};
  /* join: the PE's idle hook finishes the lease once pools drain */
  std::atomic<int> finishing{0};
  std::atomic<int> finished{0};
  std::mutex jm;
  std::vector<CthThread> joiners;
};

enum ABTI_thread_type { ABTI_THREAD_PRIMARY, ABTI_THREAD_NAMED, ABTI_THREAD_DETACHED, ABTI_THREAD_SCHED };

struct ABTI_thread_attr {
  size_t stacksize;           /* 0 = default */
  void *stackaddr;
  ABT_bool migratable;
};

struct ABTI_thread {
  CthThread cth;
  ABTI_pool *pool;            /* associated pool ("last pool") */
  ABT_unit unit;              /* unit in a user pool, else NULL */
  ABTI_thread_type type;
  void (*fn)(void *);
  void *arg;
  ABTI_thread_attr attr;
  uint64_t id;
  ABTI_xstream *last_xstream;
  ABTI_pool *migrate_to;      /* deferred ABT_thread_migrate_to_pool */
  std::atomic<int> terminated{0};
  std::mutex jm;
  std::vector<CthThread> joiners;
  std::vector<void *> keys;   /* ABT_key values, indexed by key id */
  bool freed_by_exit;         /* detached: struct deleted in the exit fn */
  bool is_task;               /* created by ABT_task_create: a tasklet, run as a small ULT */
  ABTI_pool *blocked_pool;    /* pool whose num_blocked this thread holds while BLOCKED */
  std::atomic<int> blocked_counted{0};
  std::atomic<int> tstate{0}; /* tasklets (cth == NULL): CTH_STATE_READY/RUNNING/TERMINATED */
  std::jmp_buf exit_jmp;      /* ABT_thread_exit longjmps back to the entry frame */
};

struct ABTI_key {
  void (*destructor)(void *);
  int id;
};

/* ---- global state (init.cpp) ---- */
struct ABTI_global {
  std::atomic<int> initialized{0};
  int num_pes;                          /* == max xstreams */
  size_t default_stacksize;
  std::mutex xm;
  std::vector<ABTI_xstream *> xstreams; /* indexed by rank; NULL = free */
  ABTI_xstream *primary_xstream;
  ABTI_thread *primary_thread;
  std::atomic<uint64_t> next_id{1};
  std::atomic<int> next_key{0};
  /* user-pool units -> threads (Argobots keeps the same map) */
  std::mutex um;
  std::unordered_map<void *, ABTI_thread *> units;
  /* reconverse handler indices (same on every PE; first registrant records,
   * the others verify -- workers register before rank 0 does) */
  std::atomic<int> lease_handler{-1};
  std::atomic<int> affinity_handler{-1};
  std::atomic<int> run_handler{-1};
  std::atomic<int> release_handler{-1};
};
extern ABTI_global *ABTI_g;
extern thread_local ABTI_xstream *ABTI_tls_xstream; /* per PE */
extern thread_local CthThread ABTI_tls_runner;       /* the user scheduler running on this PE, or NULL */
extern thread_local ABTI_thread *ABTI_tls_task;      /* the tasklet running inline on this PE, or NULL */
CthThread ABTI_choose_fn(void);                      /* CthThFn for shim ULTs */
void ABTI_start_runner(ABTI_sched *s);               /* on the PE that will run it */
void ABTI_xstream_release(ABTI_xstream *xs);         /* on the PE: give the lease back */

/* ---- helpers ---- */
inline bool ABTI_initialized() { return ABTI_g && ABTI_g->initialized.load(std::memory_order_acquire); }
#define ABTI_CHECK_INITIALIZED() do { if (!ABTI_initialized()) return ABT_ERR_UNINITIALIZED; } while (0)
#define ABTI_CHECK_NULL(p, err) do { if (ABTI_is_null_handle((const void *)(p))) return (err); } while (0)

/* With ABT_NULL == 0 (this header), the ABT_*_NULL handles are small
 * non-zero integers (0x01..0x13), so every handle-to-object conversion must
 * treat both NULL and those sentinels as "no object". */
inline bool ABTI_is_null_handle(const void *h) { return reinterpret_cast<uintptr_t>(h) < 0x100; }
template <class T, class H> inline T *ABTI_obj(H h) { return ABTI_is_null_handle(h) ? nullptr : reinterpret_cast<T *>(h); }
inline ABTI_pool *ABTI_pool_get(ABT_pool h) { return ABTI_obj<ABTI_pool>(h); }
inline ABT_pool ABTI_pool_handle(ABTI_pool *p) { return p ? reinterpret_cast<ABT_pool>(p) : ABT_POOL_NULL; }
inline ABTI_sched *ABTI_sched_get(ABT_sched h) { return ABTI_obj<ABTI_sched>(h); }
inline ABT_sched ABTI_sched_handle(ABTI_sched *s) { return s ? reinterpret_cast<ABT_sched>(s) : ABT_SCHED_NULL; }
inline ABTI_xstream *ABTI_xstream_get(ABT_xstream h) { return ABTI_obj<ABTI_xstream>(h); }
inline ABT_xstream ABTI_xstream_handle(ABTI_xstream *x) { return x ? reinterpret_cast<ABT_xstream>(x) : ABT_XSTREAM_NULL; }
inline ABTI_thread *ABTI_thread_get(ABT_thread h) { return ABTI_obj<ABTI_thread>(h); }
inline ABT_thread ABTI_thread_handle(ABTI_thread *t) { return t ? reinterpret_cast<ABT_thread>(t) : ABT_THREAD_NULL; }
inline ABTI_thread_attr *ABTI_attr_get(ABT_thread_attr h) { return ABTI_obj<ABTI_thread_attr>(h); }
inline ABTI_key *ABTI_key_get(ABT_key h) { return ABTI_obj<ABTI_key>(h); }

/* the calling context */
inline bool ABTI_on_pe() { return CmiIsPeThread() != 0; }
/* the ABT thread running on this PE, or NULL (external pthread, or a
 * reconverse-internal thread such as a scheduler standin) */
inline ABTI_thread *ABTI_self_thread() {
  if (!ABTI_on_pe()) return nullptr;
  if (ABTI_tls_task) return ABTI_tls_task; /* a tasklet runs on the poller's stack */
  return reinterpret_cast<ABTI_thread *>(CthGetUserData(CthSelf()));
}
/* a ULT that can suspend (a tasklet or an external thread cannot) */
inline bool ABTI_can_block(ABTI_thread *t) { return t && t->cth != nullptr && !t->is_task; }

/* init.cpp: the PE count ABT_init uses (ABT_MAX_NUM_XSTREAMS + 1, or a generous default) */
int ABTI_num_pes_rule();

/* pool.cpp */
ABTI_pool *ABTI_pool_create_builtin(ABT_pool_kind kind, ABT_pool_access access, ABT_bool automatic);
void ABTI_pool_destroy(ABTI_pool *p);
void ABTI_pool_associate(ABTI_thread *t, ABTI_pool *p); /* set t->pool (+ unit in user pools) */
void ABTI_pool_disassociate(ABTI_thread *t);
int ABTI_poll_pool(void *ctx);                          /* CsdPollFn */
int ABTI_poll_pool_prio(void *ctx);                     /* CsdPollFn: strict priority, ctx = ABTI_sched::PrioCtx* */
void ABTI_pool_run_thread(ABTI_thread *t);              /* resume a popped thread on this PE */

/* thread.cpp */
void ABTI_thread_awaken_fn(CthThread cth, void *arg);   /* CthAwakenArgFn */
void ABTI_thread_block(ABTI_thread *self, CthVoidFn after, void *arg); /* blocked count + CthSuspendBlocked */
int ABTI_thread_join_impl(ABTI_thread *t);
void ABTI_thread_destroy(ABTI_thread *t);
void ABTI_thread_terminated(ABTI_thread *t); /* publish TERMINATED, wake joiners, free if detached */
ABTI_thread *ABTI_thread_wrap_primary(CthThread cth, ABTI_pool *pool);
ABTI_thread *ABTI_thread_wrap_runner(CthThread cth, ABTI_pool *pool); /* a user scheduler's loop thread, pinned to its PE */

/* sched.cpp */
ABTI_sched *ABTI_sched_create_predef(ABT_sched_predef predef, int num_pools, ABT_pool *pools, int *err);
void ABTI_sched_destroy(ABTI_sched *s);
CsdSchedTable ABTI_sched_build_table(ABTI_sched *s);

/* xstream.cpp */
int ABTI_xstream_lease(ABTI_sched *sched, int want_rank, ABTI_xstream **out);
/* run fn(arg) on a PE (rank 1 if it exists) and wait for it; for callers
 * that are not PE threads and need per-PE runtime state (CthCreate) */
void ABTI_run_on_pe(void (*fn)(void *), void *arg);
void ABTI_xstream_idle_hook(void *);                    /* CcdCondFn, registered per PE */
void ABTI_register_handlers();                          /* same order on every PE */
void ABTI_xstream_install(ABTI_xstream *xs);            /* table + lease message */

#endif /* ABTI_H_INCLUDED */
