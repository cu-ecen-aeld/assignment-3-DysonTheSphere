#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include "aesdworker.h"

#define PORT "9000"
#define BACKLOG 10
#define FILE_PATH "/var/tmp/aesdsocketdata"
#define CHUNK_SIZE 2048
#define TIMESTAMP_INTERVAL 10

// Behavior is undefined if the signal handler refers to any object other than by... 
// assigning a value to an object declared as volatile sig_atomic_t
volatile sig_atomic_t got_sig = 0;

// Protect file
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Threads singly linked
static struct worker *head = NULL;

void signal_handler(int sig)
{
    got_sig = 1;
}

static struct worker* hire_worker(int fd, const char *client_ip)
{
    struct worker *hire = malloc(sizeof(struct worker));
    if (!hire)
        return NULL;
    
    hire->fd = fd;
    hire->complete = 0;
    hire->next = NULL;
    // Copy string from pointer
    strcpy(hire->client_ip, client_ip);
    
    // Ensure signals are caught by main thread, not workers
    sigset_t set, oldset;
    sigfillset(&set);
    
    if (pthread_sigmask(SIG_BLOCK, &set, &oldset) != 0)
    {
        free(hire);
        return NULL;
    }
    
    int rc = pthread_create(&hire->id, NULL, assign_worker, hire);
    
    // Revert signals
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    
    if (rc != 0)
    {
        free(hire);
        return NULL;
    }
    
    // Prepend head
    hire->next = head;
    head = hire;
    
    return hire;
}

