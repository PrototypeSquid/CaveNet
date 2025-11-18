// cave_client_windows.c - Why does windows have to be cantankerous

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")

#define BUF_SIZE 4096

// ANSI colors
#define COL_RESET   "\x1b[0m"
#define COL_SYS     "\x1b[90m"  // gray
#define COL_ME      "\x1b[32m"  // green
#define COL_NICK    "\x1b[36m"  // cyan
#define COL_ERR     "\x1b[31m"  // red

static SOCKET g_sock = INVALID_SOCKET;
static char current_nick[32] = "";

// ---------- console helpers ----------

static void enable_ansi_colors(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;

    DWORD mode = 0;
    if (!GetConsoleMode(hOut, &mode)) return;

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, mode);
}

static void print_prompt(void) {
    printf("> ");
    fflush(stdout);
}

// ---------- protocol formatting ----------

static void handle_server_line(const char *line) {
    // SYS :text -> system msg
    if (strncmp(line, "SYS :", 5) == 0) {
        const char *msg = line + 5;
        printf("\n" COL_SYS "[system] %s" COL_RESET "\n", msg);
        return;
    }

    // MSG @nick :text
    if (strncmp(line, "MSG ", 4) == 0) {
        const char *p = line + 4;

        if (*p == '@') p++;

        char nick[32];
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

    // fallback: show any other line (PROFILE, WELCOME, ERR, etc.)
    if (*line) {
        printf("\n[server] %s\n", line);
    }
}

// ---------- network thread ----------

DWORD WINAPI net_thread(LPVOID lpParam) {
    char buf[BUF_SIZE];
    char linebuf[BUF_SIZE];
    int line_len = 0;

    (void)lpParam; // unused

    while (1) {
        int n = recv(g_sock, buf, BUF_SIZE - 1, 0);
        if (n <= 0) {
            printf("\n" COL_ERR "[disconnected from server]" COL_RESET "\n");
            break;
        }

        for (int i = 0; i < n; i++) {
            char ch = buf[i];

            if (ch == '\n') {
                linebuf[line_len] = '\0';
                // trim CR
                if (line_len > 0 && linebuf[line_len - 1] == '\r') {
                    linebuf[line_len - 1] = '\0';
                }
                handle_server_line(linebuf);
                line_len = 0;
            } else if (line_len < BUF_SIZE - 1) {
                linebuf[line_len++] = ch;
            }
        }
    }

    return 0;
}

// ---------- main ----------

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <server_ip> <port>\n"
            "Example: %s 127.0.0.1 7777\n",
            argv[0], argv[0]);
        return 1;
    }

    const char *server_ip = argv[1];
    int port = atoi(argv[2]);

    enable_ansi_colors();

    // --- WinSock init ---
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", r);
        return 1;
    }

    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) {
        fprintf(stderr, "socket() failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);

    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) <= 0) {
        fprintf(stderr, "inet_pton failed\n");
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    if (connect(g_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "connect() failed: %d\n", WSAGetLastError());
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    printf("Connected to %s:%d\n", server_ip, port);
    printf("Commands:\n");
    printf("  /nick NAME     set your nickname\n");
    printf("  /quit          exit\n");
    printf("Type anything else to send a chat message.\n");

    // --- start network thread ---
    HANDLE hThread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (!hThread) {
        fprintf(stderr, "CreateThread failed\n");
        closesocket(g_sock);
        WSACleanup();
        return 1;
    }

    // --- main thread: read user input ---
    char inbuf[BUF_SIZE];

    while (1) {
        print_prompt();

        if (!fgets(inbuf, sizeof(inbuf), stdin)) {
            break;
        }

        size_t len = strlen(inbuf);
        if (len && inbuf[len - 1] == '\n') {
            inbuf[len - 1] = '\0';
            len--;
        }
        if (len == 0) continue;

        // slash commands
        if (inbuf[0] == '/') {
            if (strcmp(inbuf, "/quit") == 0) {
                printf("Bye.\n");
                break;
            }

            if (strncmp(inbuf, "/nick ", 6) == 0) {
                const char *name = inbuf + 6;
                if (*name == '\0') {
                    printf(COL_ERR "Usage: /nick NAME" COL_RESET "\n");
                    continue;
                }
                char line[BUF_SIZE];
                snprintf(line, sizeof(line), "NICK %s", name);
                send(g_sock, line, (int)strlen(line), 0);
                send(g_sock, "\r\n", 2, 0);

                snprintf(current_nick, sizeof(current_nick), "%s", name);
                continue;
            }

            printf(COL_ERR "Unknown command: %s" COL_RESET "\n", inbuf);
            continue;
        }

        // normal chat
        char line[BUF_SIZE];
        snprintf(line, sizeof(line), "MSG :%s", inbuf);
        send(g_sock, line, (int)strlen(line), 0);
        send(g_sock, "\r\n", 2, 0);
    }

    // cleanup
    shutdown(g_sock, SD_BOTH);
    closesocket(g_sock);
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    WSACleanup();
    return 0;
}
