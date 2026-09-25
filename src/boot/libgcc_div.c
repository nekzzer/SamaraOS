/* Freestanding replacements for libgcc 64-bit division helpers.
 * Needed because the host libgcc is 64-bit-only and we link -m32.
 * Bit-by-bit long division: no real 64-bit div instruction used,
 * so gcc cannot recurse back into these symbols.
 */
#include <stdint.h>

static uint64_t udivmod64(uint64_t num, uint64_t den, uint64_t* rem)
{
    /* Fast path: both operands fit in 32 bits (by far the common case for
     * offsets/sizes in the kernel) -> use the native 32/32 divide instead
     * of a 64-iteration bit loop. */
    if (num <= UINT32_MAX && den <= UINT32_MAX) {
        uint32_t n = (uint32_t)num, d = (uint32_t)den;
        if (rem)
            *rem = n % d;
        return n / d;
    }

    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((num >> i) & 1);
        if (r >= den) {
            r -= den;
            q |= (uint64_t)1 << i;
        }
    }
    if (rem)
        *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    return udivmod64(a, b, 0);
}

uint64_t __umoddi3(uint64_t a, uint64_t b)
{
    uint64_t r;
    udivmod64(a, b, &r);
    return r;
}

int64_t __divdi3(int64_t a, int64_t b)
{
    uint64_t ua = a < 0 ? -(uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? -(uint64_t)b : (uint64_t)b;
    int64_t q = (int64_t)udivmod64(ua, ub, 0);
    return (a < 0) != (b < 0) ? -q : q;
}

int64_t __moddi3(int64_t a, int64_t b)
{
    uint64_t ua = a < 0 ? -(uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? -(uint64_t)b : (uint64_t)b;
    uint64_t r;
    udivmod64(ua, ub, &r);
    return a < 0 ? -(int64_t)r : (int64_t)r;
}
