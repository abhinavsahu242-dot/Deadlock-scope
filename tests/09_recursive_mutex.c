#include <pthread.h>
#include <stdio.h>

pthread_mutex_t recLock;

void helper(void) {
    pthread_mutex_lock(&recLock);
    /* Nested re-entrant acquisition */
    pthread_mutex_unlock(&recLock);
}

int main(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&recLock, &attr);
    pthread_mutexattr_destroy(&attr);

    pthread_mutex_lock(&recLock);
    helper();
    pthread_mutex_unlock(&recLock);

    pthread_mutex_destroy(&recLock);
    printf("Recursive mutex test passed cleanly.\n");
    return 0;
}
