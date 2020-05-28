#ifndef GARBAGE_COLLECT
#define GARBAGE_COLLECT

#include "block_header.h"
#include "block_list.h"
#include <atomic>

class thread_allocator;

class garbage_collect
{
    friend class thread_allocator;

private:
    std::atomic<block_header *> _gc_at_bat; // where the gc pulls from.
    uint64_t _gc_pad1[7];                   // gc thread and this thread should not false-share these values

    block_header *_gc_on_deck; // where we save frees while waiting on gc to bat.
    uint64_t _gc_pad2[7];      // gc thread and this thread should not false-share these values

    static inline block_list *as_block_list(block_header *h)
    {
        return reinterpret_cast<block_list *>(h);
    }

public:
    void constructor(){
        _gc_at_bat=nullptr;
        memset(_gc_pad1,0,sizeof(_gc_pad1));
        _gc_on_deck=nullptr;
        memset(_gc_pad2,0,sizeof(_gc_pad2));
    }

    void release(block_header *h)
    {
        as_block_list(_gc_on_deck)->push(h);
        if (_gc_at_bat.load() == nullptr)
        {
            _gc_at_bat.store(_gc_on_deck);
            _gc_on_deck = nullptr;
        }
    }

    /** 
    * called by gc thread and pops the at-bat free list
    */
    block_header *get_garbage() // grab a pointer previously claimed.
    {
        if (block_header *garbage = _gc_at_bat.load())
        {
            _gc_at_bat.store(nullptr);
            return garbage;
        }
        return nullptr;
    }
};

#endif