#include <pthread.h>
#include <stdio.h>

pthread_mutex_t lockA = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lockB = PTHREAD_MUTEX_INITIALIZER;

void *worker1(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lockA);
    pthread_mutex_lock(&lockB);
    pthread_mutex_unlock(&lockB);
    pthread_mutex_unlock(&lockA);
    return NULL;
}

void *worker2(void *arg) {
    (void)arg;
    pthread_mutex_lock(&lockB);
    pthread_mutex_lock(&lockA);
    pthread_mutex_unlock(&lockA);
    pthread_mutex_unlock(&lockB);
    return NULL;
}

int main(void) {
    pthread_t t1, t2;

    /* Worker 1 runs and is fully joined BEFORE Worker 2 is created */
    pthread_create(&t1, NULL, worker1, NULL);
    pthread_join(t1, NULL);

    /* Worker 2 spawned after Worker 1 terminates */
    pthread_create(&t2, NULL, worker2, NULL);
    pthread_join(t2, NULL);

    printf("Lifecycle-ordered execution completed cleanly.\n");
    return 0;
}
