/*
 * server.c - a single-threaded, non-blocking TCP chat server built on poll().
 *
 * The whole server runs in one thread. See README for why that beats
 * thread-per-client here; the short version is that every client's state is
 * touched by exactly one thread, so there are no locks and no data races to
 * get wrong.
 */
#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Written from a signal handler, so it must be sig_atomic_t and volatile:
 * anything else is undefined behaviour and the compiler is free to cache it
 * in a register, which would make the loop never notice the flag. */
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signum)
{
    (void)signum;
    g_stop = 1;
}

void server_install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    /* No SA_RESTART on purpose: we want poll() to return EINTR so the loop
     * checks g_stop promptly instead of sleeping until the next event. */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /*
     * Writing to a socket whose peer has closed raises SIGPIPE, whose default
     * action kills the process. One client hanging up would take the whole
     * server down. Ignoring it turns the same event into a write() that
     * returns -1/EPIPE, which the code below handles.
     */
    signal(SIGPIPE, SIG_IGN);
}

int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void log_msg(const char *fmt, ...)
{
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    va_list ap;
    fprintf(stderr, "[%s] ", stamp);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ output */

/*
 * Queues a message for one client. Never writes to the socket directly.
 *
 * This is the rule that keeps one slow client from stalling everyone: a
 * blocking send() to a peer that has stopped reading would block the single
 * thread, and with it every other conversation on the server. Instead the
 * bytes go into the client's own buffer and poll() tells us when the socket
 * can take them.
 */
static void queue(client *c, const char *msg)
{
    if (c->fd == -1 || c->closing)
        return;

    if (buffer_append_str(&c->out, msg) != 0) {
        log_msg("out of memory queueing for %s; dropping", c->nick);
        c->closing = true;
        return;
    }

    /*
     * Backpressure. If a peer never reads, its queue grows without bound and
     * the server eventually dies of memory exhaustion - which is a denial of
     * service any client could trigger deliberately. Past the cap we give up
     * on that one client rather than on the process.
     */
    if (c->out.len > MAX_OUTBUF_BYTES) {
        log_msg("%s is %zu bytes behind; disconnecting", c->nick, c->out.len);
        c->closing = true;
    }
}

/* Sends to everyone except `except` (pass NULL to include everyone). */
static void broadcast(roster *r, const client *except, const char *msg)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client *c = &r->clients[i];
        if (c->fd == -1 || c == except)
            continue;
        queue(c, msg);
    }
}

/* Writes whatever the kernel will currently take. Returns -1 if the client
 * should be disconnected. */
