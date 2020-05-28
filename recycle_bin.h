#ifndef RECYCLE_BIN
#define RECYCLE_BIN

#include "common.h"
#include "block_header.h"
#include "block_list.h"
#include "ring_buffer.h"

/**
 * @brief 全局缓存，由ringbuffer和二级缓存组成
 */
class recycle_bin
{
public:
    recycle_bin()
        : _read_pos(0), _full(0), _write_pos(0)
    {
        memset(&_free_queue, 0, sizeof(_free_queue));
    }

    // block can be used by thread
    // read the _read_pos without any atomic sync, we only care about an estimate
    int64_t available()
    {
        return _write_pos - *((int64_t *)&_read_pos);
    }

    ////////////////////////////////被本地线程调用///////////////////////////////////
    int64_t claim(int64_t num)
    {
        return _read_pos.fetch_add(num);
    }

    block_header *get_block(int64_t claim_pos)
    {
        return _free_queue.at(claim_pos);
    }

    void clear_block(int64_t claim_pos)
    {
        _free_queue.at(claim_pos) = nullptr;
    }
    //////////////////////////////////////////////////////////////////////////////

    /**
       * determines how many chunks should be required to consider this bin full. 
       * we prefer to store appropriate blocks in the queue.
       * 
       * @return blocks nums the gc should publish, QUEUE_SIZE-1 max
       */
    int64_t check_status()
    {
        int64_t av = available();

        if (av < 0) // imply potential demand
        {
            if (_full == 0)
                _full = 2; // fast increase to expect demand
            else
                _full *= 2;
            _full = std::max(av, _full);
            _full = std::max(_full, (int64_t)QUEUE_SIZE - 1); // insure do not write cover
            _write_pos = claim(1);                            // reset
            return _full;
        }
        else if (av > 0)
        {
            int consumed = _full - av;
            _full--; // slow back off, may be we didn't have enough to produce
            if (_full < 0)
                _full = 0;
            return consumed == 0 ? -1 : consumed;
        }
        else if (av == 0) // _full is just right, keep it
        {
            return _full;
        }
        return 0;
    }

    void clear_cached_block(block_header *h)
    {
        _free_list.remove(h);
    }

    void cache_block(block_header *h)
    {
        _free_list.push(h);
    }

    /**
     * @brief 从缓存中提取内存块，取消可合并状态
     */
    block_header *get_cache_block()
    {
        block_header *h = _free_list.pop();
        //Since the gc thread is single, the operation is safe. Done by others
        h->unset_state(block_header::mergable);
        return h;
    }

    /**
     * @brief 向ring_buffer中生产块
     */
    bool produce_block_to_ring_buffer()
    {
        bool found_work = false;
        int64_t needed = check_status(); // returns the number of chunks need
        if (needed > 0)
        {
            _full_count = 0;

            int64_t next_write_pos = _write_pos;
            block_header *next = get_cache_block();

            while (next && needed > 0)
            {
                //poping block from bin and pushing into queue
                found_work = true;
                ++next_write_pos;
                // skip left things，if the queue was full, it will keep skiping
                if (!_free_queue.at(next_write_pos))
                {
                    _free_queue.at(next_write_pos) = next;
                    next = get_cache_block();
                }
                --needed;
            }

            if (next)
                cache_block(next); // leftover

            _write_pos = next_write_pos;
        }
        else
            _full_count++;

        return found_work;
    }

    void reclaim_ring_buffer()
    {
        if (_full_count > 10000)
        {
            int av = available();
            for (int i = 0; i < av; i++)
            {
                int64_t claim_pos = claim(1);
                if (claim_pos <= av)
                {
                    block_header *h = get_block(claim_pos);
                    if (h)
                    {
                        h->set_state(block_header::mergable); //set state mergable
                        cache_block(h);
                    }
                }
                else
                    break; //other thread has consumed
            }
            _full_count = 0;
        }
    }

    ring_buffer<block_header *, QUEUE_SIZE> _free_queue;
    std::atomic<int64_t> _read_pos; // written to by read thread
    int64_t _pad[7];                // below this point is written to by gc thread

    int64_t _write_pos; // read by consumers to know the last valid entry.

    int64_t _full_count; // gc thread checked and found the queue full, no one want any
    int64_t _full;       // limit the number of blocks kept in queue

    block_list _free_list; // blocks are stored as a double-linked list
};

#endif