/**
 *   Each thread has its own 'arena' where it can allocate 'new' blocks of what ever size it needs (buckets). After
 *   a thread is done with memory it places it in a garbage collection queue.
 *
 *   The garbage collector follows each threads trash bin and moves the blocks into a recycled list that
 *   all other threads can pull from.
 *
 *   The garbage collector can grow these queues as necessary and shrink them as time progresses.
 */
#include <thread>
#include <stdint.h>
#include <memory.h>
#include <stdlib.h>
#include <iostream>
#include <vector>
#include <assert.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <sys/mman.h>

#define CHUNK_SIZE (4 * 1024 * 1024)
#define HEDER_SIZE 8
#define MIN_BLOCK_SIZE (CHUNK_SIZE - 8)
#define ROUNDS 200000
#define LOG2(X) ((unsigned)(8 * sizeof(unsigned long long) - __builtin_clzll((X)) - 1))
#define NUM_BINS 22
#define QUEUE_SIZE 128

char *mmap_alloc(size_t s)
{
   void *limit = ::mmap(0, s, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

   if (!limit)
      throw std::bad_alloc();
   return static_cast<char *>(limit);
}

void mmap_free(void *pos, size_t s)
{
   ::munmap(pos, s);
}

template <typename EventType, uint64_t Size = 1024>
class ring_buffer
{
public:
   typedef EventType event_type;

   static_assert(((Size != 0) && ((Size & (~Size + 1)) == Size)),
                 "Ring buffer's must be a power of 2");

   /** @return a read-only reference to the event at pos */
   const EventType &at(int64_t pos) const
   {
      return _buffer[pos & (Size - 1)];
   }

   /** @return a reference to the event at pos */
   EventType &at(int64_t pos)
   {
      return _buffer[pos & (Size - 1)];
   }

   int64_t get_buffer_index(int64_t pos) const { return pos & (Size - 1); }
   int64_t get_buffer_size() const { return Size; }

private:
   EventType _buffer[Size];
};

//basic block, all the member will be allocated in mmap area
class block_header
{
public:
   //get next block
   block_header *next()
   {
      if (_size > 0)
         return reinterpret_cast<block_header *>(data() + _size);
      else
         return nullptr;
   }

   //get prev block
   block_header *prev()
   {
      if (_prev_size <= 0)
         return nullptr;
      return reinterpret_cast<block_header *>(reinterpret_cast<char *>(this) - _prev_size - 8);
   }

   enum flags_enum
   {
      unknown = 0,
      mergable = 1, // int gc cached, mergable
      bigdata = 2,
   };

   flags_enum get_state() { return (flags_enum)_flags; }

   struct queue_state // the block is serving as a Double linked-list node
   {
      block_header *next;
      block_header *prev;
   };

   void set_state(flags_enum e)
   {
      _flags = e;
   }

   queue_state &as_queue_node()
   {
      return *reinterpret_cast<queue_state *>(data());
   }

   queue_state &init_as_queue_node()
   {
      queue_state &s = as_queue_node();
      s.next = nullptr;
      s.prev = nullptr;
      return s;
   }

   void init(int s)
   {
      _prev_size = 0;
      _size = -(s - 8);
   }

   char *data() { return ((char *)this) + 8; } //return data ptr
   int size() const { return abs(_size); }     //return size of data

   // split a block, create a new block at p and return it
   block_header *split_after(int s)
   {
      block_header *n = reinterpret_cast<block_header *>(data() + s);
      n->_prev_size = s;
      n->_size = size() - s - 8;

      if (_size < 0) //tail block of the page
         n->_size = -n->_size;

      _size = s;
      return n;
   }

   // merge this block with next, return head of new block.
   block_header *merge_next()
   {
      block_header *nxt = next();
      if (!nxt || nxt->_flags != mergable)
         return this;

      //update __size of this
      _size += nxt->size() + 8;
      if (nxt->_size < 0)
         _size = -_size;

      //update _prev_size of next block
      nxt = next();
      if (nxt)
      {
         nxt->_prev_size = size();
      }
      return this;
   }

   // merge this block with the prev, return the head of new block
   block_header *merge_prev()
   {
      block_header *p = prev();
      if (!p)
         return this;
      if (p->_flags != mergable)
         return this;
      return p->merge_next();
   }

private:
   int32_t _prev_size; // offset to previous header.
   int32_t _size : 28; // offset to next, negitive indicates tail, 2 GB max
   int32_t _flags : 4;
};

// returns a new block page allocated via mmap.
block_header *allocate_block_page();

//thread private，all the member will be allocated in mmap area
class thread_allocator
{
   friend class garbage_collector;

public:
   char *alloc(size_t s);

   //put node into _gc_on_deck and put _gc_on_deck onto _gc_at_bat atomic if it is null
   void free(char *c)
   {
      block_header *node = reinterpret_cast<block_header *>(c - 8);
      if (node->size() > MIN_BLOCK_SIZE)
      {
         mmap_free(node, node->size() + 8);
         return;
      }
      node->init_as_queue_node().next = _gc_on_deck;
      if (!_gc_at_bat)
      {
         _gc_at_bat = node; //atomic
         _gc_on_deck = nullptr;
      }
      else
         _gc_on_deck = node;
   }

   static thread_allocator &get()
   {
      static __thread thread_allocator *tld = nullptr;

      if (!tld)
      {
         //TODO:get page from gc
         tld = reinterpret_cast<thread_allocator *>(mmap_alloc(sizeof(thread_allocator)));

         //allocate pthread_threadlocal var, attach a destructor /clean up callback to that variable
         thread_local thread_allocator::thread_allocator_gc tlv;
      }
      return *tld;
   }

private:
   thread_allocator();
   ~thread_allocator();

   void destructor()
   {
      // give the rest of our allocated chunks to the gc thread
      for (int i = 0; i <= NUM_BINS; i++)
      {
         if (_bin_cache[i])
         {
            free(_bin_cache[i]->data());
         }
      }
      _done = 1; //final release by gc via ummap
   }

   block_header *fetch_block_from_bin(int bin);

   //each bin_cache only cache one block to increase the elastic of the system managed memory
   bool store_cache(block_header *h)
   {
      auto bin = LOG2(h->size());
      if (_bin_cache[bin] == nullptr)
      {
         _bin_cache[bin] = h;
         return true;
      }
      return false;
   }

   block_header *fetch_cache(int bin)
   {
      if (_bin_cache[bin])
      {
         block_header *b = _bin_cache[bin];
         _bin_cache[bin] = nullptr;
         return b;
      }
      return nullptr;
   }

   /** 
    * called by gc thread and pops the at-bat free list
    */
   block_header *get_garbage() // grab a pointer previously claimed.
   {
      if (block_header *gar = _gc_at_bat.load())
      {
         _gc_at_bat.store(nullptr);
         return gar;
      }
      return nullptr;
   }

   //callback to destruct thread_allocator
   class thread_allocator_gc
   {
   public:
      ~thread_allocator_gc()
      {
         thread_allocator &t = thread_allocator::get();
         thread_allocator *p = &t;
         if (p != nullptr)
         {
            t.destructor();
         }
      }
   };

   std::atomic<block_header *> _gc_at_bat; // where the gc pulls from.
   uint64_t _gc_pad1[7];                   // gc thread and this thread should not false-share these values
   block_header *_gc_on_deck;              // where we save frees while waiting on gc to bat.
   uint64_t _done;                         // use by gc to cleanup and remove from list.
   thread_allocator *_next;                // used by gc to link thread_allocs together
   block_header *_bin_cache[NUM_BINS + 1]; // cache for splited bin
};

typedef thread_allocator *thread_alloc_ptr;

class block_list
{
public:
   block_list() : _free_list(nullptr) {}

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
      block_header *head = _free_list;
      if (_free_list)
      {
         block_header *h = _free_list->as_queue_node().next;
         if (h)
            h->as_queue_node().prev = nullptr;
         _free_list = h;
         //Since the gc thread is single, the operation is safe
         head->set_state(block_header::unknown);
      }
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

private:
   block_header *_free_list;
};

/**
 *   Polls all threads for freed items.
 *   Upon receiving a freed item, it will look at its size and move it to the proper recycle bin for other threads to consume.
 *
 *   When there is less work to do, the garbage collector will attempt to combine blocks into larger blocks
 *   and move them to larger cache sizes until it ultimately 'completes a page' and returns it to the system.  
 *
 *   From the perspective of the 'system' an alloc involves a single atomic fetch_add.
 *
 *   A free involves a non-atomic store.
 *
 *   No other sync is necessary.
 */
class garbage_collector
{
public:
   garbage_collector();
   ~garbage_collector();

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

      ring_buffer<block_header *, QUEUE_SIZE> _free_queue;
      std::atomic<int64_t> _read_pos; // written to by read thread
      int64_t _pad[7];                // below this point is written to by gc thread
      int64_t _full_count;            // gc thread checked and found the queue full, no one want any
      int64_t _full;                  // limit the number of blocks kept in queue
      int64_t _write_pos;             // read by consumers to know the last valid entry.
      block_list _free_list;          // blocks are stored as a double-linked list
   };

   recycle_bin &find_cache_bin_for(block_header *h)
   {
      return _bins[LOG2(h->size())];
   }

   recycle_bin &get_bin(size_t bin_num)
   {
      return _bins[bin_num];
   }

   //Since the gc thread is single, the function is safe
   void clear_cached_block(block_header *h)
   {
      find_cache_bin_for(h)._free_list.remove(h);
   }

   void merge_block(block_header *h)
   {
      block_header *nxt_block = h->next();
      if (nxt_block->get_state() == block_header::mergable)
      {
         //need to clean up bin or the merged block may be allo twice
         clear_cached_block(nxt_block);
         h = h->merge_next();
      }
      block_header *prv_block = h->prev();
      if (prv_block->get_state() == block_header::mergable)
      {
         clear_cached_block(prv_block);
         h = h->merge_prev();
      }
   }

   void cache_block(block_header *h)
   {
      find_cache_bin_for(h)._free_list.push(h);
   }

   void register_allocator(thread_alloc_ptr ta);

   static garbage_collector &get()
   {
      static garbage_collector gc;
      return gc;
   }

private:
   static void run();

   std::atomic<thread_allocator *> _thread_head; // threads that we are actively looping on and use to release resource
   std::thread _thread;                          // gc thread.. doing the hard work
   static std::atomic<bool> _done;               //use to notice gc thread over
   recycle_bin _bins[NUM_BINS + 1];
};
std::atomic<bool> garbage_collector::_done(false);