static void *assign_worker(void *arg)
{
    struct worker *curr_worker = (struct worker*) arg;
    int clientfd = curr_worker->fd;
    
    // Setup for receiving packets
    int buf_alloc = CHUNK_SIZE;
    char *recv_buf = (char*) malloc(buf_alloc);
    if (!recv_buf)
    {
        perror("recv_buf");
        close(clientfd);
        curr_worker->complete = 1;
        return NULL;
    }
    
    int recv_done = 0;
    int recv_total = 0;
    
    while (!recv_done)
    {
        //Check that there is enough space first
        if (buf_alloc - recv_total < CHUNK_SIZE)
        {
            buf_alloc += CHUNK_SIZE;
            char *temp = (char *) realloc(recv_buf, buf_alloc);
            if (!temp)
            {
                perror("realloc");
                break;
            }
            recv_buf = temp;
        }
        
        // Read the message into heap
        int bytes = recv(clientfd, recv_buf + recv_total, CHUNK_SIZE, 0);
        if (bytes > 0)
        {
            // Check if end of packet
            for (int i = recv_total; i < recv_total + bytes; i++)
                if(recv_buf[i] == '\n')
                {
                    recv_done = 1;
                    recv_total = i + 1;
                    break;
                }
            // newline not found, all bytes tracked
            if (!recv_done)    
                recv_total += bytes;
        }
        else if (bytes == 0)
        {
            char log_buf[NI_MAXHOST + 27];
            snprintf(log_buf, sizeof(log_buf), "Closed connection from %s\n", curr_worker->client_ip);
            const char* closed = log_buf;
            if (write(STDOUT_FILENO, closed, strlen(closed)) == -1 && errno != EINTR)
                // Ignored, this is an additional print solely for runtime debugging
            syslog(LOG_INFO, "%s", closed);
            break;
        }
        else
        {
            perror("recv");
            break;
        }
    }
    
    if (recv_total > 0)
    {
        // Obtain a lock
        pthread_mutex_lock(&file_mutex);
        
        // Flush to disk
        int filefd = open(FILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (filefd == -1)
            perror("file open");
        else
        {
            if (write(filefd, recv_buf, recv_total) == -1)
                perror("file write");
            
            close(filefd);
        }
        
        // Read to buffer
        char* send_buf = NULL;
        int send_buf_size = 0;
        
        filefd = open(FILE_PATH, O_RDONLY);
        if (filefd == -1)
            perror("file open");
        else
        {
            off_t file_size = lseek(filefd, 0, SEEK_END);
            lseek(filefd, 0, SEEK_SET);
            
            if (file_size > 0)
            {
                send_buf = malloc(file_size);
                
                if (send_buf)
                {
                    int total_read = 0;
                    while (total_read < file_size)
                    {
                        int bytes_read = read(filefd, send_buf + total_read, file_size - total_read);
                        if (bytes_read <= 0)
                            break;
                        total_read += bytes_read;
                    }
                    
                    send_buf_size = total_read;
                }
            }
            close(filefd);
            
            /*
            char echo_buf[CHUNK_SIZE];
            int bytes_read;
            
            while ((bytes_read = read(filefd, echo_buf, sizeof(echo_buf))) > 0)
            {
                int rc = send(clientfd, echo_buf, bytes_read, 0);
                if (rc == -1)
                {
                    perror("send");
                    break;
                }
            }   
            close(filefd);
            */
        }
        // Release the lock
        pthread_mutex_unlock(&file_mutex);
        
        // Send to client
        int sent_total = 0;
        int send_remaining = send_buf_size;
        while (send_remaining > 0)
        {
            int rc = send(clientfd, send_buf + sent_total, send_remaining, 0);
            if (rc == -1)
            {
                perror("send");
                break;
            }
                
                sent_total += rc;
                send_remaining -= rc;            
        }
        free(send_buf);
    }
    
    // Clean the heap buffer
    free(recv_buf);
    recv_buf = NULL;
    
    curr_worker->complete = 1;
    close(clientfd);
    return NULL;
}

static void retire_workers(aesdworker_fire_t mode)
{
    struct worker *curr = head;
    struct worker *prev = NULL;
    
    while (curr)
    {
        if (curr->complete || mode == ALL)
        {
            // Retire the worker
            pthread_join(curr->id, NULL);
            
            // Increment head or point around the gap
            if (!prev)
                head = curr->next;
            else
                prev->next = curr->next;
            
            struct worker *retiree = curr;
            curr = curr->next; // Designate successor
            free(retiree); // Cake will be served in the break room
        }
        else
        {
            // Retain, traverse payroll
            prev = curr;
            curr = curr->next;
        }
    }
}

// Dedicated timekeeper
static void *hr_worker(void *arg)
{
    struct timespec *start = (struct timespec *) arg;
    struct timespec wake = *start;
    
    while (!got_sig)
    {
        // Add interval to time
        wake.tv_sec += TIMESTAMP_INTERVAL;
        
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, NULL) == EINTR)
            if (got_sig)
                break;
        
        time_t t;
        struct tm detail;
        char time_str[64];
        
        time(&t);
        localtime_r(&t, &detail);
        
        int len = strftime(time_str, sizeof(time_str), "timestamp:%a, %d %b %Y %T %z\n", &detail);
        
        if (len > 0)
        {
            // Obtain a lock, could delay
            pthread_mutex_lock(&file_mutex);
            int filefd = open(FILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (filefd != -1)
            {
                if (write(filefd, time_str, len) == -1)
                    syslog(LOG_ERR, "Failed to write timestamp: %s", strerror(errno));
                
                close(filefd);
            }
            
            // Release the lock
            pthread_mutex_unlock(&file_mutex);
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{     
    struct addrinfo hints, *res;
    int rc;
    int sockfd;
    int daemon = 0;
    
    if (argc > 1 && strcmp(argv[1], "-d") == 0)
        daemon = 1;

    // The use of closelog() is optional.
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    
    // Register handler
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGINT, &sa, NULL) == -1) 
    {
        perror("sigaction SIGINT");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) == -1) 
    {
        perror("sigaction SIGTERM");
        return -1;
    }

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;          // IPv4
    hints.ai_socktype = SOCK_STREAM;    // TCP
    hints.ai_flags = AI_PASSIVE;        // INADDR_ANY

    rc = getaddrinfo(NULL, PORT, &hints, &res);
    if (rc != 0)
    {
        // getaddrinfo() doesn't set errno
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    // Create a socket
    sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sockfd == -1)
    {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }

    // Set REUSE flags
    const int reuse = 1;
    rc = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));
    if (rc != 0)
    {
        perror("address reuse");
        freeaddrinfo(res);
        close(sockfd);
        return -1;
    }

    // Bind socket
    rc = bind(sockfd, res->ai_addr, res->ai_addrlen);
    if (rc != 0)
    {
        perror("bind");
        freeaddrinfo(res);
        close(sockfd);
        return -1;
    }
    // Bound! No longer needed
    freeaddrinfo(res);
    
    // Daemon?
    if (daemon)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("fork");
            close(sockfd);
            return -1;
        }
        
        // Parent
        if (pid > 0)
        {
            _exit(EXIT_SUCCESS);
        }
        
        // Child
        if (setsid() == -1)
        {
            perror("setsid");
            close(sockfd);
            return -1;
        }
        
        if (chdir("/") == -1)
        {
            perror("chdir");
            close(sockfd);
            return -1;
        }
        
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0)
        {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }

    // Listen for connections
    rc = listen(sockfd, BACKLOG);
    if (rc != 0)
    {
        perror("listen");
        close(sockfd);
        return -1;
    }
    
    // Initialize the clock
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    
    // Hire a time keeper!
    pthread_t timekeeper;
    
    // Block signals in the worker
    sigset_t set, oldset;
    sigfillset(&set);
    pthread_sigmask(SIG_BLOCK, &set, &oldset);
    
    rc = pthread_create(&timekeeper, NULL, hr_worker, &start);
    
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    if (rc != 0)
    {
        fprintf(stderr, "pthread_create timekeeperL %s\n", strerror(rc));
        close(sockfd);
        return -1;
    }
    
    
    // Server loop
    while(!got_sig)
    {        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(struct sockaddr_in);
        
        int clientfd = accept(sockfd, (struct sockaddr*) &client_addr, &addr_len);
        if (clientfd == -1)
        {
            // Make sure it wasn't a signal
            if (errno != EINTR)
                perror("client");
            
            continue;
        }

        // Extract IP string from client_addr
        char client_ip[NI_MAXHOST];
        rc = getnameinfo((struct sockaddr*) &client_addr, addr_len, client_ip, sizeof(client_ip), NULL, 0, NI_NUMERICHOST);
        if (rc != 0)
        {
            // getnameinfo() doesn't set errno
            fprintf(stderr, "getnameinfo: %s\n", gai_strerror(rc));
            close(clientfd);
            continue;
        }

        // Additional byte for \0
        char log_buf[NI_MAXHOST + 27];
        snprintf(log_buf, sizeof(log_buf), "Accepted connection from %s\n", client_ip);
        // Let's make the string immutable
        const char* accepted = log_buf;

        // Log success
        if (write(STDOUT_FILENO, accepted, strlen(accepted)) == -1 && errno != EINTR)
            // Ignored, this is an additional print solely for runtime debugging 
        syslog(LOG_INFO, "%s", accepted);
        
        // Hire a worker
        struct worker *hired = hire_worker(clientfd, client_ip);
        if (!hired)
        {
            fprintf(stderr, "Failed to hire worker\n");
            close(clientfd);
        }
        
        // Check for complete work
        retire_workers(DONE);
    }
    
    // Cleanup
    close(sockfd);
    
    // Out of business!
    pthread_join(timekeeper, NULL);
    retire_workers(ALL);
    
    unlink(FILE_PATH);
    
    syslog(LOG_INFO, "Caught signal, exiting");
    closelog();
    
    return 0;
}
