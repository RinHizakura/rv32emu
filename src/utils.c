/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "utils.h"

#if defined(__APPLE__)
#define HAVE_MACH_TIMER
#include <mach/mach_time.h>
#elif !defined(_WIN32) && !defined(_WIN64)
#define HAVE_POSIX_TIMER
#ifdef CLOCK_MONOTONIC
#define CLOCKID CLOCK_MONOTONIC
#else
#define CLOCKID CLOCK_REALTIME
#endif
#endif

#define MAX_PATH_LEN 1024

/* Calculate "x * n / d" without unnecessary overflow or loss of precision.
 *
 * Reference:
 * https://elixir.bootlin.com/linux/v6.10.7/source/include/linux/math.h#L121
 */
#if !defined(HAVE_POSIX_TIMER)
static inline uint64_t mult_frac(uint64_t x, uint64_t n, uint64_t d)
{
    const uint64_t q = x / d;
    const uint64_t r = x % d;

    return q * n + r * n / d;
}
#endif

static void get_time_info(int32_t *tv_sec, int32_t *tv_nsec)
{
#if defined(HAVE_POSIX_TIMER)
    struct timespec t;
    clock_gettime(CLOCKID, &t);
    *tv_sec = t.tv_sec;
    *tv_nsec = t.tv_nsec;
#elif defined(HAVE_MACH_TIMER)
    static mach_timebase_info_data_t info;
    /* If it is the first time running, obtain the timebase. Using denom == 0
     * indicates that sTimebaseInfo is uninitialized.
     */
    if (info.denom == 0)
        (void) mach_timebase_info(&info);
    uint64_t nsecs = mult_frac(mach_absolute_time(), info.numer, info.denom);
    *tv_sec = nsecs / 1e9;
    *tv_nsec = nsecs - (*tv_sec * 1e9);
#else /* low resolution timer */
    clock_t t = clock();
    *tv_sec = t / CLOCKS_PER_SEC;
    *tv_nsec = mult_frac(t % CLOCKS_PER_SEC, 1e9, CLOCKS_PER_SEC);
#endif
}

void rv_gettimeofday(struct timeval *tv)
{
    int32_t tv_sec, tv_nsec;
    get_time_info(&tv_sec, &tv_nsec);
    tv->tv_sec = tv_sec;
    tv->tv_usec = tv_nsec / 1000;
}

void rv_clock_gettime(struct timespec *tp)
{
    int32_t tv_sec, tv_nsec;
    get_time_info(&tv_sec, &tv_nsec);
    tp->tv_sec = tv_sec;
    tp->tv_nsec = tv_nsec;
}

char *sanitize_path(const char *input)
{
    size_t n = strnlen(input, MAX_PATH_LEN);

    char *ret = calloc(n + 1, sizeof(char));
    if (!ret)
        return NULL;

    /* After sanitization, the new path will only be shorter than the original
     * one. Thus, we can reuse the space.
     */
    if (n == 0) {
        ret[0] = '.';
        return ret;
    }

    bool is_root = (input[0] == '/');

    /* Invariants:
     * reading from path; r is index of next byte to process -> path[r]
     * writing to buf; w is index of next byte to write -> ret[strlen(ret)]
     * dotdot is index in buf where .. must stop, either because:
     *   (a) it is the leading slash;
     *   (b) it is a leading ../../.. prefix.
     */
    size_t w = 0, r = 0;
    size_t dotdot = 0;
    if (is_root) {
        ret[w] = '/';
        w++;
        r = 1;
        dotdot = 1;
    }

    while (r < n) {
        if (input[r] == '/') {
            /*  empty path element */
            r++;
        } else if (input[r] == '.' && (r + 1 == n || input[r + 1] == '/')) {
            /* . element */
            r++;
        } else if (input[r] == '.' && input[r + 1] == '.' &&
                   (r + 2 == n || input[r + 2] == '/')) {
            /* .. element: remove to last '/' */
            r += 2;

            if (w > dotdot) {
                /* can backtrack */
                w--;
                while (w > dotdot && ret[w] != '/') {
                    w--;
                }
            } else if (!is_root) {
                /* cannot backtrack, but not is_root, so append .. element. */
                if (w > 0) {
                    ret[w] = '/';
                    w++;
                }
                ret[w] = '.';
                w++;
                ret[w] = '.';
                w++;
                dotdot = w;
            }
        } else {
            /* real path element, add slash if needed */
            if ((is_root && w != 1) || (!is_root && w != 0)) {
                ret[w] = '/';
                w++;
            }

            /* copy element */
            for (; r < n && input[r] != '/'; r++) {
                ret[w] = input[r];
                w++;
            }
        }
    }

    /* Turn empty string into "." */
    if (w == 0) {
        ret[w] = '.';
        w++;
    }

    /* starting from w till the end, we should mark it as '\0' since that part
     * of the buffer is not used.
     */
    memset(ret + w, '\0', n + 1 - w);

    return ret;
}

HASH_FUNC_IMPL(set_hash, SET_SIZE_BITS, 1 << SET_SIZE_BITS);

void set_reset(set_t *set)
{
    memset(set, 0, sizeof(set_t));
}

/**
 * set_add - insert a new element into the set
 * @set: a pointer points to target set
 * @key: the key of the inserted entry
 */
bool set_add(set_t *set, rv_hash_key_t key)
{
    const rv_hash_key_t index = set_hash(key);

    uint8_t count = 0;
    for (; set->table[index][count]; count++) {
        assert(count < SET_SLOTS_SIZE);
        if (set->table[index][count] == key)
            return false;
    }

    assert(count < SET_SLOTS_SIZE);
    set->table[index][count] = key;
    return true;
}

/**
 * set_has - check whether the element exist in the set or not
 * @set: a pointer points to target set
 * @key: the key of the inserted entry
 */
bool set_has(set_t *set, rv_hash_key_t key)
{
    const rv_hash_key_t index = set_hash(key);

    for (uint8_t count = 0; set->table[index][count]; count++) {
        assert(count < SET_SLOTS_SIZE);
        if (set->table[index][count] == key)
            return true;
    }
    return false;
}
