/**
 *   Each thread has its own 'arena' where it can allocate 'new' blocks of what ever size it needs (buckets). After
 *   a thread is done with memory it places it in a garbage collection queue.
 *
 *   The garbage collector follows each threads trash bin and moves the blocks into a recycled list that
 *   all other threads can pull from.
 *
 *   The garbage collector can grow these queues as necessary and shrink them as time progresses.
 */
#include "common.h"
#include "block_header.h"
#include "bin_allocator.h"
#include "recycle_bin.h"
#include "size_map.h"
#include "page_map.h"
#include "os.h"

#define LOG2(X) ((unsigned)(8 * sizeof(unsigned long long) - __builtin_clzll((X)) - 1))

class garbage_collector;
class thread_allocator_gc;

//thread private，all the member will be allocated in mmap area
class thread_allocator
{
    friend class garbage_collector;
    friend class thread_allocator_gc;

private:
    uint64_t _done;          // use by gc to cleanup and remove from list.
    thread_allocator *_next; // used by gc to link thread_allocs together

    garbage_collect _garbage_collect;

    bin_allocator<NUM_LARGE_BINS, 0> _large_bin_allocator;
    fixed_bin_allocator<NUM_SMALL_BINS, SMALL_BIN_SIZE> _small_bin_allocator;
    fixed_bin_allocator<1, LEAF_SIZE> _meta_bin_allocator;

public:
    char *alloc(size_t s);

    char *alloc_small(int bin, block_header *h, bin_info &binfo)
    {
        int flag_full = 0;
        char *p = binfo.alloc(h, flag_full);
        if (flag_full)
            _small_bin_allocator.clear_cache(bin);
        return p;
    }

    /**
     * @brief 快速释放内存块
     */
    void free(char *c);

    void free_small(char *c, bin_info &binfo)
    {
        block_header *h = reinterpret_cast<block_header *>(c);
        int size = binfo.size;
        int pos = garbage_collector::get_pos(h, size);
        int flag_empty = 0;
        binfo.free(pos, flag_empty);
        if (flag_empty)
            _garbage_collect.release(h);
    }

    /**
     * @brief 单例模式获取线程类
     */
    static thread_allocator *get()
    {
        static __thread thread_allocator *tld = nullptr;

        if (!tld)
        {
            tld = reinterpret_cast<thread_allocator *>(os::mmap_alloc(sizeof(thread_allocator)));

            thread_allocator::constructor(tld);

            //allocate pthread_threadlocal var, attach a destructor /clean up callback to that variable
            thread_local thread_allocator_gc tlv;
        }
        return tld;
    }

private:
    thread_allocator();

    ~thread_allocator();

    static void constructor(thread_allocator *tp)
    {
        tp->_done = false;
        tp->_next = nullptr;
        tp->_garbage_collect.constructor();
        tp->_large_bin_allocator.constructor();
        tp->_small_bin_allocator.constructor();
        tp->_meta_bin_allocator.constructor();
        garbage_collector::get().register_allocator(tp);
    }

    static void destructor(thread_allocator *tp)
    {
        tp->_done = 1; //final release by gc via ummap
        // give the rest of our allocated chunks to the gc thread
        tp->_large_bin_allocator.destructor(tp->_garbage_collect);
        tp->_small_bin_allocator.destructor(tp->_garbage_collect);
        tp->_meta_bin_allocator.destructor(tp->_garbage_collect);
    }
};

//callback to destruct thread_allocator
class thread_allocator_gc
{
public:
    ~thread_allocator_gc()
    {
        thread_allocator *tp = thread_allocator::get();
        if (tp != nullptr)
        {
            thread_allocator::destructor(tp);
        }
    }
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
    garbage_collector()
        : smap(), _thread_head(nullptr), _thread(&garbage_collector::run) {}

    ~garbage_collector()
    {
        _done.store(true, std::memory_order_release);
        _thread.join();
    }

    /**
     * @brief 核心思想是释放时候的计算延迟进行，所以这里通过标记来进行多台
     */
    recycle_bin &find_recycle_bin_for(block_header *h)
    {
        if (h->is_aligned())
            return _algin_bin;
        if (h->is_meta())
            return _meta_bin;
        else
            return _bins[get_size_class(h->size())];
    }

    recycle_bin &get_bin(int large_bin)
    {
        return _bins[large_bin - NUM_SMALL_BINS];
    }

    recycle_bin &get_align_bin()
    {
        return _algin_bin;
    }

    recycle_bin &get_meta_bin()
    {
        return _meta_bin;
    }

