#ifndef BLOCK_HEADER
#define BLOCK_HEADER
#include <stdint.h>
#include <algorithm>

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
      alignblock = 4,
      metablock = 8,
   };

   flags_enum get_state() { return (flags_enum)_flags; }

   struct queue_state // the block is serving as a Double linked-list node
   {
      block_header *next;
      block_header *prev;
   };

   void set_state(flags_enum e)
   {
      _flags |= e;
   }

   void unset_state(flags_enum e)
   {
      _flags &= ~e;
   }

   bool is_mergable()
   {
      return (_flags &= mergable) != 0;
   }

   bool is_bigdata()
   {
      return (_flags &= bigdata) != 0;
   }

   bool is_aligned()
   {
      return (_flags &= alignblock) != 0;
   }

   bool is_meta()
   {
      return (_flags &= metablock) != 0;
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
      if (!nxt || !nxt->is_mergable())
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
      if (!p->is_mergable())
         return this;
      return p->merge_next();
   }

private:
   int32_t _prev_size; // offset to previous header.
   int32_t _size : 28; // offset to next, negitive indicates tail, 2 GB max
   int32_t _flags : 4;
};

#endif