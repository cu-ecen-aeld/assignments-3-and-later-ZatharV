/**
 * 
 * @file    aesdsocket.c
 * @author  Atharv More
 * @ref     https://gemini.google.com/share/3aa11fc7e916
 * @date    02/14/2026
 * 
 */

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
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) 
    {
        syslog(LOG_INFO, "Sinal received: %s", (sig == SIGINT) ? "SIGINT" : ((sig == SIGTERM) ? "SIGTERM": ""));
        
        // If server_fd is created then close ir
        if (server_fd != -1) 
        {
            close(server_fd);
        }
        remove(DATA_FILE);
        closelog();
        exit(0);
    }
}

/**
 * @brief   Checks if the program should be in daemon mode
 * 
 * @return  1 if daemon
 *          0 otherwise
 */
int is_daemon_mode(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "-d") == 0)
        return 1;
    else
        return 0;
}

/**
 * @brief Sets signal Handing
 */
void set_signal_handling()
{
    // Setup signal handling
    struct sigaction sig_action = {0};
    sig_action.sa_handler = signal_handler;
    sigaction(SIGINT, &sig_action, NULL);
    sigaction(SIGTERM, &sig_action, NULL);
}

int main(int argc, char *argv[]) {
    int daemon_mode = is_daemon_mode(argc, argv);

    // Open Logs
    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Signal handling is important for SIGINT and SIGTERM
    set_signal_handling();

    // Create and configure socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) 
    {
        syslog(LOG_ERR, "Create Socket: Failed");
        closelog();
        return -1;
    }

    int opt = 1;
    if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        syslog(LOG_ERR, "setsockopt: Failed");
        close(server_fd);
        closelog();
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);        // Convert port to Network Byte Order
    addr.sin_addr.s_addr = INADDR_ANY;  // Bind to all available interfaces

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "Bind: failed");
        close(server_fd);
        return -1;
    }


    // In daemon mode, it should run in the background
    if (daemon_mode) 
    {
        pid_t pid = fork();

        if (pid < 0)
        {
            syslog(LOG_ERR, "Forking failed in daemon mode");
            closelog();
            return -1;
        }

        if (pid > 0) 
            exit(0); // In daemon mode, parent exits
        
        if (setsid() < 0) 
        {
            return -1;
        }

        chdir("/"); // Change to root dir

        // Redirect standard files to /dev/null
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    // Listen to connection
    if (listen(server_fd, 10) < 0)
    {
        syslog(LOG_ERR, "Listen: Failed");
        close(server_fd);
        return -1;
    }

    while(1) 
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd == -1) 
        {
            continue;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        data_fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        
        char *recv_buffer = malloc(BUF_SIZE);
        ssize_t bytes_received;
        size_t total_received = 0;

        while ((bytes_received = recv(client_fd, recv_buffer + total_received, BUF_SIZE - 1, 0)) > 0) 
        {
            total_received += bytes_received;
            if (memchr(recv_buffer + (total_received - bytes_received), '\n', bytes_received)) {
                break;
            }
            recv_buffer = realloc(recv_buffer, total_received + BUF_SIZE);
        }


        write(data_fd, recv_buffer, total_received);
        lseek(data_fd, 0, SEEK_SET);
        

        char send_buf[BUF_SIZE];
        ssize_t r_bytes;
        while ((r_bytes = read(data_fd, send_buf, BUF_SIZE)) > 0) 
        {
            send(client_fd, send_buf, r_bytes, 0);
        }

        close(client_fd);
        close(data_fd);
        free(recv_buffer);
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
    }

    return 0;
}
