#include <string.h>
#include "event.h"
// Initializes the event queue.
// Sets indices, count, and prepares synchronization primitives.
void event_queue_init(EventQueue* q) {
    
    q->head = q->tail = q->count = 0;
    q->shutdown = false;
    pthread_mutex_init(&q->mutex,     nullptr);
    pthread_cond_init(&q->not_empty,  nullptr);
    pthread_cond_init(&q->not_full,   nullptr);
}
// Cleans up queue resources.
// Destroys mutex and condition variables to prevent leaks.
void event_queue_destroy(EventQueue* q) {
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}
// Pushes a new event into the queue (producer side).
// Waits if queue is full, unless shutdown is triggered.
void event_queue_push(EventQueue* q, const Event& e) {
    pthread_mutex_lock(&q->mutex);
    while (q->count >= EVENT_QUEUE_CAPACITY && !q->shutdown)
        pthread_cond_wait(&q->not_full, &q->mutex);
    if (q->shutdown) {
        pthread_mutex_unlock(&q->mutex);
        return;
    }
    q->items[q->tail] = e;
    q->tail = (q->tail + 1) % EVENT_QUEUE_CAPACITY;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}
// Pops an event from the queue (consumer side).
// Waits if queue is empty; returns a sentinel event if shutdown occurs.
Event event_queue_pop(EventQueue* q) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->not_empty, &q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        Event sentinel{};
        sentinel.type    = EVT_MATCH_OVER;
        sentinel.message = "shutdown";
        return sentinel;
    }
    Event e = q->items[q->head];
    q->head = (q->head + 1) % EVENT_QUEUE_CAPACITY;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return e;
}
// Signals shutdown of the queue.
// Wakes up all waiting producer and consumer threads.
void event_queue_shutdown(EventQueue* q) {
    pthread_mutex_lock(&q->mutex);
    q->shutdown = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
}