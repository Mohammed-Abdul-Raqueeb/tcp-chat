#include "client.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void roster_init(roster *r)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        r->clients[i].fd = -1;
        buffer_init(&r->clients[i].in);
        buffer_init(&r->clients[i].out);
    }
    r->count = 0;
    r->next_guest = 1;
}

void roster_free(roster *r)
{
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (r->clients[i].fd != -1)
            roster_remove(r, &r->clients[i]);
}

client *roster_add(roster *r, int fd, const char *addr)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &r->clients[i];
        if (c->fd != -1)
            continue;

        c->fd = fd;
        c->closing = false;
        buffer_init(&c->in);
        buffer_init(&c->out);

        snprintf(c->addr, sizeof c->addr, "%s", addr);

        /* Everyone gets a usable name immediately, so a client can talk
         * before it has run /nick. Uniqueness is guaranteed by the
         * monotonic counter, which is never decremented on disconnect. */
        do {
            snprintf(c->nick, sizeof c->nick, "guest%d", r->next_guest++);
        } while (roster_find_nick_excluding(r, c->nick, c));

        r->count++;
        return c;
    }
    return NULL; /* roster full */
}

void roster_remove(roster *r, client *c)
{
    if (c->fd == -1)
        return;

    close(c->fd);
    c->fd = -1;
    c->closing = false;

    /* Both buffers are heap allocations owned by the client. Forgetting
     * either one leaks on every disconnect, which on a long-running server
     * is the difference between stable and dead by Thursday. */
    buffer_free(&c->in);
    buffer_free(&c->out);

    r->count--;
}

/* Case-insensitive because "Alice" and "alice" being different people in a
 * chat room confuses humans far more than it helps them. */
static int nick_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

client *roster_find_nick_excluding(roster *r, const char *nick, const client *skip)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &r->clients[i];
        if (c->fd == -1 || c == skip)
            continue;
        if (nick_equal(c->nick, nick))
            return c;
    }
    return NULL;
}

client *roster_find_nick(roster *r, const char *nick)
{
    return roster_find_nick_excluding(r, nick, NULL);
}

const char *nick_validate(const char *nick)
{
    size_t len = strlen(nick);

    if (len == 0)
        return "nickname cannot be empty";
    if (len > MAX_NICK_LEN)
        return "nickname is too long";

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)nick[i];
        /* A whitelist, not a blacklist. Control characters would corrupt a
         * terminal, and a space would make "/who" output ambiguous. */
        if (!isalnum(ch) && ch != '_' && ch != '-')
            return "nickname may only contain letters, digits, '_' and '-'";
    }
    return NULL;
}
