#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

pthread_mutex_t gateLock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lockA    = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lockB    = PTHREAD_MUTEX_INITIALIZER;

void *worker1(void *arg) {
    (void)arg;
    pthread_mutex_lock(&gateLock);
    pthread_mutex_lock(&lockA);
    pthread_mutex_lock(&lockB);
    pthread_mutex_unlock(&lockB);
    pthread_mutex_unlock(&lockA);
    pthread_mutex_unlock(&gateLock);
    return NULL;
}

void *worker2(void *arg) {
    (void)arg;
    pthread_mutex_lock(&gateLock);
    pthread_mutex_lock(&lockB);
    pthread_mutex_lock(&lockA);
    pthread_mutex_unlock(&lockA);
    pthread_mutex_unlock(&lockB);
    pthread_mutex_unlock(&gateLock);
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker1, NULL);
    pthread_create(&t2, NULL, worker2, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("Gate-lock protected test finished cleanly.\n");
    return 0;
}
