#include <unistd.h>
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