#ifndef BLOCK_LIST
#define BLOCK_LIST

#include "block_header.h"
#include <unistd.h>
class thread_allocator;

/**
 * 二级缓存
 */
class block_list
{
    friend class thread_allocator;

public:
    block_list() : _free_list(nullptr) {}

    bool empty()
    {
        return _free_list == nullptr;
    }

    void push(block_header *h)
    {
        block_header::queue_state &qs = h->init_as_queue_node();
        qs.next = _free_list;
        if (_free_list)
        {
            _free_list->as_queue_node().prev = h;
        }
        _free_list = h;
    }

    block_header *pop()
    {
        if (empty)
            return nullptr;

        block_header *head = _free_list;

        block_header *h = _free_list->as_queue_node().next;
        if (h)
            h->as_queue_node().prev = nullptr;
        _free_list = h;

        return head;
    }

    void remove(block_header *h)
    {
        block_header::queue_state &node = h->as_queue_node();
        block_header *prev_header = node.prev;
        block_header *next_header = node.next;

        if (prev_header == nullptr) //head node
        {
            _free_list = next_header;
        }
        else
        {
            prev_header->as_queue_node().next = next_header;
            if (next_header)
                next_header->as_queue_node().prev = prev_header;
        }
    }

    block_header *peek()
    {
        return _free_list;
    }

protected:
    block_header *_free_list;
};

template <size_t pop_size>
class fixed_block_list : public block_list
{
    block_header *pop()
    {
        block_header *head = block_list::pop();

        if (head && head->size() > pop_size)
            push(head->split_after(pop_size));

        return head;
    }

    block_header *pop_chunk()
    {
        return block_list::pop();
    }
};

#endif