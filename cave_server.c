// cave_server.c - CAVE chat server with basic profile support
#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     // for strcasecmp
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define CAVE_PORT 7777
#define MAX_CLIENTS 32
#define BUF_SIZE 4096

// Profile-related limits
#define CAVE_NICK_MAX        32
#define CAVE_DISPLAY_MAX     64
#define CAVE_BIO_MAX        512
#define CAVE_PRONOUNS_MAX    16

typedef struct {
    int fd;                                  // socket descriptor
    char nick[CAVE_NICK_MAX];                // username
    char display_name[CAVE_DISPLAY_MAX];     // "pretty" name
    char bio[CAVE_BIO_MAX];                  // custom markup bio
    char pronouns[CAVE_PRONOUNS_MAX];        // e.g. "he/him, she/her, they/them"

    char buf[BUF_SIZE];                      // input buffer
    size_t buf_len;                          // how much of buf is used
} client_t;

static client_t clients[MAX_CLIENTS];

// ----------------------- utility functions -----------------------

static void client_init(client_t *c) {
    c->fd = -1;
    c->nick[0] = '\0';
    c->display_name[0] = '\0';
    c->bio[0] = '\0';
    c->pronouns[0] = '\0';
    c->buf_len = 0;
}

static void send_line(int fd, const char *line) {
    size_t len = strlen(line);
    send(fd, line, len, 0);
    send(fd, "\r\n", 2, 0);
}

static void broadcast_line(int from_fd, const char *line) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].fd != from_fd) {
            send_line(clients[i].fd, line);
        }
    }
}

// Find a client by nickname (exact match)
static client_t *find_client_by_nick(const char *nick) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 &&
            clients[i].nick[0] != '\0' &&
            strcmp(clients[i].nick, nick) == 0) {
            return &clients[i];
        }
    }
    return NULL;
}

// ----------------------- PROFILE command handler -----------------------

static void handle_profile_command(client_t *c, const char *args) {
    // args is everything after "PROFILE "
    // e.g. "SET DISPLAYNAME :Mothman" or "GET mothman"

    // trim leading spaces
    while (*args == ' ') args++;

    // ----- PROFILE SET -----
    if (strncmp(args, "SET ", 4) == 0) {
        const char *p = args + 4;        // skip "SET "
        while (*p == ' ') p++;          // trim spaces

        // p now at field name, e.g. DISPLAYNAME / BIO / PRONOUNS
        char field[32];
        int n = 0;
        while (*p && *p != ' ' && *p != ':' && n < (int)sizeof(field) - 1) {
            field[n++] = *p++;
        }
        field[n] = '\0';

        while (*p == ' ') p++;

        const char *colon = strchr(p, ':');
        if (!colon) {
            send_line(c->fd, "PROFILE ERR SYNTAX");
            return;
        }
        const char *value = colon + 1;

        // trim leading spaces from value
        while (*value == ' ') value++;

        if (strcasecmp(field, "DISPLAYNAME") == 0) {
            if (strlen(value) >= CAVE_DISPLAY_MAX) {
                send_line(c->fd, "PROFILE ERR VALUE_TOO_LONG");
                return;
            }
            snprintf(c->display_name, sizeof(c->display_name), "%s", value);
            send_line(c->fd, "PROFILE OK DISPLAYNAME");

        } else if (strcasecmp(field, "BIO") == 0) {
            if (strlen(value) >= CAVE_BIO_MAX) {
                send_line(c->fd, "PROFILE ERR VALUE_TOO_LONG");
                return;
            }
            snprintf(c->bio, sizeof(c->bio), "%s", value);
            send_line(c->fd, "PROFILE OK BIO");

        } else if (strcasecmp(field, "PRONOUNS") == 0) {
            if (strlen(value) >= CAVE_PRONOUNS_MAX) {
                send_line(c->fd, "PROFILE ERR VALUE_TOO_LONG");
                return;
            }
            snprintf(c->pronouns, sizeof(c->pronouns), "%s", value);
            send_line(c->fd, "PROFILE OK PRONOUNS");

        } else {
            send_line(c->fd, "PROFILE ERR FIELD");
        }

    // ----- PROFILE GET -----
    } else if (strncmp(args, "GET ", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ') p++;

        char target_nick[CAVE_NICK_MAX];
        int n = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' &&
               n < (int)sizeof(target_nick) - 1) {
            target_nick[n++] = *p++;
        }
        target_nick[n] = '\0';

        if (target_nick[0] == '\0') {
            send_line(c->fd, "PROFILE ERR SYNTAX");
            return;
        }

        client_t *target = find_client_by_nick(target_nick);
        if (!target) {
            char line[128];
            snprintf(line, sizeof(line),
                     "PROFILE ERR NOTFOUND %s", target_nick);
            send_line(c->fd, line);
            return;
        }

        char line[BUF_SIZE];

        if (target->display_name[0]) {
            snprintf(line, sizeof(line),
                     "PROFILE DATA %s DISPLAYNAME :%s",
                     target->nick, target->display_name);
            send_line(c->fd, line);
        }

        if (target->pronouns[0]) {
            snprintf(line, sizeof(line),
                     "PROFILE DATA %s PRONOUNS :%s",
                     target->nick, target->pronouns);
            send_line(c->fd, line);
        }

        if (target->bio[0]) {
            snprintf(line, sizeof(line),
                     "PROFILE DATA %s BIO :%s",
                     target->nick, target->bio);
            send_line(c->fd, line);
        }

        snprintf(line, sizeof(line),
                 "PROFILE END %s", target->nick);
        send_line(c->fd, line);

    } else {
        send_line(c->fd, "PROFILE ERR SYNTAX");
    }
}

