#ifndef PAGE_MAP
#define PAGE_MAP

#include <stdint.h>
#include "bit_index.h"
#include "common.h"
#include "bin_allocator.h"

//BITS
#define POINTER_BITS_64 (48)
#define KLEAF_BITS (15)
#define SMALL_BIN_BITS (10)
#define BITS (POINTER_BITS_64 - SMALL_BIN_BITS - KLEAF_BITS)
#define KROOT_BITS (BITS - KLEAF_BITS)

//LENGTH
#define KLEAF_LENGTH (1 << KLEAF_BITS)
#define KROOT_LENGTH (1 << KROOT_BITS)

//SIZE
#define LEAF_SIZE (sizeof(bin_info) * KLEAF_LENGTH)
#define LEAF_ALLOC_NUM 20
#define META_CHUNK_SIZE (LEAF_SIZE * LEAF_ALLOC_NUM)

class block_header;
class garbage_collector;
class thread_allocator;

class bin_info
{
    friend class pagemap;
    friend class garbage_collector;
    friend class thread_allocator;

public:
    bin_info(uint8_t sz) : size(sz) {}

    /**
     * @brief 使用位图偏移来分配内存块,h为aligned_block首地址
     */
    char *alloc(block_header *h, int &flag_full)
    {
        uint64_t pos = bindex.first_set_bit();
        bindex.set(pos);

        flag_full = 0;
        if (bindex.count() == SMALL_BIN_SIZE / size)
        {
            flag_full = 1;
        }

        char *p = h->data();
        return p + pos * size;
    }

    /**
     * @brief 使用内存块在位图中的序号来释放内存块，pos为内存块在aligned_blokc序号
     */
    void free(uint64_t pos, int &flag_empty)
    {
        bindex.clear(pos);

        flag_empty = 0;
        if (bindex.empty())
            flag_empty = 1;
    }

private:
    uint32_t size;
    bit_index bindex;
};

class pagemap
{
private:
    struct Leaf
    {
        bin_info binfo[KLEAF_LENGTH];
    };

    Leaf *root[KROOT_LENGTH];

public:
    typedef uintptr_t Number;

    pagemap() : root{} {}

    bin_info &get(Number n)
    {
        const Number i1 = n >> KLEAF_BITS;
        bin_info b(0);
        if (root[i1] == nullptr)
            return b;
        const Number i2 = n & (KLEAF_LENGTH - 1);
        if (root[i1]->binfo[i2].size == 0)
            return b;
        return root[i1]->binfo[i2];
    }

    bin_info &get_existing(Number n) const
    {
        const Number i1 = n >> KLEAF_BITS;
        const Number i2 = n & (KLEAF_LENGTH - 1);
        return root[i1]->binfo[i2];
    }

    void set(Number n, bin_info &b)
    {
        const Number i1 = n >> KLEAF_BITS;
        const Number i2 = n & (KLEAF_LENGTH - 1);
        root[i1]->binfo[i2] = b;
    }
    
    bool is_init(Number n){
        const Number i1 = n >> KLEAF_BITS;
        if(root[i1]==nullptr)
            return false;
        else
            return true;
    }

    void init(Number n, void *h)
    {
        const Number i1 = n >> KLEAF_BITS;

        // Make 2nd level node if necessary
        Leaf *leaf = reinterpret_cast<Leaf *>(h);
        memset(leaf, 0, sizeof(*leaf));
        root[i1] = leaf;
    }
};

#endif