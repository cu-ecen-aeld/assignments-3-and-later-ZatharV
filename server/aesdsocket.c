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

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUF_SIZE 1024

int server_fd = -1;
int data_fd = -1;

// Signal handler to ensure clean exit and file removal
void handle_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        
        if (server_fd != -1) close(server_fd);
        // Delete the data file as required by the assignment
        remove(DATA_FILE);
        closelog();
        exit(0);
    }
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
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Create and configure socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed: %m");
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "Bind failed: %m");
        close(server_fd);
        return -1;
    }

    // Daemonize if requested via -d argument
    if (daemon_mode) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0); // Parent exits
        
        if (setsid() < 0) return -1;
        chdir("/");
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    if (listen(server_fd, 10) < 0) {
        syslog(LOG_ERR, "Listen failed: %m");
        close(server_fd);
        return -1;
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd == -1) continue;

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        // Open/Create the data file for appending
        data_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        
        char *recv_buffer = malloc(BUF_SIZE);
        ssize_t bytes_received;
        size_t total_received = 0;

        // Loop receiving until a newline (\n) is found
        while ((bytes_received = recv(client_fd, recv_buffer + total_received, BUF_SIZE - 1, 0)) > 0) {
            total_received += bytes_received;
            // Check for newline in the newly received chunk
            if (memchr(recv_buffer + (total_received - bytes_received), '\n', bytes_received)) {
                break;
            }
            recv_buffer = realloc(recv_buffer, total_received + BUF_SIZE);
        }

        // Write the full packet to the file
        write(data_fd, recv_buffer, total_received);

        // --- CRITICAL FIX: RESET FILE POINTER TO START ---
        lseek(data_fd, 0, SEEK_SET);
        

        // Read the entire cumulative file and send back to client
        char send_buf[BUF_SIZE];
        ssize_t r_bytes;
        while ((r_bytes = read(data_fd, send_buf, BUF_SIZE)) > 0) {
            send(client_fd, send_buf, r_bytes, 0);
        }

        // Cleanup for this connection
        close(client_fd);
        close(data_fd);
        free(recv_buffer);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    return 0;
}