garbage_collector::garbage_collector()
    : _thread_head(nullptr), _thread(&garbage_collector::run) {}

garbage_collector::~garbage_collector()
{
   _done.store(true, std::memory_order_release);
   _thread.join();
}

void garbage_collector::register_allocator(thread_allocator *ta)
{
   thread_allocator *stale_head = _thread_head.load(std::memory_order_relaxed);
   do
   {
      ta->_next = stale_head;
   } while (!_thread_head.compare_exchange_weak(stale_head, ta, std::memory_order_release));
}

void garbage_collector::run()
{
   try
   {
      garbage_collector &self = garbage_collector::get();

      while (true)
      {
         // for each thread, grab all of the free blocks and move them into the proper free set bin, but save the list for a follow-up merge
         // that takes into consideration all free blocks in all threads except those published in gc.
         thread_alloc_ptr cur_al = *((thread_alloc_ptr *)&self._thread_head);
         bool found_work = false;

         while (cur_al)
         {
            block_header *cur = cur_al->get_garbage();

            if (cur)
               found_work = true;

            //merge and cache block
            while (cur)
            {
               block_header *nxt = cur->as_queue_node().next;

               cur->set_state(block_header::mergable); //set state mergable
               self.merge_block(cur);
               self.cache_block(cur);
               cur = nxt;
            }

            // get the next thread.
            cur_al = cur_al->_next;
         }

         // for each recycle bin, check the queue to see if it
         // is getting low and if so, put some chunks in play
         for (int i = 0; i <= NUM_BINS; ++i)
         {
            garbage_collector::recycle_bin &bin = self._bins[i];

            int64_t needed = bin.check_status(); // returns the number of chunks need
            if (needed > 0)
            {
               bin._full_count = 0;

               int64_t next_write_pos = bin._write_pos;
               block_header *next = bin._free_list.pop();

               while (next && needed > 0)
               {
                  //poping block from bin and pushing into queue
                  found_work = true;
                  ++next_write_pos;
                  // skip left things，if the queue was full, it will keep skiping
                  if (!bin._free_queue.at(next_write_pos))
                  {
                     bin._free_queue.at(next_write_pos) = next;
                     next = bin._free_list.pop();
                  }
                  --needed;
               }

               if (next)
                  bin._free_list.push(next); // leftover

               bin._write_pos = next_write_pos;
            }
            else
               bin._full_count++;
         }

         if (!found_work)
         {
            // reclaim cache
            // TODO: can optimize data in queue that isn't be consumed
            for (int i = 0; i <= NUM_BINS; ++i)
            {
               garbage_collector::recycle_bin &bin = self._bins[i];
               if (bin._full_count > 10000)
               {
                  if (i == NUM_BINS) //keep page in queue
                  {
                     block_header *h;

                     while ((h = bin._free_list.pop()))
                     {
                        mmap_free(h, h->size() + 8);
                     }
                  }
                  else
                  {
                     int av = bin.available();
                     for (int i = 0; i < av; i++)
                     {
                        int64_t claim_pos = bin.claim(1);
                        if (claim_pos <= av)
                        {
                           block_header *h = bin.get_block(claim_pos);
                           if (h)
                           {
                              h->set_state(block_header::mergable); //set state mergable
                              self.merge_block(h);
                           }
                        }
                        else
                           break; //other thread has consumed
                     }
                  }
                  bin._full_count = 0;
               }
            }
         }

         if (!found_work)
            usleep(1000);

         if (_done.load(std::memory_order_acquire))
            return;
      }
   }
   catch (...)
   {
      fprintf(stderr, "gc caught exception\n");
   }
}

