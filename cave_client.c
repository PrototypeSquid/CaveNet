// cave_client.c 
#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     // for strcasecmp (if needed later)
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

#define BUF_SIZE 4096

// Simple ANSI color codes for a "Discord-ish" feel
#define COL_RESET   "\x1b[0m"
#define COL_SYS     "\x1b[90m"  // gray
#define COL_NICK    "\x1b[36m"  // cyan
#define COL_ME      "\x1b[32m"  // green
#define COL_ERR     "\x1b[31m"  // red
#define COL_PROFILE "\x1b[35m"  // magenta

#define CAVE_NICK_MAX        32
#define CAVE_DISPLAY_MAX     64
#define CAVE_BIO_MAX        512
#define CAVE_PRONOUNS_MAX    32

// Track the nick we THINK we are (based on /nick)
static char current_nick[CAVE_NICK_MAX] = "";

// For assembling network data into lines
static char net_buf[BUF_SIZE];
static size_t net_len = 0;

// Simple "currently viewed profile" state
typedef struct {
    int active;
    char nick[CAVE_NICK_MAX];
    char display_name[CAVE_DISPLAY_MAX];
    char pronouns[CAVE_PRONOUNS_MAX];
    char bio[CAVE_BIO_MAX];
} profile_view_t;

static profile_view_t pv = {0};

static void print_prompt(void) {
    printf("> ");
    fflush(stdout);
}

static void send_line(int fd, const char *line) {
    size_t len = strlen(line);
    send(fd, line, len, 0);
    send(fd, "\r\n", 2, 0);
}

// ------------------------ PROFILE VIEW RENDERING ------------------------

static void clear_profile_view(void) {
    pv.active = 0;
    pv.nick[0] = '\0';
    pv.display_name[0] = '\0';
    pv.pronouns[0] = '\0';
    pv.bio[0] = '\0';
}

static void show_profile_view(void) {
    if (!pv.active) return;

    printf(COL_PROFILE "----- Profile: %s -----" COL_RESET "\n", pv.nick);

    if (pv.display_name[0]) {
        printf("Display name: %s\n", pv.display_name);
    }
    if (pv.pronouns[0]) {
        printf("Pronouns: %s\n", pv.pronouns);
    }
    if (pv.bio[0]) {
        printf("Bio: %s\n", pv.bio);
    }

    printf(COL_PROFILE "---------------------------" COL_RESET "\n");
}

// ------------------------ SERVER MESSAGE PARSING ------------------------

static void handle_net_line(const char *line) {
    // System messages from server
    if (strncmp(line, "SYS :", 5) == 0) {
        const char *msg = line + 5;
        printf("\n" COL_SYS "[system] %s" COL_RESET "\n", msg);
        return;
    }

    // Chat messages: MSG @nick :text
    if (strncmp(line, "MSG ", 4) == 0) {
        const char *p = line + 4;

        // Example line: "MSG @surge :hello"
        if (*p == '@') p++;

        char nick[CAVE_NICK_MAX];
        int n = 0;
        while (*p && *p != ' ' && *p != ':' && n < (int)sizeof(nick) - 1) {
            nick[n++] = *p++;
        }
        nick[n] = '\0';

        const char *colon = strchr(line, ':');
        const char *body = colon ? (colon + 1) : "";

        const char *color = (current_nick[0] && strcmp(current_nick, nick) == 0)
                            ? COL_ME : COL_NICK;

        printf("\n%s%s%s: %s\n", color, nick, COL_RESET, body);
        return;
    }

    // PROFILE DATA <nick> FIELD :value
    if (strncmp(line, "PROFILE DATA ", 13) == 0) {
        const char *p = line + 13;
        while (*p == ' ') p++;

        // read nick
        char nick[CAVE_NICK_MAX];
        int n = 0;
        while (*p && *p != ' ' && n < (int)sizeof(nick) - 1) {
            nick[n++] = *p++;
        }
        nick[n] = '\0';

        while (*p == ' ') p++;

        // read field name
        char field[32];
        n = 0;
        while (*p && *p != ' ' && *p != ':' && n < (int)sizeof(field) - 1) {
            field[n++] = *p++;
        }
        field[n] = '\0';

        const char *colon = strchr(p, ':');
        if (!colon) return;
        const char *value = colon + 1;
        while (*value == ' ') value++;

        // If new profile, reset
        if (!pv.active || strcmp(pv.nick, nick) != 0) {
            clear_profile_view();
            pv.active = 1;
            snprintf(pv.nick, sizeof(pv.nick), "%s", nick);
        }

        if (strcmp(field, "DISPLAYNAME") == 0) {
            snprintf(pv.display_name, sizeof(pv.display_name), "%s", value);
        } else if (strcmp(field, "PRONOUNS") == 0) {
            snprintf(pv.pronouns, sizeof(pv.pronouns), "%s", value);
        } else if (strcmp(field, "BIO") == 0) {
            snprintf(pv.bio, sizeof(pv.bio), "%s", value);
        }

        return;
    }

    // PROFILE END <nick>  -> show the block
    if (strncmp(line, "PROFILE END ", 12) == 0) {
        const char *p = line + 12;
        while (*p == ' ') p++;

        char nick[CAVE_NICK_MAX];
        int n = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' &&
               n < (int)sizeof(nick) - 1) {
            nick[n++] = *p++;
        }
        nick[n] = '\0';

        // Only show if this matches tracked profile
        if (pv.active && strcmp(pv.nick, nick) == 0) {
            printf("\n");
            show_profile_view();
            clear_profile_view();
        }
        return;
    }

    // PROFILE ERR ...
    if (strncmp(line, "PROFILE ERR ", 12) == 0) {
        const char *err = line + 12;
        printf("\n" COL_ERR "[profile error] %s" COL_RESET "\n", err);
        return;
    }

    // Fallback: raw line (useful during debugging)
    if (*line) {
        printf("\n[raw] %s\n", line);
    }
}

