#include <errno.h>

#include "atomic.h"
#include "threads_impl.h"

int pthread_spin_trylock(pthread_spinlock_t* s) { return a_cas_shim(s, 0, EBUSY); }
