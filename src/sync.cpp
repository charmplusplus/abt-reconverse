/*
 * abt-reconverse: an Argobots (ABT) C ABI implemented on top of Reconverse.
 *
 * GENERATED SKELETON (phase 2, step 1).  Every public Argobots entry point is
 * present as a C symbol; the bodies are stubs that record the call and return
 * ABT_ERR_FEATURE_NA.  Real semantics land in later steps; replace stubs in
 * place, keeping the extern "C" block.
 */

#include "abti.h"

/* Mutexes, condition variables, rwlocks, eventuals, futures, barriers. */

/* Stub bodies deliberately ignore their arguments. */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

extern "C" {

int ABT_mutex_create(ABT_mutex *newmutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_create");
}

int ABT_mutex_create_with_attr(ABT_mutex_attr attr, ABT_mutex *newmutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_create_with_attr");
}

int ABT_mutex_free(ABT_mutex *mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_free");
}

int ABT_mutex_lock(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_lock");
}

int ABT_mutex_lock_high(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_lock_high");
}

int ABT_mutex_lock_low(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_lock_low");
}

int ABT_mutex_trylock(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_trylock");
}

int ABT_mutex_spinlock(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_spinlock");
}

int ABT_mutex_unlock(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_unlock");
}

int ABT_mutex_unlock_se(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_unlock_se");
}

int ABT_mutex_unlock_de(ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_unlock_de");
}

int ABT_mutex_equal(ABT_mutex mutex1, ABT_mutex mutex2, ABT_bool *result)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_equal");
}

int ABT_mutex_get_attr(ABT_mutex mutex, ABT_mutex_attr *attr)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_get_attr");
}

int ABT_mutex_attr_create(ABT_mutex_attr *newattr)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_attr_create");
}

int ABT_mutex_attr_free(ABT_mutex_attr *attr)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_attr_free");
}

int ABT_mutex_attr_set_recursive(ABT_mutex_attr attr, ABT_bool recursive)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_attr_set_recursive");
}

int ABT_mutex_attr_get_recursive(ABT_mutex_attr attr, ABT_bool *recursive)
{
    ABTI_UNIMPLEMENTED("ABT_mutex_attr_get_recursive");
}

int ABT_cond_create(ABT_cond *newcond)
{
    ABTI_UNIMPLEMENTED("ABT_cond_create");
}

int ABT_cond_free(ABT_cond *cond)
{
    ABTI_UNIMPLEMENTED("ABT_cond_free");
}

int ABT_cond_wait(ABT_cond cond, ABT_mutex mutex)
{
    ABTI_UNIMPLEMENTED("ABT_cond_wait");
}

int ABT_cond_timedwait(ABT_cond cond, ABT_mutex mutex, const struct timespec *abstime)
{
    ABTI_UNIMPLEMENTED("ABT_cond_timedwait");
}

int ABT_cond_signal(ABT_cond cond)
{
    ABTI_UNIMPLEMENTED("ABT_cond_signal");
}

int ABT_cond_broadcast(ABT_cond cond)
{
    ABTI_UNIMPLEMENTED("ABT_cond_broadcast");
}

int ABT_rwlock_create(ABT_rwlock *newrwlock)
{
    ABTI_UNIMPLEMENTED("ABT_rwlock_create");
}

int ABT_rwlock_free(ABT_rwlock *rwlock)
{
    ABTI_UNIMPLEMENTED("ABT_rwlock_free");
}

int ABT_rwlock_rdlock(ABT_rwlock rwlock)
{
    ABTI_UNIMPLEMENTED("ABT_rwlock_rdlock");
}

int ABT_rwlock_wrlock(ABT_rwlock rwlock)
{
    ABTI_UNIMPLEMENTED("ABT_rwlock_wrlock");
}

int ABT_rwlock_unlock(ABT_rwlock rwlock)
{
    ABTI_UNIMPLEMENTED("ABT_rwlock_unlock");
}

int ABT_eventual_create(int nbytes, ABT_eventual *neweventual)
{
    ABTI_UNIMPLEMENTED("ABT_eventual_create");
}

int ABT_eventual_free(ABT_eventual *eventual)
{
    ABTI_UNIMPLEMENTED("ABT_eventual_free");
}

int ABT_eventual_wait(ABT_eventual eventual, void **value)
{
    ABTI_UNIMPLEMENTED("ABT_eventual_wait");
}

int ABT_eventual_timedwait(ABT_eventual eventual, void **value, const struct timespec *abstime)
{
    ABTI_UNIMPLEMENTED("ABT_eventual_timedwait");
}

int ABT_eventual_test(ABT_eventual eventual, void **value, ABT_bool *is_ready)
{
    ABTI_UNIMPLEMENTED("ABT_eventual_test");
}

int ABT_eventual_set(ABT_eventual eventual, void *value, int nbytes)
{
    ABTI_UNIMPLEMENTED("ABT_eventual_set");
}

int ABT_eventual_reset(ABT_eventual eventual)
{
    ABTI_UNIMPLEMENTED("ABT_eventual_reset");
}

int ABT_future_create(uint32_t num_compartments, void (*cb_func)(void **arg), ABT_future *newfuture)
{
    ABTI_UNIMPLEMENTED("ABT_future_create");
}

int ABT_future_free(ABT_future *future)
{
    ABTI_UNIMPLEMENTED("ABT_future_free");
}

int ABT_future_wait(ABT_future future)
{
    ABTI_UNIMPLEMENTED("ABT_future_wait");
}

int ABT_future_test(ABT_future future, ABT_bool *is_ready)
{
    ABTI_UNIMPLEMENTED("ABT_future_test");
}

int ABT_future_set(ABT_future future, void *value)
{
    ABTI_UNIMPLEMENTED("ABT_future_set");
}

int ABT_future_reset(ABT_future future)
{
    ABTI_UNIMPLEMENTED("ABT_future_reset");
}

int ABT_barrier_create(uint32_t num_waiters, ABT_barrier *newbarrier)
{
    ABTI_UNIMPLEMENTED("ABT_barrier_create");
}

int ABT_barrier_reinit(ABT_barrier barrier, uint32_t num_waiters)
{
    ABTI_UNIMPLEMENTED("ABT_barrier_reinit");
}

int ABT_barrier_free(ABT_barrier *barrier)
{
    ABTI_UNIMPLEMENTED("ABT_barrier_free");
}

int ABT_barrier_wait(ABT_barrier barrier)
{
    ABTI_UNIMPLEMENTED("ABT_barrier_wait");
}

int ABT_barrier_get_num_waiters(ABT_barrier barrier, uint32_t *num_waiters)
{
    ABTI_UNIMPLEMENTED("ABT_barrier_get_num_waiters");
}


} /* extern "C" */