block_header *allocate_block_page()
{
   char *limit = mmap_alloc(CHUNK_SIZE);
   block_header *bl = reinterpret_cast<block_header *>(limit);
   bl->init(CHUNK_SIZE);

   return bl;
}

thread_allocator::thread_allocator()
{
   _done = false;
   _next = nullptr;
   _gc_at_bat = nullptr;
   _gc_on_deck = nullptr;
   memset(_bin_cache, 0, sizeof(_bin_cache));
   garbage_collector::get().register_allocator(this);
}

char *thread_allocator::alloc(size_t s)
{
   if (s == 0)
      return nullptr;

   // we need 8 bytes for the header, then round to the nearest power of 2.
   size_t data_size = s;
   if (data_size < 32)
      data_size = 32;
   else if (data_size > MIN_BLOCK_SIZE)
      return mmap_alloc(s);

   int min_bin = LOG2(s + 7) + 1;

   for (int bin = min_bin; bin <= NUM_BINS; ++bin)
   {
      block_header *b = fetch_block_from_bin(bin);
      if (b)
      {
         block_header *tail = b->split_after(s);
         if (tail && !store_cache(tail))
         {
            this->free(tail->data());
         }
         return b->data();
      }
   }

   block_header *new_page = allocate_block_page();
   block_header *tail = new_page->split_after(s);

   if (tail && !store_cache(tail))
   {
      this->free(tail->data());
   }

   return new_page->data();
}

