/*
 * buffer.h - a growable byte buffer.
 *
 * Every client owns two of these: one for bytes read off the socket that do
 * not yet form a complete message, and one for bytes we want to send but the
 * kernel would not accept yet. Both cases are unavoidable with TCP, which is
 * a byte stream with no notion of a message.
 */
#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

typedef struct {
    char  *data; /* NOT NUL-terminated; len is authoritative */
    size_t len;  /* bytes currently held */
    size_t cap;  /* bytes allocated */
} buffer;

/* Zero-initialises. A zeroed buffer is valid and allocates nothing. */
void buffer_init(buffer *b);

/* Frees the storage and returns the buffer to the empty state. */
void buffer_free(buffer *b);

/*
 * Appends n bytes. Returns 0 on success, -1 if allocation failed
 * (in which case the buffer is left exactly as it was).
 */
int buffer_append(buffer *b, const char *bytes, size_t n);

/* Appends a NUL-terminated string, without the terminator. */
int buffer_append_str(buffer *b, const char *s);

/*
 * Removes the first n bytes, shifting the rest down. Used after a partial
 * write, to drop only what the kernel actually accepted.
 */
void buffer_consume(buffer *b, size_t n);

/*
 * Extracts the first complete '\n'-terminated line.
 *
 * On success, copies the line WITHOUT its terminator into out (always
 * NUL-terminated, truncated to out_size-1), removes it from the buffer and
 * returns 1. Returns 0 if no complete line is buffered yet.
 *
 * A trailing '\r' is stripped, so telnet and netcat clients (which send
 * CRLF) behave the same as raw socket clients.
 */
int buffer_take_line(buffer *b, char *out, size_t out_size);

/* Index of the first '\n', or -1. Exposed for the line-length guard. */
long buffer_find_newline(const buffer *b);

#endif /* BUFFER_H */
