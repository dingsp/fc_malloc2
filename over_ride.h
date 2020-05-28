// Used to override malloc routines on systems that define the
// memory allocation routines to be weak symbols in their libc
// (almost all unix-based systems are like this), on gcc, which
// suppports the 'alias' attribute.

#include <stddef.h>
#include <sys/cdefs.h>

#include <new>

// visibility("default") ensures that these symbols are always exported, even
// with -fvisibility=hidden.
#define GCMALLOC_ALIAS(fn) \
    __attribute__((alias(#fn), visibility("default")))

void *operator new(size_t size) GCMALLOC_ALIAS(operator new);
void operator delete(void *p)GCMALLOC_ALIAS(operator delete);

extern "C"
{
    void *malloc(size_t size) GCMALLOC_ALIAS(gc_malloc);
    void free(void *ptr) GCMALLOC_ALIAS(gc_free);
}