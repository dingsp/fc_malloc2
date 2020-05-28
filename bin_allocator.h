#ifndef BIN_ALLOCATOR
#define BIN_ALLOCATOR

#include <cstddef>
#include "block_header.h"
#include "garbage_collect.h"

class thread_allocator;

template <size_t bin_num, size_t pop_size>
class bin_allocator
{
   friend class thread_allocator;

protected:
   block_header *_bin_cache[bin_num + 1]; //first cache

public:
   bin_allocator()
   {
      memset(_bin_cache, 0, bin_num + 1);
   }

   void constructor()
   {
      memset(_bin_cache, 0, bin_num + 1);
   }

   void destructor(garbage_collect &gcollect)
   {
      for (size_t i = 0; i < bin_num + 1; i++)
         if (_bin_cache[i])
            gcollect.release(_bin_cache[i]);
   }

   /**
    *  @brief 提取一级缓存
    */
   block_header *fetch_cache(int bin)
   {
      if (_bin_cache[bin])
      {
         block_header *h = _bin_cache[bin];
         _bin_cache[bin] = nullptr;
         return h;
      }
      else
         return nullptr;
   }

   /**
    *  @brief 获取一级缓存
    */
   block_header *get_cache(int bin)
   {
      if (_bin_cache[bin])
         return _bin_cache[bin];
      else
         return nullptr;
   }

   /**
    *  @brief 清空一级缓存
    */
   void *clear_cache(int bin)
   {
      _bin_cache[bin] = nullptr;
   }

   /**
    * @brief 存储一级缓存
    */
   bool store_cache(block_header *h, int bin)
   {
      if (_bin_cache[bin] != nullptr)
         return false;
      else
      {
         _bin_cache[bin] = h;
         return true;
      }
   }

   /**
    * @brief 提取中端
    */
   block_header *fetch_block_from_middle(recycle_bin &bin)
   {
      bool found = false;
      block_header *ret;

      // this is our one and only atomic 'sync' operation...
      int64_t claim_pos = rb.claim(1);

      // it will be thread safe, not two threads will access the same queue
      if (claim_pos <= rb._write_pos)
      {
         block_header *h = rb.get_block(claim_pos);
         if (h)
         {
            rb.clear_block(claim_pos); // let gc know we took it.
            return h;
         }
      }
   }

   /**
    * @brief 提取内存块
    */
   block_header *fetch_block_from_front_and_middle(int bin, recycle_bin &rbin)
   {
      //提取一级缓存
      block_header *ret;
      ret = fetch_cache(bin);
      if (ret)
         return ret;

      //从中端尝试提取两块
      bool found = false;
      ret = fetch_block_from_middle(rbin);
      if (ret)
      {
         found = true;
         if (!store(bin, ret))
            return ret;
      }

      ret = fetch_block_from_middle(rbin);
      if (ret)
         return ret;
      else if (found)
         return fetch_cache(bin);
      else
         return nullptr;
   }
};

/**
 * @brief 用来分配固定大小内存块
 */
template <size_t bin_num, size_t pop_size>
class fixed_bin_allocator : public bin_allocator
{
protected:
   /**
    * @brief 增加二级缓存，二级缓存的提取值固定
    */
   fixed_block_list<pop_size> _block_list;

public:
   fixed_bin_allocator() : _block_list(nullptr) {}

   void constructor()
   {
      bin_allocator::constructor();
      _block_list = nullptr;
   }

   void destructor(garbage_collect &gcollect)
   {
      bin_allocator::destructor();
      block_header *h = _block_list.pop_chunk();
      while (h)
      {
         gcollect.release(h);
         h = _block_list.pop_chunk();
      }
   }

   /**
    * @brief 提取二级缓存
    */
   block_header *fetch_list()
   {
      return _block_list.pop();
   }

   /**
    * @brief 存储二级缓存
    */
   void store_list(block_header *h)
   {
      _block_list.push(h);
   }

   /**
    * @brief 获取一级缓存
    */
   block_header *get_cache(int bin)
   {
      return bin_allocator::get_cache(bin);
   }

   /**
    *  @brief 清空一级缓存
    */
   void *clear_cache(int bin)
   {
      bin_allocator::clear_cache(int bin);
   }

   /**
    * @brief 存储一级缓存
    */
   bool store_cache(block_header *h, int bin)
   {
      return bin_allocator::store_cache(h, bin);
   }

   /**
    * @brief 提取中端
    */
   block_header *fetch_block_from_middle(recycle_bin &bin)
   {
      return bin_allocator::fetch_block_from_middle(bin);
   }

   block_header *fetch_block_from_second_cache_above(int bin, recycle_bin &rbin, garbage_collector &gcollect, block_header::flags_enum flag, size_t chunk_size, size_t list_cache_num)
   {
      //提取二级缓存
      block_header *h;
      h = fetch_list();
      if (h)
         return h;

      //从中端尝试提取四块
      bool found = false;
      for (size_t i = 0; i < list_cache_num - 1; i++)
      {
         h = fetch_block_from_middle();
         if (h)
         {
            store_cache(h);
            found = true;
         }
      }
      h = fetch_block_from_middle();
      if (h)
         return h;
      else if (found == true)
         return fetch_list();

      //从后端提取大块
      block_header *new_page = os::allocate_block_page(chunk_size);
      new_page->set_state(flag);

      //分割大块到缓存中
      block_header *p = new_page;
      block_header *tail = new_page->split_after(SMALL_BIN_SIZE);

      for (size_t i = 0; i < list_cache_num - 1; i++)
      {
         store_list(tail);
         tail = tail->split_after(pop_size);
      }
      _garbage_collect.release(tail);

      return new_page;
   }
};

#endif