#include "buffer.h"

#include <stdlib.h>
#include <string.h>

#define BUFFER_MIN_CAP 256

void buffer_init(buffer *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void buffer_free(buffer *b)
{
    free(b->data);
    buffer_init(b);
}

int buffer_append(buffer *b, const char *bytes, size_t n)
{
    if (n == 0)
        return 0;

    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : BUFFER_MIN_CAP;

        /* Double until it fits: amortised O(1) per byte appended, rather
         * than the O(n^2) copying that growing by exactly n would cause. */
        while (cap < b->len + n) {
            /* Guard the doubling itself against overflow before it wraps. */
            if (cap > (size_t)-1 / 2)
                return -1;
            cap *= 2;
        }

        /* realloc into a temporary: on failure realloc returns NULL and the
         * original block is still valid, so assigning it directly to b->data
         * would leak everything the buffer held. */
        char *grown = realloc(b->data, cap);
        if (!grown)
            return -1;

        b->data = grown;
        b->cap = cap;
    }

    memcpy(b->data + b->len, bytes, n);
    b->len += n;
    return 0;
}

int buffer_append_str(buffer *b, const char *s)
{
    return buffer_append(b, s, strlen(s));
}

void buffer_consume(buffer *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    /* memmove, not memcpy: source and destination overlap. */
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

long buffer_find_newline(const buffer *b)
{
    if (b->len == 0)
        return -1;

    /* memchr, not strchr: the buffer holds arbitrary bytes and is not
     * NUL-terminated, so strchr would read past len. */
    const char *nl = memchr(b->data, '\n', b->len);
    return nl ? (long)(nl - b->data) : -1;
}

int buffer_take_line(buffer *b, char *out, size_t out_size)
{
    long idx = buffer_find_newline(b);
    if (idx < 0)
        return 0;

    size_t line_len = (size_t)idx;

    /* Strip CR so that CRLF clients (telnet, netcat -C) match LF clients. */
    if (line_len > 0 && b->data[line_len - 1] == '\r')
        line_len--;

    size_t copy = line_len;
    if (out_size == 0)
        return 0;
    if (copy > out_size - 1)
        copy = out_size - 1;

    memcpy(out, b->data, copy);
    out[copy] = '\0';

    /* Consume the line AND its terminator, whatever we chose to copy. */
    buffer_consume(b, (size_t)idx + 1);
    return 1;
}