// ----------------------- main command handler -----------------------

static void handle_command(client_t *c, const char *line) {
    if (strncmp(line, "NICK ", 5) == 0) {
        snprintf(c->nick, sizeof(c->nick), "%s", line + 5);
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "SYS :%s joined",
                 c->nick[0] ? c->nick : "anonymous");
        broadcast_line(c->fd, msg);
        send_line(c->fd, "SYS :nickname set");

    } else if (strncmp(line, "MSG ", 4) == 0) {
        const char *text = line + 4;
        const char *colon = strchr(text, ':');
        if (colon) {
            colon++;
        } else {
            colon = text;
        }

        char msg[BUF_SIZE];
        snprintf(msg, sizeof(msg),
                 "MSG @%s :%s",
                 c->nick[0] ? c->nick : "anon",
                 colon);
        broadcast_line(c->fd, msg);
        send_line(c->fd, msg);

    } else if (strcmp(line, "PING") == 0) {
        send_line(c->fd, "PONG");

    } else if (strncmp(line, "PROFILE ", 8) == 0) {
        handle_profile_command(c, line + 8);

    } else {
        send_line(c->fd, "ERR :unknown command");
    }
}

// ----------------------- socket read loop per client -----------------------

static void handle_client_data(client_t *c) {
    char *buf = c->buf;
    ssize_t n = recv(c->fd,
                     buf + c->buf_len,
                     BUF_SIZE - c->buf_len - 1,
                     0);

    if (n <= 0) {
        // disconnect
        close(c->fd);
        client_init(c);
        return;
    }

    c->buf_len += (size_t)n;
    buf[c->buf_len] = '\0';

    char *start = buf;
    for (;;) {
        char *newline = strstr(start, "\n");
        if (!newline) break;

        *newline = '\0';
        if (newline > start && *(newline - 1) == '\r') {
            *(newline - 1) = '\0';
        }

        if (*start != '\0') {
            handle_command(c, start);
        }

        start = newline + 1;
    }

    size_t remaining = c->buf + c->buf_len - start;
    memmove(c->buf, start, remaining);
    c->buf_len = remaining;
}

// ----------------------- main server loop -----------------------

int main(void) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CAVE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 8) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_init(&clients[i]);
    }

    printf("CAVE server listening on port %d\n", CAVE_PORT);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1) {
                FD_SET(clients[i].fd, &rfds);
                if (clients[i].fd > maxfd) maxfd = clients[i].fd;
            }
        }

        int ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        // new connection
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                int assigned = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd == -1) {
                        clients[i].fd = cfd;
                        clients[i].buf_len = 0;
                        clients[i].nick[0] = '\0';
                        clients[i].display_name[0] = '\0';
                        clients[i].bio[0] = '\0';
                        clients[i].pronouns[0] = '\0';

                        send_line(cfd, "WELCOME CAVE/0.1");
                        assigned = 1;
                        break;
                    }
                }
                if (!assigned) {
                    send_line(cfd, "ERR :server full");
                    close(cfd);
                }
            }
        }

        // existing clients
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 && FD_ISSET(clients[i].fd, &rfds)) {
                handle_client_data(&clients[i]);
            }
        }
    }

    close(listen_fd);
    return 0;
}
