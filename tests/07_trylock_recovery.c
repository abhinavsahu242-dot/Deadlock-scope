#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

pthread_mutex_t lockA = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lockB = PTHREAD_MUTEX_INITIALIZER;

void *worker1(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        pthread_mutex_lock(&lockA);
        usleep(5000);
        pthread_mutex_lock(&lockB);
        pthread_mutex_unlock(&lockB);
        pthread_mutex_unlock(&lockA);
        usleep(1000);
    }
    return NULL;
}

void *worker2(void *arg) {
    (void)arg;
    for (int i = 0; i < 5; i++) {
        pthread_mutex_lock(&lockB);
        /* Trylock with backoff */
        if (pthread_mutex_trylock(&lockA) == 0) {
            pthread_mutex_unlock(&lockA);
            pthread_mutex_unlock(&lockB);
        } else {
            /* Backoff on contention */
            pthread_mutex_unlock(&lockB);
            usleep(2000);
            i--; /* retry */
        }
    }
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker1, NULL);
    pthread_create(&t2, NULL, worker2, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("Trylock backoff completed without deadlock.\n");
    return 0;
}
