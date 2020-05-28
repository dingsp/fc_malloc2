#include <stddef.h>
#include <stdint.h>
#include <sizeclass.h>

#define LOG2(X) ((unsigned)(8 * sizeof(unsigned long long) - __builtin_clzll((X)) - 1))

inline constexpr size_t kNumClasses = 78;
inline constexpr size_t kMaxSize = 256 * 1024;

inline constexpr size_t kAlignment = 8;
inline constexpr size_t kAlignmentShift = LOG2(kAlignment);

// Sizes <= 1024 have an alignment >= 8.  So for such sizes we have an
// array indexed by ceil(size/8).  Sizes > 1024 have an alignment >= 128.
// So for these larger sizes we have an array indexed by ceil(size/128).
//
// We flatten both logical arrays into one physical array and use
// arithmetic to compute an appropriate index.  The constants used by
// ClassIndex() were selected to make the flattening work.
//
// Examples:
//   Size       Expression                      Index
//   -------------------------------------------------------
//   0          (0 + 7) / 8                     0
//   1          (1 + 7) / 8                     1
//   ...
//   1024       (1024 + 7) / 8                  128
//   1025       (1025 + 127 + (120<<7)) / 128   129
//   ...
//   32768      (32768 + 127 + (120<<7)) / 128  376
static constexpr int kMaxSmallSize = 1024;
static constexpr size_t kClassArraySize =
    ((kMaxSize + 127 + (120 << 7)) >> 7) + 1;

class sizemap
{
public:
    sizemap()
    {
        init_class_array();
    }

    inline size_t get_sizeclass(size_t size)
    {
        return class_array_[ClassIndex(size)];
    }

    inline size_t get_next_recycle_bin(size_t bin)
    {
        return kSizeClasses[bin].next_recycle_bin;
    }

private:
    void init_class_array();

    //compute index of the class_array[] entry for it,
    static inline size_t ClassIndex(size_t s)
    {
        if ((s <= kMaxSmallSize))
            return (static_cast<uint32_t>(s) + 7) >> 3;
        else if (s <= kMaxSize)
            return (static_cast<uint32_t>(s) + 127 + (120 << 7)) >> 7;
    }

    // class_array_ is accessed on every malloc
    unsigned char class_array_[kClassArraySize];
};

void sizemap::init_class_array()
{
    // Mapping from size class to max size storable in that class
    uint32_t class_to_size_[kNumClasses];
    uint32_t class_to_nrbin_[kNumClasses];

    for (int x = 0; x < kNumClasses; x++)
    {
        class_to_size_[x] = kSizeClasses[kNumClasses].size;

        int next_size = 0;
        //遍历所有大小类
        for (int c = 1; c < kNumClasses; c++)
        {
            const int max_size_in_class = class_to_size_[c];

            //遍历所有8递增的size，计算其大小类
            for (int s = next_size; s <= max_size_in_class; s += kAlignment)
            {
                class_array_[ClassIndex(s)] = c;
            }
            next_size = max_size_in_class + kAlignment;
            if (next_size > kMaxSize)
            {
                break;
            }
        }
    }
}