static int flush_client(client *c)
{
    while (c->out.len > 0) {
        ssize_t n = send(c->fd, c->out.data, c->out.len, 0);

        if (n > 0) {
            /* A partial write is normal, not an error: the socket send buffer
             * filled up. Consume only what was actually accepted and keep the
             * rest for the next POLLOUT. */
            buffer_consume(&c->out, (size_t)n);
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0; /* socket is full for now; try again on the next POLLOUT */

        if (n < 0 && errno == EINTR)
            continue; /* interrupted before sending anything; retry */

        return -1; /* EPIPE, ECONNRESET, ... */
    }
    return 0;
}

/* ----------------------------------------------------------------- commands */

static void cmd_who(roster *r, client *c)
{
    char line[MAX_LINE_LEN * 2];
    int n = snprintf(line, sizeof line, "* %d online:", r->count);

    for (int i = 0; i < MAX_CLIENTS && n > 0 && (size_t)n < sizeof line; i++) {
        if (r->clients[i].fd == -1)
            continue;
        n += snprintf(line + n, sizeof line - (size_t)n, " %s", r->clients[i].nick);
    }
    /* snprintf truncates rather than overflowing, but it returns the length it
     * WOULD have written, so n can exceed the buffer. Clamp before appending. */
    if (n < 0 || (size_t)n >= sizeof line)
        n = (int)sizeof line - 2;

    snprintf(line + n, sizeof line - (size_t)n, "\n");
    queue(c, line);
}

static void cmd_nick(roster *r, client *c, const char *arg)
{
    char msg[MAX_LINE_LEN + 128];

    const char *why = nick_validate(arg);
    if (why) {
        snprintf(msg, sizeof msg, "* cannot rename: %s\n", why);
        queue(c, msg);
        return;
    }

    /* "excluding self" so that /nick Alice while already Alice (or aLiCe)
     * is a no-op rather than a collision with yourself. */
    if (roster_find_nick_excluding(r, arg, c)) {
        snprintf(msg, sizeof msg, "* cannot rename: \"%s\" is taken\n", arg);
        queue(c, msg);
        return;
    }

    char old[MAX_NICK_LEN + 1];
    snprintf(old, sizeof old, "%s", c->nick);
    snprintf(c->nick, sizeof c->nick, "%s", arg);

    snprintf(msg, sizeof msg, "* %s is now known as %s\n", old, c->nick);
    broadcast(r, NULL, msg);
    log_msg("%s renamed %s -> %s", c->addr, old, c->nick);
}

static const char *HELP =
    "* commands: /nick <name>  /me <action>  /who  /help  /quit\n";

/* Handles one complete line from a client. */
static void handle_line(roster *r, client *c, char *line)
{
    char msg[MAX_LINE_LEN + MAX_NICK_LEN + 32];

    if (line[0] == '\0')
        return; /* an empty line is not a message */

    if (line[0] != '/') {
        snprintf(msg, sizeof msg, "[%s] %s\n", c->nick, line);
        broadcast(r, c, msg); /* the sender does not need their own words back */
        return;
    }

    /* Split "/cmd rest of line" into the verb and its argument. */
    char *arg = strchr(line, ' ');
    if (arg) {
        *arg = '\0';
        arg++;
        while (*arg == ' ')
            arg++;
    } else {
        arg = line + strlen(line); /* points at the NUL: an empty argument */
    }

    if (strcmp(line, "/nick") == 0) {
        cmd_nick(r, c, arg);
    } else if (strcmp(line, "/who") == 0) {
        cmd_who(r, c);
    } else if (strcmp(line, "/me") == 0) {
        if (*arg == '\0') {
            queue(c, "* usage: /me <action>\n");
        } else {
            snprintf(msg, sizeof msg, "* %s %s\n", c->nick, arg);
            broadcast(r, NULL, msg);
        }
    } else if (strcmp(line, "/help") == 0) {
        queue(c, HELP);
    } else if (strcmp(line, "/quit") == 0) {
        queue(c, "* goodbye\n");
        /* Not an immediate close: the goodbye is still only queued, and
         * closing now would discard it. The loop disconnects the client once
         * its output buffer has drained. */
        c->closing = true;
    } else {
        snprintf(msg, sizeof msg, "* unknown command \"%s\"; try /help\n", line);
        queue(c, msg);
    }
}

/* -------------------------------------------------------------------- input */

/* Returns -1 if the client should be disconnected. */
static int handle_readable(roster *r, client *c)
{
    char chunk[4096];

    for (;;) {
        ssize_t n = recv(c->fd, chunk, sizeof chunk, 0);

        if (n == 0)
            return -1; /* orderly shutdown by the peer */

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break; /* drained everything available */
            if (errno == EINTR)
                continue;
            return -1;
        }

        if (buffer_append(&c->in, chunk, (size_t)n) != 0) {
            log_msg("out of memory reading from %s", c->nick);
            return -1;
        }

        /*
         * TCP has no message boundaries. One recv() may hold half a message,
         * three messages, or two and a half - so the only correct thing to do
         * is accumulate and pull off whole lines. Assuming one recv() equals
         * one message is the single most common bug in a first chat server.
         */
        char line[MAX_LINE_LEN + 1];
        while (buffer_take_line(&c->in, line, sizeof line)) {
            handle_line(r, c, line);
            if (c->closing)
                return 0; /* stop parsing; drain output, then disconnect */
        }

        /*
         * A client that sends megabytes with no newline would otherwise make
         * `in` grow forever. Once more than one maximum line is buffered with
         * no terminator in sight, the peer is not speaking the protocol.
         */
        if (c->in.len > MAX_LINE_LEN && buffer_find_newline(&c->in) < 0) {
            queue(c, "* line too long\n");
            c->closing = true;
            c->in.len = 0;
            return 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ accept */

static void handle_accept(server *s)
{
    /*
     * Loop rather than accept once. The listening socket is level-triggered
     * here so a single accept per wakeup would still work, but draining the
     * backlog in one pass avoids a syscall round-trip per pending connection
     * and is what an edge-triggered epoll would require anyway.
     */
    for (;;) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof peer;

        int fd = accept(s->listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return; /* backlog drained */
            if (errno == EINTR || errno == ECONNABORTED)
                continue; /* a peer went away between SYN and accept */
            log_msg("accept: %s", strerror(errno));
            return;
        }

        char addr[INET6_ADDRSTRLEN + 8] = "unknown";
        if (peer.ss_family == AF_INET) {
            struct sockaddr_in *v4 = (struct sockaddr_in *)&peer;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof ip);
            snprintf(addr, sizeof addr, "%s:%u", ip, ntohs(v4->sin_port));
        }

        if (set_nonblocking(fd) != 0) {
            log_msg("set_nonblocking: %s", strerror(errno));
            close(fd);
            continue;
        }

        client *c = roster_add(&s->roster, fd, addr);
        if (!c) {
            /* Full. Say so and hang up, rather than accepting and then
             * silently dropping the connection. */
            const char *full = "* server full, try later\n";
            send(fd, full, strlen(full), MSG_NOSIGNAL);
            close(fd);
            log_msg("rejected %s: server full", addr);
            continue;
        }

        char msg[128];
        snprintf(msg, sizeof msg, "* welcome, you are %s. /help for commands\n", c->nick);
        queue(c, msg);

        snprintf(msg, sizeof msg, "* %s joined\n", c->nick);
        broadcast(&s->roster, c, msg);

        log_msg("%s connected as %s (%d online)", addr, c->nick, s->roster.count);
    }
}

static void disconnect(server *s, client *c, const char *why)
{
    char msg[128];
    char nick[MAX_NICK_LEN + 1];
    snprintf(nick, sizeof nick, "%s", c->nick);

    log_msg("%s (%s) disconnected: %s", nick, c->addr, why);
    roster_remove(&s->roster, c);

    snprintf(msg, sizeof msg, "* %s left\n", nick);
    broadcast(&s->roster, NULL, msg);
}

/* -------------------------------------------------------------- event loop */

int server_start(server *s, unsigned short port)
{
    roster_init(&s->roster);

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        perror("socket");
        return -1;
    }

    /*
     * Without SO_REUSEADDR, restarting the server fails with EADDRINUSE for
     * up to two minutes, because closed connections sit in TIME_WAIT holding
     * the port. This is the single most common "why won't my server start"
     * question.
     */
    int yes = 1;
    if (setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        perror("setsockopt");
        close(s->listen_fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port); /* htons: network byte order is big-endian */

    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(s->listen_fd);
        return -1;
    }

    if (listen(s->listen_fd, 64) < 0) {
        perror("listen");
        close(s->listen_fd);
        return -1;
    }

    /* The listening socket must be non-blocking too: accept() can block even
     * after poll() reports readiness, if the peer resets in between. */
    if (set_nonblocking(s->listen_fd) != 0) {
        perror("fcntl");
        close(s->listen_fd);
        return -1;
    }

    return 0;
}

