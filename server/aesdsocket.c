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

#define PORT "9000"
#define BACKLOG 10
#define FILE_PATH "/var/tmp/aesdsocketdata"
#define CHUNK_SIZE 2048

// Behavior is undefined if the signal handler refers to any object other than by... 
// assigning a value to an object declared as volatile sig_atomic_t
volatile sig_atomic_t got_sig = 0;

void signal_handler(int sig)
{
    got_sig = 1;
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
    
    // Server loop
    while(!got_sig)
    {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(struct sockaddr_in);
        int clientfd;

        clientfd = accept(sockfd, (struct sockaddr*) &client_addr, &addr_len);
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
        write(STDOUT_FILENO, accepted, strlen(accepted));
        syslog(LOG_INFO, "%s", accepted);

        // Setup for receiving packets
        int buf_alloc = CHUNK_SIZE;
        char *recv_buf = (char*) malloc(buf_alloc);
        if (!recv_buf)
        {
            perror("recv_buf");
            close(clientfd);
            continue;
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
                snprintf(log_buf, sizeof(log_buf), "Closed connection from %s\n", client_ip);
                const char* closed = log_buf;
                write(STDOUT_FILENO, closed, strlen(closed));
                syslog(LOG_INFO, "%s", closed);
                break;
            }
            else
            {
                perror("recv");
                break;
            }
        }
        
        // Flush to disk
        if (recv_total > 0)
        {
            int filefd = open(FILE_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
            if (filefd == -1)
                perror("file open");
            else
            {
                if (write(filefd, recv_buf, recv_total) == -1)
                    perror("file write");
                
                close(filefd);
            }
            
            filefd = open(FILE_PATH, O_RDONLY);
            if (filefd == -1)
                perror("file open");
            else
            {
                char echo_buf[CHUNK_SIZE];
                int bytes_read;
                
                while ((bytes_read = read(filefd, echo_buf, sizeof(echo_buf))) > 0)
                {
                    rc = send(clientfd, echo_buf, bytes_read, 0);
                    if (rc == -1)
                    {
                        perror("send");
                        break;
                    }
                }   
                close(filefd);
            }
        }
        
        // Clean the heap buffer
        free(recv_buf);
        recv_buf = NULL;
        
        close(clientfd);
    }
    
    // Cleanup
    close(sockfd);
    unlink(FILE_PATH);
    
    syslog(LOG_INFO, "Caught signal, exiting");
    closelog();
    
    return 0;
}
