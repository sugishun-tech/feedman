/* SPDX-License-Identifier: MIT */
#ifndef FEEDMAN_TEST_SERVER_H
#define FEEDMAN_TEST_SERVER_H
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
typedef struct {
    int socket,port,delay_ms;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t idle;
    int workers;
    atomic_int requests,active,max_active,conditional;
    atomic_bool stop;
}
TestServer;
int test_server_start(TestServer *server,int delay_ms);
void test_server_stop(TestServer *server);
#endif
