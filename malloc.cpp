#include "thread.h"

void *operator new(size_t s)
{
    return thread_allocator::get()->alloc(s);
}

void operator delete(void *s)
{
    return thread_allocator::get()->free(reinterpret_cast<char *>(s));
}

char *gc_malloc(int s)
{
    return thread_allocator::get()->alloc(s);
}

void gc_free(char *s)
{
    return thread_allocator::get()->free(s);
}