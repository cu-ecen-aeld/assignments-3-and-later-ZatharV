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

#define PORT 9000
#define DATAFILE "/var/tmp/aesdsocketdata"

static volatile sig_atomic_t exit_requested = 0;
int sockfd = -1;
int clientfd = -1;

void signal_handler(int sig)
{
    syslog(LOG_INFO, "Caught signal, exiting");
    exit_requested = 1;
}

int main(int argc, char *argv[])
{
    struct sockaddr_in serv_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Install signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 1. Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        syslog(LOG_ERR, "socket() failed");
        perror("socket");
        return -1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Bind to port 9000
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(PORT);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        syslog(LOG_ERR, "bind() failed");
        perror("bind");
        close(sockfd);
        return -1;
    }

    // 3. Listen
    if (listen(sockfd, 10) < 0) {
        syslog(LOG_ERR, "listen() failed");
        perror("listen");
        close(sockfd);
        return -1;
    }

    // 4. Accept loop
    while (!exit_requested) {

        clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);
        if (clientfd < 0) {
            if (exit_requested) break;  // interrupted by signal
            syslog(LOG_ERR, "accept() failed");
            perror("accept");
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Receive buffer
        char recvbuf[1024];
        size_t total_len = 0;
        char *packet = NULL;

        // 5. Receive until newline
        while (!exit_requested) {
            ssize_t bytes = recv(clientfd, recvbuf, sizeof(recvbuf), 0);
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

        // 6. Append packet to file
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

        // 7. Send full file back
        int fd = open(DATAFILE, O_RDONLY);
        if (fd >= 0) {
            char filebuf[1024];
            ssize_t r;
            while ((r = read(fd, filebuf, sizeof(filebuf))) > 0) {
                send(clientfd, filebuf, r, 0);
            }
            close(fd);
        }

        // 8. Close connection
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        close(clientfd);
        clientfd = -1;
    }

    // Cleanup on exit
    if (clientfd != -1) close(clientfd);
    if (sockfd != -1) close(sockfd);

    remove(DATAFILE);

    closelog();
    return 0;
}

