/*
 * client.h - one connected peer, and the roster of all of them.
 */
#ifndef CLIENT_H
#define CLIENT_H

#include <stdbool.h>
#include <netinet/in.h>

#include "buffer.h"

#define MAX_CLIENTS      64
#define MAX_NICK_LEN     16      /* excluding the NUL */
#define MAX_LINE_LEN     512     /* longest message we accept, in bytes */
#define MAX_OUTBUF_BYTES 262144  /* 256 KiB of backlog before we give up on a peer */

typedef struct {
    int    fd;                        /* -1 when the slot is free */
    char   nick[MAX_NICK_LEN + 1];
    char   addr[INET6_ADDRSTRLEN + 8]; /* "ip:port", for logging */
    buffer in;                        /* bytes read, not yet a full line */
    buffer out;                       /* bytes to send, not yet accepted by the kernel */
    bool   closing;                   /* flush what is queued, then disconnect */
} client;

typedef struct {
    client clients[MAX_CLIENTS];
    int    count;
    int    next_guest; /* counter behind the default "guest7" style nicknames */
} roster;

void    roster_init(roster *r);
void    roster_free(roster *r);

/* Returns the new client, or NULL when the roster is full. */
client *roster_add(roster *r, int fd, const char *addr);

/* Closes the fd, frees the buffers and marks the slot free. */
void    roster_remove(roster *r, client *c);

/* Case-insensitive lookup; NULL if nobody is using that nickname. */
client *roster_find_nick(roster *r, const char *nick);

/* Same, but ignores one client - used when checking whether a rename would
 * collide with somebody other than the renamer themselves. */
client *roster_find_nick_excluding(roster *r, const char *nick, const client *skip);

/*
 * Validates a proposed nickname.
 * Returns NULL if it is acceptable, or a human-readable reason if not.
 * Rules: 1..MAX_NICK_LEN characters, and only [A-Za-z0-9_-].
 */
const char *nick_validate(const char *nick);

#endif /* CLIENT_H */