// Assemble lines from the TCP stream and feed into handle_net_line
static int handle_net_data(int fd) {
    ssize_t n = recv(fd, net_buf + net_len, BUF_SIZE - net_len - 1, 0);
    if (n <= 0) {
        return -1; // disconnected or error
    }

    net_len += (size_t)n;
    net_buf[net_len] = '\0';

    char *start = net_buf;
    for (;;) {
        char *newline = strstr(start, "\n");
        if (!newline) break;

        *newline = '\0';
        if (newline > start && *(newline - 1) == '\r') {
            *(newline - 1) = '\0';
        }

        if (*start) {
            handle_net_line(start);
        }

        start = newline + 1;
    }

    size_t remaining = net_buf + net_len - start;
    memmove(net_buf, start, remaining);
    net_len = remaining;

    return 0;
}

// ------------------------ USER INPUT HANDLING ------------------------

static void handle_user_input(int fd) {
    char inbuf[BUF_SIZE];

    if (!fgets(inbuf, sizeof(inbuf), stdin)) {
        // EOF on stdin, just exit
        printf("\nExiting.\n");
        exit(0);
    }

    // strip trailing newline
    size_t len = strlen(inbuf);
    if (len && inbuf[len - 1] == '\n') {
        inbuf[len - 1] = '\0';
        len--;
    }

    // Empty line? ignore
    if (len == 0) return;

    // Slash commands
    if (inbuf[0] == '/') {
        // /quit
        if (strcmp(inbuf, "/quit") == 0) {
            printf("Bye!\n");
            exit(0);
        }

        // /nick NAME
        if (strncmp(inbuf, "/nick ", 6) == 0) {
            const char *name = inbuf + 6;
            if (*name == '\0') {
                printf(COL_ERR "Usage: /nick NAME" COL_RESET "\n");
                return;
            }
            char line[BUF_SIZE];
            snprintf(line, sizeof(line), "NICK %s", name);
            send_line(fd, line);
            snprintf(current_nick, sizeof(current_nick), "%s", name);
            return;
        }

        // /profile get NICK
        if (strncmp(inbuf, "/profile get ", 13) == 0) {
            const char *nick = inbuf + 13;
            if (*nick == '\0') {
                printf(COL_ERR "Usage: /profile get NICK" COL_RESET "\n");
                return;
            }
            char line[BUF_SIZE];
            snprintf(line, sizeof(line), "PROFILE GET %s", nick);
            send_line(fd, line);
            return;
        }

        // /profile set displayname TEXT
        if (strncmp(inbuf, "/profile set displayname ", 25) == 0) {
            const char *value = inbuf + 25;
            if (*value == '\0') {
                printf(COL_ERR "Usage: /profile set displayname TEXT" COL_RESET "\n");
                return;
            }
            char line[BUF_SIZE];
            snprintf(line, sizeof(line), "PROFILE SET DISPLAYNAME :%s", value);
            send_line(fd, line);
            return;
        }

        // /profile set bio TEXT
        if (strncmp(inbuf, "/profile set bio ", 17) == 0) {
            const char *value = inbuf + 17;
            if (*value == '\0') {
                printf(COL_ERR "Usage: /profile set bio TEXT" COL_RESET "\n");
                return;
            }
            char line[BUF_SIZE];
            snprintf(line, sizeof(line), "PROFILE SET BIO :%s", value);
            send_line(fd, line);
            return;
        }

        // /profile set pronouns TEXT
        if (strncmp(inbuf, "/profile set pronouns ", 22) == 0) {
            const char *value = inbuf + 22;
            if (*value == '\0') {
                printf(COL_ERR "Usage: /profile set pronouns TEXT" COL_RESET "\n");
                return;
            }
            char line[BUF_SIZE];
            snprintf(line, sizeof(line), "PROFILE SET PRONOUNS :%s", value);
            send_line(fd, line);
            return;
        }

        // Unknown slash command
        printf(COL_ERR "Unknown command: %s" COL_RESET "\n", inbuf);
        printf("Known: /nick, /profile get, /profile set displayname|bio|pronouns, /quit\n");
        return;
    }

    // Default: send as chat message
    char line[BUF_SIZE];
    snprintf(line, sizeof(line), "MSG :%s", inbuf);
    send_line(fd, line);
}

// ------------------------ MAIN ------------------------

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "Usage: %s <server_ip> <port>\n\n"
                "Example: %s 127.0.0.1 7777\n",
                argv[0], argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(fd);
        return 1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    printf("Connected to %s:%d\n", server_ip, port);
    printf("Type: /nick NAME to set your nickname\n");
    printf("      /profile set displayname TEXT\n");
    printf("      /profile set bio TEXT\n");
    printf("      /profile set pronouns TEXT\n");
    printf("      /profile get NICK\n");
    print_prompt();

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(fd, &rfds);
        int maxfd = (fd > STDIN_FILENO) ? fd : STDIN_FILENO;

        int ready = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select");
            break;
        }

        // Keyboard input
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            handle_user_input(fd);
            print_prompt();
        }

        // Network input
        if (FD_ISSET(fd, &rfds)) {
            if (handle_net_data(fd) < 0) {
                printf("\nDisconnected from server.\n");
                break;
            }
            print_prompt();
        }
    }

    close(fd);
    return 0;
}