unsigned short server_port(const server *s)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof addr;
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &len) < 0)
        return 0;
    return ntohs(addr.sin_port);
}

void server_run(server *s)
{
    struct pollfd fds[MAX_CLIENTS + 1];
    client *owner[MAX_CLIENTS + 1];

    while (!g_stop) {
        /* Slot 0 is always the listening socket. */
        int n = 0;
        fds[n].fd = s->listen_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        owner[n] = NULL;
        n++;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            client *c = &s->roster.clients[i];
            if (c->fd == -1)
                continue;

            fds[n].fd = c->fd;
            /* Only ask about writability when there is something to write.
             * Registering POLLOUT unconditionally makes poll() return
             * immediately every time and spins the CPU at 100%. */
            fds[n].events = POLLIN | (c->out.len > 0 ? POLLOUT : 0);
            fds[n].revents = 0;
            owner[n] = c;
            n++;
        }

        if (poll(fds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue; /* a signal arrived; re-check g_stop */
            log_msg("poll: %s", strerror(errno));
            break;
        }

        if (fds[0].revents & POLLIN)
            handle_accept(s);

        for (int i = 1; i < n; i++) {
            client *c = owner[i];

            /* The slot may have been freed earlier in this same pass. */
            if (c->fd == -1 || fds[i].revents == 0)
                continue;

            /* POLLHUP means the peer closed; POLLERR is a socket error.
             * Neither is reported through POLLIN, so both must be checked
             * explicitly or a hung-up client stays in the roster forever. */
            if (fds[i].revents & (POLLERR | POLLNVAL)) {
                disconnect(s, c, "socket error");
                continue;
            }

            if ((fds[i].revents & POLLOUT) && flush_client(c) != 0) {
                disconnect(s, c, "write failed");
                continue;
            }

            if (fds[i].revents & POLLIN) {
                if (handle_readable(&s->roster, c) != 0) {
                    disconnect(s, c, "peer closed");
                    continue;
                }
            } else if (fds[i].revents & POLLHUP) {
                /* Only after POLLIN is handled: a peer can close its writing
                 * end while a full message is still in flight, and that
                 * message should still be delivered. */
                disconnect(s, c, "hang-up");
                continue;
            }

            /* A client that asked to quit, or that we gave up on, leaves once
             * its queued bytes have gone out. */
            if (c->closing) {
                flush_client(c);
                if (c->out.len == 0)
                    disconnect(s, c, "closing");
            }
        }
    }

    log_msg("shutting down (%d client(s) connected)", s->roster.count);
}

void server_stop(server *s)
{
    roster_free(&s->roster);
    if (s->listen_fd != -1) {
        close(s->listen_fd);
        s->listen_fd = -1;
    }
}