    /**
     * @brief 用来合并recyclebin中缓存状态的内存块
     */
    void merge_block(block_header *h)
    {
        block_header *nxt_block = h->next();
        if (nxt_block->is_mergable())
        {
            //需要清除recyclebin中的缓存
            find_recycle_bin_for(nxt_block).clear_cached_block(nxt_block);
            h = h->merge_next();
        }
        block_header *prv_block = h->prev();
        if (prv_block->is_mergable())
        {
            find_recycle_bin_for(prv_block).clear_cached_block(prv_block);
            h = h->merge_prev();
        }
    }

    /**
     * @brief 本地线程调用用来注册自己
     */
    void register_allocator(thread_allocator *ta)
    {
        thread_allocator *stale_head = _thread_head.load(std::memory_order_relaxed);
        do
        {
            ta->_next = stale_head;
        } while (!_thread_head.compare_exchange_weak(stale_head, ta, std::memory_order_release));
    }

    /**
     * @brief 垃圾回收器调用用来解除线程的注册
     */
    void register_allocator(thread_allocator *ta);

    /**
     * @brief 单例模式获取类实例
     */
    static garbage_collector &get();

    /**
    * @brief 获取大小类
    */
    size_t get_size_class(size_t t)
    {
        return smap.get_sizeclass(t);
    }

    /**
     * @brief 获取按层次搜索recyclebin的跳跃层数
     */
    int get_next_recycle_bin(int bin)
    {
        return smap.get_next_recycle_bin(bin);
    }

    //////////////////////////////////////////////////映射相关API-start//////////////////////////////////////////////////
    /**
     * @brief 获取bin_info
     */
    bin_info &get_bin_info(block_header *h)
    {
        return pmap.get(get_number(h));
    }

    /**
     * @brief 判断是否存在映射
     */
    static inline bool is_mapped(bin_info &binfo)
    {
        return binfo.size == 0;
    }

    /**
     * @brief 判断是否初始化映射内部数据结构
     */
    bool is_init(block_header *h)
    {
        return pmap.is_init(get_number(h));
    }

    /**
     * @brief 初始化映射内部数据结构
     */
    void init(block_header *h, block_header *meta_h)
    {
        void *p = reinterpret_cast<void *>(meta_h);
        pmap.init(get_number(h), p);
    }

    /**
     * @brief 获取在单元块中偏移量
     */
    static inline uint64_t get_pos(block_header *h, int size)
    {
        uint64_t ret = reinterpret_cast<uint64_t>(h);
        return (ret & (1 << (SMALL_BIN_BITS + 1) - 1) - HEDER_SIZE) / size;
    }
    //////////////////////////////////////////////////映射相关API-end//////////////////////////////////////////////////

private:
    static void run();

    static inline pagemap::Number get_number(block_header *h)
    {
        return reinterpret_cast<pagemap::Number>(h) >> SMALL_BIN_SIZE;
    }

    std::atomic<thread_allocator *> _thread_head; // threads that we are actively looping on and use to release resource
    std::thread _thread;                          // gc thread.. doing the hard work
    static std::atomic<bool> _done;               //use to notice gc thread over
    recycle_bin _bins[NUM_LARGE_BINS + 1];
    recycle_bin _algin_bin, _meta_bin;
    sizemap smap;
    pagemap pmap;
};

std::atomic<bool> garbage_collector::_done(false);

garbage_collector &garbage_collector::get()
{
    static garbage_collector gc;
    return gc;
}

