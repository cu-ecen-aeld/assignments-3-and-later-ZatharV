#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUF_SIZE 1024

int server_fd = -1;

// Signal handler
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        
        // Close the server socket to break the accept loop
        if (server_fd != -1) {
            close(server_fd);
            server_fd = -1;
        }
    }
}

void cleanup() {
    remove(DATA_FILE);
    closelog();
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Setup signal handling
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Socket address info setup
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, PORT, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    server_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (server_fd == -1) {
        freeaddrinfo(res);
        return -1;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        freeaddrinfo(res);
        close(server_fd);
        return -1;
    }

    if (bind(server_fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        close(server_fd);
        return -1;
    }
    freeaddrinfo(res);

    // Daemonization block
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0); // Parent exits successfully
        
        setsid();
        chdir("/");
        int dev_null = open("/dev/null", O_RDWR);
        dup2(dev_null, STDIN_FILENO);
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        close(dev_null);
    }

    if (listen(server_fd, 10) != 0) {
        close(server_fd);
        return -1;
    }

    while (server_fd != -1) {
        struct sockaddr_in client_addr;
        socklen_t addr_size = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_size);
        
        if (server_fd == -1) break; // Check if signal handler closed socket
        if (client_fd == -1) continue;

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Open/Create file
        int fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0666);
        if (fd == -1) {
            close(client_fd);
            continue;
        }

        char buf[BUF_SIZE];
        ssize_t bytes_recv;
        
        // Receive and write to file until newline
        while ((bytes_recv = recv(client_fd, buf, BUF_SIZE, 0)) > 0) {
            if (write(fd, buf, bytes_recv) == -1) break;
            if (memchr(buf, '\n', bytes_recv)) break;
        }

        // Send file content back to client
        lseek(fd, 0, SEEK_SET);
        ssize_t bytes_read;
        while ((bytes_read = read(fd, buf, BUF_SIZE)) > 0) {
            send(client_fd, buf, bytes_read, 0);
        }
        
        close(fd);
        close(client_fd);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    cleanup();
    return 0;
}