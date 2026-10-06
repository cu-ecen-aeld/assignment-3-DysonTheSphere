#ifndef AESDWORKER_H
#define AESDWORKER_H

#include <pthread.h>
#include <time.h>

// Might as well handle both types of thread join in the same SLL traversal
typedef enum 
{ 
    DONE, // Default behavior
    ALL 
} aesdworker_fire_t;

struct worker 
{
    pthread_t id;
    int fd;
    int complete;
    char client_ip[NI_MAXHOST];
    struct worker *next;
};

// Signature declaration
static void *assign_worker(void *arg);
static void retire_workers(aesdworker_fire_t mode);

#endif 
