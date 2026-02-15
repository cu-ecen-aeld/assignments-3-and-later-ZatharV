#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t exit_requested = 0;
int server_fd = -1;
int client_fd = -1;

void signal_handler(int sig)
{
    syslog(LOG_INFO, "Caught signal, exiting");
    exit_requested = 1;

    if (client_fd != -1) close(client_fd);
    if (server_fd != -1) close(server_fd);
}

int main(int argc, char *argv[])
{
    struct sockaddr_in serv_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Ensure /var/tmp exists (Buildroot often lacks it)
    if (mkdir("/var/tmp", 0777) == -1 && errno != EEXIST) {
        syslog(LOG_ERR, "Failed to create /var/tmp");
        return -1;
    }

    // Install signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, "socket() failed");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        syslog(LOG_ERR, "bind() failed");
        close(server_fd);
        return -1;
    }

    // Listen
    if (listen(server_fd, 10) < 0) {
        syslog(LOG_ERR, "listen() failed");
        close(server_fd);
        return -1;
    }

    // Accept loop
    while (!exit_requested) {

        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (exit_requested) break;
            syslog(LOG_ERR, "accept() failed");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Receive packet until newline
        char recvbuf[1024];
        char *packet = NULL;
        size_t total_len = 0;

        while (!exit_requested) {
            ssize_t bytes = recv(client_fd, recvbuf, sizeof(recvbuf), 0);
            if (bytes <= 0) break;

            char *newbuf = realloc(packet, total_len + bytes);
            if (!newbuf) {
                syslog(LOG_ERR, "malloc failed");
                free(packet);
                packet = NULL;
                break;
            }
            packet = newbuf;

            memcpy(packet + total_len, recvbuf, bytes);
            total_len += bytes;

            if (memchr(recvbuf, '\n', bytes)) break;
        }

        // Append packet to file
        if (packet) {
            int fd = open(DATAFILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                syslog(LOG_ERR, "open() failed for data file");
            } else {
                write(fd, packet, total_len);
                close(fd);
            }
            free(packet);
        }

        // Send full file back
        int fd = open(DATAFILE, O_RDONLY);
        if (fd >= 0) {
            char filebuf[1024];
            ssize_t r;
            while ((r = read(fd, filebuf, sizeof(filebuf))) > 0) {
                send(client_fd, filebuf, r, 0);
            }
            close(fd);
        }

        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        close(client_fd);
        client_fd = -1;
    }

    // Cleanup
    if (client_fd != -1) close(client_fd);
    if (server_fd != -1) close(server_fd);

    remove(DATAFILE);

    closelog();
    return 0;
}

