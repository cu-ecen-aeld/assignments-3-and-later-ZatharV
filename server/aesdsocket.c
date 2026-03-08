/**
 * @file      aesdsocket.c
 * @author    Atharv More
 * @brief     Full implementation of multi-threaded socket server with 
 * conditional support for the aesdchar device driver.
 * @date      02/14/2026
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
#include <sys/queue.h>
#include <time.h>

#define PORT 9000
#define BUF_SIZE 1024

// Assignment 8 Build Switch: Default to 1 (Char Device)
#ifndef USE_AESD_CHAR_DEVICE
    #define USE_AESD_CHAR_DEVICE 1
#endif

// Define path based on the build switch
#if USE_AESD_CHAR_DEVICE
    #define DATA_FILE "/dev/aesdchar"
#else
    #define DATA_FILE "/var/tmp/aesdsocketdata"
#endif

// Thread management structure
struct thread_data_s {
    pthread_t thread_id;
    int client_fd;
    char client_ip[INET_ADDRSTRLEN];
    int complete_flag;
    SLIST_ENTRY(thread_data_s) entries;
};

// Global variables
int server_fd = -1;
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile sig_atomic_t keep_running = 1;

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        keep_running = 0;
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
    }
}

/**
 * Timer thread: Only included if NOT using the char device
 */
#if !USE_AESD_CHAR_DEVICE
void* timestamp_handler(void* thread_param) {
    while (keep_running) {
        for (int i = 0; i < 10 && keep_running; i++) {
            sleep(1);
        }
        if (!keep_running) break;

        time_t rawtime;
        struct tm *info;
        char time_buf[100];
        char final_str[150];

        time(&rawtime);
        info = localtime(&rawtime);

        strftime(time_buf, sizeof(time_buf), "%a, %d %b %Y %T %z", info);
        int len = snprintf(final_str, sizeof(final_str), "timestamp:%s\n", time_buf);

        pthread_mutex_lock(&file_mutex);
        int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd != -1) {
            write(fd, final_str, len);
            close(fd);
        }
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}
#endif

/**
 * Connection thread: Handles client RX/TX logic
 */
void* thread_handler(void* thread_param) {
    struct thread_data_s* data = (struct thread_data_s*)thread_param;
    char *recv_buffer = malloc(BUF_SIZE);
    if (!recv_buffer) goto cleanup;

    ssize_t bytes_received;
    size_t total_received = 0;
    size_t current_buf_size = BUF_SIZE;

    // Receive data until we get a newline terminator
    while (keep_running) {
        bytes_received = recv(data->client_fd, recv_buffer + total_received,
                              current_buf_size - total_received - 1, 0);
        if (bytes_received <= 0) break;

        total_received += bytes_received;
        recv_buffer[total_received] = '\0';

        // Check if we received a newline (end of command)
        if (memchr(recv_buffer + (total_received - bytes_received), '\n', bytes_received)) {
            break;
        }

        // Grow buffer if needed
        current_buf_size += BUF_SIZE;
        char *new_ptr = realloc(recv_buffer, current_buf_size);
        if (!new_ptr) goto cleanup;
        recv_buffer = new_ptr;
    }

    if (total_received > 0) {
        pthread_mutex_lock(&file_mutex);

        // FIX 1: Write and read use SEPARATE file descriptors.
        // FIX 2: No O_CREAT on char device; no lseek needed since
        //        opening a new fd for read starts at position 0 (driver manages f_pos).
#if USE_AESD_CHAR_DEVICE
        // Write to char device
        int wfd = open(DATA_FILE, O_WRONLY | O_APPEND, 0644);
        if (wfd != -1) {
            write(wfd, recv_buffer, total_received);
            close(wfd);
        } else {
            syslog(LOG_ERR, "Failed to open %s for write: %s", DATA_FILE, strerror(errno));
            pthread_mutex_unlock(&file_mutex);
            goto cleanup;
        }

        // Read back entire content from char device (new fd starts at f_pos=0)
        int rfd = open(DATA_FILE, O_RDONLY);
        if (rfd != -1) {
            char send_buf[BUF_SIZE];
            ssize_t r_bytes;
            while ((r_bytes = read(rfd, send_buf, BUF_SIZE)) > 0) {
                send(data->client_fd, send_buf, r_bytes, 0);
            }
            close(rfd);
        } else {
            syslog(LOG_ERR, "Failed to open %s for read: %s", DATA_FILE, strerror(errno));
        }
#else
        // Regular file: write, seek to start, read back
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
#endif

        pthread_mutex_unlock(&file_mutex);
    }

cleanup:
    if (recv_buffer) free(recv_buffer);
    close(data->client_fd);
    syslog(LOG_INFO, "Closed connection from %s", data->client_ip);
    data->complete_flag = 1;
    return NULL;
}

int main(int argc, char *argv[]) {
    int daemon_mode = (argc > 1 && strcmp(argv[1], "-d") == 0);
    openlog("aesdsocket", LOG_PID, LOG_USER);

    struct sigaction sig_action = { .sa_handler = signal_handler };
    sigaction(SIGINT, &sig_action, NULL);
    sigaction(SIGTERM, &sig_action, NULL);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        close(server_fd);
        return -1;
    }

    if (daemon_mode) {
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

    listen(server_fd, 10);

    SLIST_HEAD(slisthead, thread_data_s) head;
    SLIST_INIT(&head);

#if !USE_AESD_CHAR_DEVICE
    pthread_t timer_thread;
    pthread_create(&timer_thread, NULL, timestamp_handler, NULL);
#endif

    while (keep_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd == -1) {
            if (errno == EINTR || !keep_running) break;
            continue;
        }

        struct thread_data_s* new_thread = malloc(sizeof(struct thread_data_s));
        if (!new_thread) {
            close(client_fd);
            continue;
        }
        new_thread->client_fd = client_fd;
        new_thread->complete_flag = 0;
        inet_ntop(AF_INET, &client_addr.sin_addr, new_thread->client_ip, INET_ADDRSTRLEN);

        syslog(LOG_INFO, "Accepted connection from %s", new_thread->client_ip);

        if (pthread_create(&new_thread->thread_id, NULL, thread_handler, new_thread) != 0) {
            free(new_thread);
            close(client_fd);
        } else {
            SLIST_INSERT_HEAD(&head, new_thread, entries);
        }

        // Clean up completed threads
        struct thread_data_s *td, *tmp_td;
        td = SLIST_FIRST(&head);
        while (td != NULL) {
            tmp_td = SLIST_NEXT(td, entries);
            if (td->complete_flag) {
                pthread_join(td->thread_id, NULL);
                SLIST_REMOVE(&head, td, thread_data_s, entries);
                free(td);
            }
            td = tmp_td;
        }
    }

#if !USE_AESD_CHAR_DEVICE
    pthread_join(timer_thread, NULL);
#endif

    // Join any remaining threads
    struct thread_data_s *td;
    while (!SLIST_EMPTY(&head)) {
        td = SLIST_FIRST(&head);
        pthread_join(td->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, entries);
        free(td);
    }

    close(server_fd);

    // FIX 3: Do NOT remove /dev/aesdchar on exit (it's a device node, not a file)
    // Only remove the data file when NOT using the char device
#if !USE_AESD_CHAR_DEVICE
    remove(DATA_FILE);
#endif

    pthread_mutex_destroy(&file_mutex);
    closelog();
    return 0;
}
