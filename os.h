#ifndef OS
#define OS

#include <sys/mman.h>
#include "block_header.h"

class os
{
public:
    // returns a new block page allocated via mmap.
    static block_header *allocate_block_page(size_t size)
    {
        char *limit = mmap_alloc(size);
        block_header *bl = reinterpret_cast<block_header *>(limit);
        bl->init(size);
        return bl;
    }

    static char *mmap_alloc(size_t s)
    {
        void *limit = ::mmap(0, s, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if (!limit)
            throw std::bad_alloc();
        return static_cast<char *>(limit);
    }

    static void mmap_free(void *pos, size_t s)
    {
        ::munmap(pos, s);
    }
};

#endif