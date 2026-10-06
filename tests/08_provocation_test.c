#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

pthread_mutex_t lockA = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lockB = PTHREAD_MUTEX_INITIALIZER;

void *worker1(void *arg) {
    (void)arg;
    for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&lockA);
        usleep(500); /* Fast critical section natively */
        pthread_mutex_lock(&lockB);
        pthread_mutex_unlock(&lockB);
        pthread_mutex_unlock(&lockA);
        usleep(1000);
    }
    return NULL;
}

void *worker2(void *arg) {
    (void)arg;
    /* Without stress, worker1 completes all 3 rounds well before 25ms */
    usleep(25000); /* 25ms offset */
    for (int i = 0; i < 3; i++) {
        pthread_mutex_lock(&lockB);
        usleep(500);
        pthread_mutex_lock(&lockA);
        pthread_mutex_unlock(&lockA);
        pthread_mutex_unlock(&lockB);
        usleep(1000);
    }
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker1, NULL);
    pthread_create(&t2, NULL, worker2, NULL);

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("Provocation test finished cleanly without deadlock.\n");
    return 0;
}
