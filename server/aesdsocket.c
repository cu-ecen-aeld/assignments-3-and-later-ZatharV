/**
 * @file    aesdsocket.c
 * @author  Atharv More
 * @date    02/22/2026
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
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <sys/stat.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUF_SIZE 1024
#define MAX_FILE_SIZE (1024 * 1024 * 1024) // 1GB Safety Limit

// Ensure SLIST_FOREACH_SAFE is defined for older systems
#ifndef SLIST_FOREACH_SAFE
#define SLIST_FOREACH_SAFE(var, head, field, tvar)           \
    for ((var) = SLIST_FIRST((head));                        \
        (var) && ((tvar) = SLIST_NEXT((var), field), 1);     \
        (var) = (tvar))
#endif

// Thread structure for the linked list
struct thread_data {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool thread_complete;
    SLIST_ENTRY(thread_data) entries;
};

// Global variables
int server_fd = -1;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile bool exit_requested = false;

// Initialize the head of the linked list
SLIST_HEAD(slisthead, thread_data) head = SLIST_HEAD_INITIALIZER(head);

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    syslog(LOG_INFO, "Caught signal, exiting");
    exit_requested = true;
    if (server_fd != -1) {
        shutdown(server_fd, SHUT_RDWR);
    }
}

// Thread function to handle client communication
void* thread_handler(void* arg) {
    struct thread_data* data = (struct thread_data*)arg;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &data->client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

    char *recv_buffer = malloc(BUF_SIZE);
    if (!recv_buffer) goto thread_exit;

    ssize_t bytes_received;
    size_t total_received = 0;

    // Receive data until newline is found
    while (!exit_requested) {
        bytes_received = recv(data->client_fd, recv_buffer + total_received, BUF_SIZE - 1, 0);
        if (bytes_received <= 0) break;

        // Check only the newly arrived bytes for a newline
        char *newline_ptr = memchr(recv_buffer + total_received, '\n', bytes_received);
        total_received += bytes_received;

        if (newline_ptr != NULL) break;

        // Reallocate buffer if needed
        char *new_ptr = realloc(recv_buffer, total_received + BUF_SIZE);
        if (!new_ptr) {
            syslog(LOG_ERR, "Failed to reallocate buffer");
            free(recv_buffer);
            goto thread_exit;
        }
        recv_buffer = new_ptr;
    }

    // Synchronize file access
    pthread_mutex_lock(&file_mutex);
    
    // Safety check for disk space
    struct stat st;
    if (stat(DATA_FILE, &st) == 0 && st.st_size > MAX_FILE_SIZE) {
        syslog(LOG_ERR, "Safety limit reached! File > 1GB. Stopping write.");
    } else {
        int fd = open(DATA_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        if (fd != -1) {
            write(fd, recv_buffer, total_received);
            lseek(fd, 0, SEEK_SET);

            char send_buf[BUF_SIZE];
            ssize_t r_bytes;
            while ((r_bytes = read(fd, send_buf, BUF_SIZE)) > 0) {
                send(data->client_fd, send_buf, r_bytes, 0);
            }
            close(fd);
        }
    }
    pthread_mutex_unlock(&file_mutex);

    free(recv_buffer);
    close(data->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);

thread_exit:
    data->thread_complete = true;
    return arg;
}

// Timer thread function to append RFC 2822 timestamps every 10 seconds
void* timer_handler(void* arg) {
    while (!exit_requested) {
        // Robust sleep that handles interruptions
        unsigned int time_to_sleep = 10;
        while (time_to_sleep > 0 && !exit_requested) {
            time_to_sleep = sleep(time_to_sleep);
        }

        if (exit_requested) break;

        time_t rawtime;
        struct tm *info;
        char time_str[100];

        time(&rawtime);
        info = localtime(&rawtime);
        // RFC 2822 format
        size_t len = strftime(time_str, sizeof(time_str), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", info);

        pthread_mutex_lock(&file_mutex);
        int fd = open(DATA_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (fd != -1) {
            write(fd, time_str, len);
            close(fd);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    openlog("aesdsocket", LOG_PID, LOG_USER);

    // Setup signal handling
    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) return -1;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        return -1;
    }

    // Daemonize if -d flag is provided
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid > 0) exit(0);
        setsid();
        chdir("/");
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    if (listen(server_fd, 10) < 0) {
        close(server_fd);
        return -1;
    }

    // Start Timer Thread
    pthread_t timer_tid;
    pthread_create(&timer_tid, NULL, timer_handler, NULL);

    // Accept loop
    while (!exit_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd == -1) {
            if (exit_requested) break;
            continue;
        }

        struct thread_data* t_node = malloc(sizeof(struct thread_data));
        if (!t_node) {
            close(client_fd);
            continue;
        }
        t_node->client_fd = client_fd;
        t_node->client_addr = client_addr;
        t_node->thread_complete = false;

        pthread_create(&t_node->thread_id, NULL, thread_handler, t_node);
        SLIST_INSERT_HEAD(&head, t_node, entries);

        // Periodically join completed threads
        struct thread_data *it, *tmp_it;
        SLIST_FOREACH_SAFE(it, &head, entries, tmp_it) {
            if (it->thread_complete) {
                pthread_join(it->thread_id, NULL);
                SLIST_REMOVE(&head, it, thread_data, entries);
                free(it);
            }
        }
    }

    // Cleanup logic
    pthread_join(timer_tid, NULL);
    while (!SLIST_EMPTY(&head)) {
        struct thread_data* it = SLIST_FIRST(&head);
        pthread_join(it->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, entries);
        free(it);
    }

    pthread_mutex_destroy(&file_mutex);
    close(server_fd);
    remove(DATA_FILE);
    syslog(LOG_INFO, "Server shutdown complete");
    closelog();

    return 0;
}