/**
 *  Checks our local bin first, then checks the global bin.
 *
 *  @return null if no block found in cache.
 */
block_header *thread_allocator::fetch_block_from_bin(int bin)
{
   auto lo = fetch_cache(bin);
   if (lo)
      return lo;

   garbage_collector &gc = garbage_collector::get();

   garbage_collector::recycle_bin &rb = gc.get_bin(bin);

   // claim two blocks from gc, first store and second return
   bool found = false;
   for (int i = 0; i < 2; i++)
   {
      // this is our one and only atomic 'sync' operation...
      int64_t claim_pos = rb.claim(1);

      // it will be thread safe, not two threads will access the same queue
      if (claim_pos <= rb._write_pos)
      {
         block_header *h = rb.get_block(claim_pos);
         if (h)
            rb.clear_block(claim_pos); // let gc know we took it.

            if (!i)
            {
               store_cache(h);
               found = true;
            }
            else
               return h;
         }
      }
   }

   if (found)
      return fetch_cache(bin); // grab it from the cache this time.

   return nullptr;
}

char *fc_malloc(int s)
{
   return thread_allocator::get().alloc(s);
}

void fc_free(char *s)
{
   return thread_allocator::get().free(s);
}

int main(int argc, char const *argv[])
{
   return 0;
}