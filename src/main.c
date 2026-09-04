/*
 * main.c - argument parsing and startup.
 */
#include <stdio.h>
#include <stdlib.h>

#include "server.h"

int main(int argc, char **argv)
{
    unsigned short port = 5555;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [port]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        char *end;
        long value = strtol(argv[1], &end, 10);
        /* strtol, not atoi: atoi cannot distinguish "0" from "abc", so a
         * typo would silently bind a kernel-chosen port. */
        if (*end != '\0' || value < 0 || value > 65535) {
            fprintf(stderr, "invalid port: %s\n", argv[1]);
            return 2;
        }
        port = (unsigned short)value;
    }

    server s;
    server_install_signal_handlers();

    if (server_start(&s, port) != 0)
        return 1;

    /* Printed on stdout and flushed so a supervising script can wait for the
     * line before connecting - the integration tests rely on this. */
    printf("listening on 127.0.0.1:%u\n", server_port(&s));
    fflush(stdout);

    server_run(&s);
    server_stop(&s);
    return 0;
}
