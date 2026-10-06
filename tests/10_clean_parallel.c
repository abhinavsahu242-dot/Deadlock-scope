#include <pthread.h>
#include <stdio.h>

#define NUM_THREADS 4
#define ITERATIONS  500

pthread_mutex_t lock1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock3 = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < ITERATIONS; i++) {
        /* Strict global hierarchy: lock1 < lock2 < lock3 */
        pthread_mutex_lock(&lock1);
        pthread_mutex_lock(&lock2);
        pthread_mutex_lock(&lock3);

        pthread_mutex_unlock(&lock3);
        pthread_mutex_unlock(&lock2);
        pthread_mutex_unlock(&lock1);
    }
    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker, NULL);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    printf("Clean parallel test completed with zero violations.\n");
    return 0;
}
