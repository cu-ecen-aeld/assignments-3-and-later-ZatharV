#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

static volatile sig_atomic_t shutdown_requested = 0;
static int g_server_fd = -1;

static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct thread_node {
    pthread_t thread;
    int client_fd;
    bool thread_complete;
    SLIST_ENTRY(thread_node) entries;
} thread_node_t;

SLIST_HEAD(thread_list_head, thread_node) g_thread_head =
    SLIST_HEAD_INITIALIZER(g_thread_head);

static pthread_t timestamp_thread;
static bool timestamp_thread_started = false;

static void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        shutdown_requested = 1;

        // Unblock accept() if waiting
        if (g_server_fd != -1) {
            shutdown(g_server_fd, SHUT_RDWR);
            close(g_server_fd);
            g_server_fd = -1;
        }
    }
}

static int setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction SIGINT failed: %s", strerror(errno));
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        syslog(LOG_ERR, "sigaction SIGTERM failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int daemonize_after_bind(void)
{
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "Fork failed: %s", strerror(errno));
        return -1;
    }
    if (pid > 0) {
        exit(0); // parent exits
    }

    if (setsid() < 0) {
        syslog(LOG_ERR, "setsid failed: %s", strerror(errno));
        return -1;
    }

    if (chdir("/") != 0) {
        syslog(LOG_ERR, "chdir failed: %s", strerror(errno));
        return -1;
    }

    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        (void)dup2(fd, STDIN_FILENO);
        (void)dup2(fd, STDOUT_FILENO);
        (void)dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
    return 0;
}

static int send_all(int sockfd, const char *buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        ssize_t s = send(sockfd, buf + total, len - total, 0);
        if (s < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)s;
    }
    return 0;
}

static void *timestamp_thread_func(void *arg)
{
    (void)arg;

    while (!shutdown_requested) {
        // sleep 10s in chunks for responsive shutdown
        for (int i = 0; i < 10 && !shutdown_requested; i++) {
            sleep(1);
        }
        if (shutdown_requested) break;

        time_t t = time(NULL);
        struct tm tm_info;
        localtime_r(&t, &tm_info);

        char timebuf[128];
        strftime(timebuf, sizeof(timebuf), "%a, %d %b %Y %H:%M:%S %z", &tm_info);

        char line[256];
        int len = snprintf(line, sizeof(line), "timestamp:%s\n", timebuf);
        if (len <= 0) continue;

        pthread_mutex_lock(&file_mutex);
        int fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t off = 0;
            while (off < len) {
                ssize_t w = write(fd, line + off, (size_t)(len - off));
                if (w <= 0) break;
                off += w;
            }
            close(fd);
        }
        pthread_mutex_unlock(&file_mutex);
    }

    return NULL;
}

