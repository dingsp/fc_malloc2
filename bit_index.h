#include <stdint.h>

#define LZERO(X) (__builtin_clzll((X)))

class bit_index
{
private:
    uint64_t _bits;

public:
    bit_index(uint64_t s = 0) : _bits(s) {}

    uint64_t first_set_bit() const
    {
        return _bits == 0 ? 64 : LZERO(_bits);
    }
    
    bool get(uint64_t pos) const { return _bits & (1ll << (63 - pos)); }

    void set(uint64_t pos)
    {
        assert(pos < 64);
        _bits |= (1ll << (63 - pos));
    }

    bool clear(uint64_t pos)
    {
        _bits &= ~(1ll << (63 - pos));
        return _bits == 0;
    }

    uint64_t count() const { return __builtin_popcountll(_bits); }

    void set_all() { _bits = -1; }

    void clear_all() { _bits = 0; }

    bool empty() { return _bits == 0; }
};