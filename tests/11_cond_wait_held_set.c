#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cv    = PTHREAD_COND_INITIALIZER;
int ready = 0;

void *waiter(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mutex);
    while (!ready) {
        /* Atomically releases mutex and blocks */
        pthread_cond_wait(&cv, &mutex);
        /* Re-acquires mutex upon return */
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

void *signaler(void *arg) {
    (void)arg;
    usleep(20000); /* 20ms */
    /* Acquire the mutex while waiter is blocked inside cond_wait */
    pthread_mutex_lock(&mutex);
    ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mutex);
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, waiter, NULL);
    pthread_create(&t2, NULL, signaler, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("Condvar held-set test completed cleanly without error.\n");
    return 0;
}