static void *client_thread_func(void *arg)
{
    thread_node_t *node = (thread_node_t *)arg;
    int client_fd = node->client_fd;

    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    char *acc = NULL;
    size_t acc_len = 0;

    while (!shutdown_requested) {
        bytes_received = recv(client_fd, buffer, sizeof(buffer), 0);
        if (bytes_received < 0) {
            if (errno == EINTR && shutdown_requested) break;
            syslog(LOG_ERR, "recv failed: %s", strerror(errno));
            break;
        }
        if (bytes_received == 0) {
            break; // client closed
        }

        char *new_acc = realloc(acc, acc_len + (size_t)bytes_received);
        if (!new_acc) {
            syslog(LOG_ERR, "realloc failed");
            free(acc);
            acc = NULL;
            acc_len = 0;
            break;
        }
        acc = new_acc;
        memcpy(acc + acc_len, buffer, (size_t)bytes_received);
        acc_len += (size_t)bytes_received;

        // Process newline-terminated packets
        while (1) {
            void *nlptr = memchr(acc, '\n', acc_len);
            if (!nlptr) break;

            size_t pkt_len = ((char *)nlptr - acc) + 1;

            pthread_mutex_lock(&file_mutex);

            // Append packet
            int data_fd = open(DATA_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (data_fd < 0) {
                syslog(LOG_ERR, "open data file failed: %s", strerror(errno));
                pthread_mutex_unlock(&file_mutex);
                goto out;
            }

            ssize_t off = 0;
            while (off < (ssize_t)pkt_len) {
                ssize_t w = write(data_fd, acc + off, pkt_len - (size_t)off);
                if (w <= 0) {
                    syslog(LOG_ERR, "write failed: %s", strerror(errno));
                    break;
                }
                off += w;
            }
            close(data_fd);

            // Read full file and send back
            data_fd = open(DATA_FILE, O_RDONLY);
            if (data_fd < 0) {
                syslog(LOG_ERR, "open data file for read failed: %s", strerror(errno));
                pthread_mutex_unlock(&file_mutex);
                goto out;
            }

            ssize_t bytes_read;
            while ((bytes_read = read(data_fd, buffer, sizeof(buffer))) > 0) {
                if (send_all(client_fd, buffer, (size_t)bytes_read) != 0) {
                    syslog(LOG_ERR, "send failed: %s", strerror(errno));
                    break;
                }
            }
            if (bytes_read < 0) {
                syslog(LOG_ERR, "read failed: %s", strerror(errno));
            }
            close(data_fd);

            pthread_mutex_unlock(&file_mutex);

            // Remove consumed packet from accumulator
            size_t remaining = acc_len - pkt_len;
            if (remaining > 0) {
                memmove(acc, acc + pkt_len, remaining);
            }
            acc_len = remaining;

            if (acc_len == 0) {
                free(acc);
                acc = NULL;
            } else {
                char *shrunk = realloc(acc, acc_len);
                if (shrunk) acc = shrunk;
            }
        }
    }

out:
    free(acc);
    close(client_fd);
    node->client_fd = -1;
    node->thread_complete = true;
    return node;
}

static void cleanup_and_exit(void)
{
    syslog(LOG_INFO, "Caught signal, exiting");
    shutdown_requested = 1;

    // Join all client threads
    while (!SLIST_EMPTY(&g_thread_head)) {
        thread_node_t *n = SLIST_FIRST(&g_thread_head);
        SLIST_REMOVE_HEAD(&g_thread_head, entries);

        if (n->client_fd != -1) {
            shutdown(n->client_fd, SHUT_RDWR);
            close(n->client_fd);
            n->client_fd = -1;
        }
        pthread_join(n->thread, NULL);
        free(n);
    }

    // Stop timestamp thread
    if (timestamp_thread_started) {
        pthread_join(timestamp_thread, NULL);
    }

    pthread_mutex_destroy(&file_mutex);

    if (g_server_fd != -1) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    unlink(DATA_FILE);
    closelog();
    exit(0);
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    struct sockaddr_in server_addr;

    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        daemon_mode = true;
    }

    openlog("aesdsocket", LOG_PID, LOG_USER);

    if (setup_signals() != 0) {
        closelog();
        return -1;
    }

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd == -1) {
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    int opt = 1;
    if (setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        closelog();
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    if (bind(g_server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        syslog(LOG_ERR, "Bind failed: %s", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        closelog();
        return -1;
    }

    // Daemonize after bind
    if (daemon_mode) {
        if (daemonize_after_bind() != 0) {
            close(g_server_fd);
            g_server_fd = -1;
            closelog();
            return -1;
        }
    }

    // Start timestamp thread AFTER daemonize
    if (pthread_create(&timestamp_thread, NULL, timestamp_thread_func, NULL) != 0) {
        syslog(LOG_ERR, "Failed to create timestamp thread");
        cleanup_and_exit();
    }
    timestamp_thread_started = true;

    if (listen(g_server_fd, 10) < 0) {
        syslog(LOG_ERR, "Listen failed: %s", strerror(errno));
        cleanup_and_exit();
    }

    // Accept loop
    while (!shutdown_requested) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if ((errno == EINTR && shutdown_requested) || shutdown_requested) {
                break;
            }
            syslog(LOG_ERR, "Accept failed: %s", strerror(errno));
            continue;
        }

        char client_ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        syslog(LOG_INFO, "Accepted connection from %s", client_ip);

        thread_node_t *node = calloc(1, sizeof(thread_node_t));
        if (!node) {
            syslog(LOG_ERR, "calloc failed");
            close(client_fd);
            continue;
        }
        node->client_fd = client_fd;
        node->thread_complete = false;

        if (pthread_create(&node->thread, NULL, client_thread_func, node) != 0) {
            syslog(LOG_ERR, "pthread_create failed");
            close(client_fd);
            free(node);
            continue;
        }

        SLIST_INSERT_HEAD(&g_thread_head, node, entries);

        // Join completed threads (portable: no SLIST_REMOVE_AFTER)
        thread_node_t *cur = SLIST_FIRST(&g_thread_head);
        thread_node_t *prev = NULL;

        while (cur) {
            thread_node_t *next = SLIST_NEXT(cur, entries);

            if (cur->thread_complete) {
                pthread_join(cur->thread, NULL);

                if (prev == NULL) {
                    SLIST_REMOVE_HEAD(&g_thread_head, entries);
                } else {
                    // portable removal: bypass cur
                    prev->entries.sle_next = next;
                }

                free(cur);
            } else {
                prev = cur;
            }

            cur = next;
        }
    }

    cleanup_and_exit();
    return 0;
}
