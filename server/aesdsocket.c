#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#define PORT "9000"
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUF_SIZE 1024

int server_fd = -1;
int data_fd = -1;

// Signal handler for graceful shutdown
void handle_signal(int sig) {
    syslog(LOG_INFO, "Caught signal, exiting");
    if (server_fd != -1) close(server_fd);
    if (data_fd != -1) close(data_fd);
    remove(DATA_FILE);
    closelog();
    exit(0);
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = 1;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Setup signal handling
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Create Socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9000);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        return -1;
    }

    // Daemonize if requested
    if (daemon_mode) {
        if (fork() > 0) exit(0);
        setsid();
        chdir("/");
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    listen(server_fd, 10);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        data_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        
        char *buffer = malloc(BUF_SIZE);
        ssize_t bytes_received;
        size_t total_received = 0;

        // Receive until newline
        while ((bytes_received = recv(client_fd, buffer + total_received, BUF_SIZE - 1, 0)) > 0) {
            total_received += bytes_received;
            if (memchr(buffer, '\n', total_received)) break;
            buffer = realloc(buffer, total_received + BUF_SIZE);
        }

        // Write to file
        write(data_fd, buffer, total_received);

        // THE FIX: Rewind file to beginning
        lseek(data_fd, 0, SEEK_SET);

        // Send whole file back
        char send_buf[BUF_SIZE];
        ssize_t r_bytes;
        while ((r_bytes = read(data_fd, send_buf, BUF_SIZE)) > 0) {
            send(client_fd, send_buf, r_bytes, 0);
        }

        close(client_fd);
        close(data_fd);
        free(buffer);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    return 0;
}
