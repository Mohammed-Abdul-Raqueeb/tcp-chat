/*
 * server.h - the event loop.
 */
#ifndef SERVER_H
#define SERVER_H

#include "client.h"

typedef struct {
    int    listen_fd;
    roster roster;
} server;

/*
 * Binds and listens on the given port.
 * Returns 0 on success, -1 on failure (with a message on stderr).
 * If port is 0 the kernel picks one; server_port() reports which.
 */
int  server_start(server *s, unsigned short port);

/* The port actually bound, useful when port 0 was requested. */
unsigned short server_port(const server *s);

/* Runs until a shutdown signal arrives. */
void server_run(server *s);

/* Closes the listening socket and disconnects everyone. */
void server_stop(server *s);

/* Installs SIGINT/SIGTERM handlers and ignores SIGPIPE. */
void server_install_signal_handlers(void);

/* Sets O_NONBLOCK on fd. Returns 0 or -1. Exposed for tests. */
int  set_nonblocking(int fd);

#endif /* SERVER_H */
