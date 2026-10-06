#include <pthread.h>
#include <stdio.h>

pthread_mutex_t lockA = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lockB = PTHREAD_MUTEX_INITIALIZER;

int main(void) {
    /* Function 1: A -> B */
    pthread_mutex_lock(&lockA);
    pthread_mutex_lock(&lockB);
    pthread_mutex_unlock(&lockB);
    pthread_mutex_unlock(&lockA);

    /* Function 2: B -> A on the SAME thread */
    pthread_mutex_lock(&lockB);
    pthread_mutex_lock(&lockA);
    pthread_mutex_unlock(&lockA);
    pthread_mutex_unlock(&lockB);

    printf("Same-thread sequential acquisition completed.\n");
    return 0;
}