void garbage_collector::run()
{
    try
    {
        garbage_collector &self = garbage_collector::get();

        while (true)
        {
            thread_allocator *cur_al = *((thread_allocator **)&self._thread_head);
            bool found_work = false;

            //遍历注册线程,回收垃圾
            while (cur_al)
            {
                //拿到其垃圾，并尝试在整个recyclebin范围内去合并，将合并后的大块放入对应recyclebin的缓存中
                block_header *cur = cur_al->_garbage_collect.get_garbage();

                if (cur)
                    found_work = true;

                while (cur)
                {
                    block_header *nxt = cur->as_queue_node().next;
                    cur->set_state(block_header::mergable); //set state mergable
                    self.merge_block(cur);
                    self.find_recycle_bin_for(cur).cache_block(cur);
                    cur = nxt;
                }

                // get the next thread.
                cur_al = cur_al->_next;
            }

            //全局池中生产
            for (size_t i = 0; i <= NUM_LARGE_BINS; i++)
            {
                if (self._bins[i].produce_block_to_ring_buffer())
                    found_work = true;
            }
            self._algin_bin.produce_block_to_ring_buffer();
            self._meta_bin.produce_block_to_ring_buffer();

            //重新声明全局池，寻找自适应算法检测ring_buffer满的
            if (!found_work)
            {
                for (size_t i = 0; i <= NUM_LARGE_BINS; i++)
                    self._bins[i].reclaim_ring_buffer();
                self._algin_bin.reclaim_ring_buffer();
                self._meta_bin.reclaim_ring_buffer();
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

void thread_allocator::free(char *c)
{
    ////////////////////////////////////////////小块内存释放-start////////////////////////////////////////////
    block_header *h = reinterpret_cast<block_header *>(c);

    garbage_collector &gc = garbage_collector::get();
    bin_info &binfo = gc.get_bin_info(h);

    if (garbage_collector::is_mapped(binfo))
        free_small(c, binfo);
    ////////////////////////////////////////////小块内存释放-end////////////////////////////////////////////

    ////////////////////////////////////////////大块内存释放-start////////////////////////////////////////////
    //尝试大块释放,直接加入垃圾回收区中
    h = reinterpret_cast<block_header *>(c - HEDER_SIZE);
    if (h->size() <= LARGE_BLOCK)
    {
        _garbage_collect.release(h);
        return;
    }
    ////////////////////////////////////////////大块内存释放-end////////////////////////////////////////////

    ////////////////////////////////////////////巨大块内存释放-start////////////////////////////////////////////
    os::mmap_free(h, h->size() + HEDER_SIZE);
    ////////////////////////////////////////////巨大块内存释放-end////////////////////////////////////////////
    return;
}

char *thread_allocator::alloc(size_t s)
{
    if (s == 0)
        return nullptr;
    if (s < MIN_BLOCK_SIZE)
        s = MIN_BLOCK_SIZE;

    garbage_collector &gc = garbage_collector::get();
    block_header *h, *new_page, *tail;

    ////////////////////////////////////////////小块内存分配-start////////////////////////////////////////////
    if (s <= SMALL_BLOCK)
    {
        int bin = gc.get_size_class(s);

        //尝试调用前端一级缓存，成功直接返回
        h = _small_bin_allocator.get_cache(bin);
        if (h)
            return alloc_small(bin, h, gc.get_bin_info(h));

        //重新提取一个单元块
        h = _small_bin_allocator.fetch_block_from_second_cache_above(bin, gc.get_align_bin(), _garbage_collect, block_header::alignblock, ALIGN_CHUNK_SIZE, LIST_CACHE_NUM);

        //尝试建立映射
        if (!gc.is_init(h))
        {
            block_header *meta_h = _meta_bin_allocator.fetch_block_from_second_cache_above(1, gc.get_meta_bin(), _garbage_collect, block_header::metablock, META_CHUNK_SIZE, LIST_CACHE_NUM / 2);
            gc.init(h, meta_h);
        }

        //重新分配
        return alloc_small(bin, h, gc.get_bin_info(h));
    ////////////////////////////////////////////小块内存分配-end////////////////////////////////////////////
    }
    ////////////////////////////////////////////大块内存分配-start////////////////////////////////////////////
    else if (s + HEDER_SIZE < LARGE_BLOCK)
    {
        //多层次调用bin
        int min_bin = gc.get_size_class(s + HEDER_SIZE);
        min_bin -= NUM_SMALL_BINS;
        for (int bin = min_bin; bin <= NUM_BINS; bin += gc.get_next_recycle_bin(bin))
        {
            h = _large_bin_allocator.fetch_block_from_front_and_middle(bin, gc);
            if (h)
            {
                tail = h->split_after(s);
                if (bin != min_bin && !_large_bin_allocator.store_cache(h, bin))
                {
                    _garbage_collect.release(tail);
                }
                return h->data();
            }
        }

        //调用后端并切割
        new_page = os::allocate_block_page(CHUNK_SIZE);
        tail = new_page->split_after(s);

        if (tail && !_large_bin_allocator.store_cache(tail, gc.get_size_class(tail->size())))
        {
            _garbage_collect.release(tail);
        }

        return new_page->data();
    ////////////////////////////////////////////大块内存分配-end////////////////////////////////////////////
    }
    ////////////////////////////////////////////巨大块内存分配-start////////////////////////////////////////////
    else
    {
        new_page = os::allocate_block_page(s + HEDER_SIZE);
        new_page->set_state(block_header::bigdata);
        return new_page->data();
    }
    ////////////////////////////////////////////巨大块内存分配-end////////////////////////////////////////////
}