/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "test_server.h"
#include "feedman.h"
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
typedef struct {
    TestServer *server;
    int socket;
}
Connection;
static void send_all(int fd,const char *text,size_t n) {
    while(n){
        ssize_t sent=send(fd,text,n,MSG_NOSIGNAL);
        if(sent<0&&errno==EINTR)continue;
        if(sent<=0)return;
        text+=sent;
        n-=(size_t)sent;
    }
}
static void *connection(void *data) {
    Connection *c=data;
    TestServer *s=c->server;
    int fd=c->socket;
    free(c);
    char request[32768]={
        0
    };
    size_t len=0;
    while(len<sizeof request-1) {
        ssize_t n=recv(fd,request+len,sizeof request-len-1,0);
        if(n<=0)break;
        len+=(size_t)n;
        request[len]=0;
        if(strstr(request,"\r\n\r\n"))break;
    }
    int active=atomic_fetch_add(&s->active,1)+1,max=atomic_load(&s->max_active);
    while(active>max&&!atomic_compare_exchange_weak(&s->max_active,&max,active)){
    }
    atomic_fetch_add(&s->requests,1);
    char path[1024]="/";
    sscanf(request,"GET %1023s",path);
    int delay=s->delay_ms;
    if(strstr(path,"/slow"))delay=2000;
    struct timespec ts={
        .tv_sec=delay/1000,.tv_nsec=(delay%1000)*1000000L
    };
    while(nanosleep(&ts,&ts)!=0&&errno==EINTR){
    }
    bool conditional=strstr(request,"If-None-Match: \"feedman-test-v1\"")!=NULL;
    if(conditional)atomic_fetch_add(&s->conditional,1);
    FmString body={
        0
    },header={
        0
    };
    if(strstr(path,"/error")) {
        fm_string_append(&body,"Service unavailable");
        fm_string_printf(&header,"HTTP/1.1 503 Service Unavailable\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",body.len);
    }
    else if(strstr(path,"/invalid")) {
        fm_string_append(&body,"<html><body>not a feed</body></html>");
        fm_string_printf(&header,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nETag: \"bad\"\r\nConnection: close\r\n\r\n",body.len);
    }
    else if(strcmp(path,"/redirect")==0) {
        fm_string_append(&header,"HTTP/1.1 302 Found\r\nLocation: /feed/redirected\r\nETag: \"redirect-must-not-be-used\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
    else if(strstr(path,"/file-redirect")) {
        fm_string_append(&header,"HTTP/1.1 302 Found\r\nLocation: file:///etc/passwd\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
    else if(conditional) {
        fm_string_append(&header,"HTTP/1.1 304 Not Modified\r\nETag: \"feedman-test-v1\"\r\nConnection: close\r\n\r\n");
    }
    else {
        fm_string_append(&body,"<?xml version=\"1.0\"?><rss version=\"2.0\"><channel><title>Fixture</title>");
        for(int i=0;i<20;i++)fm_string_printf(&body,"<item><guid>%s-%d</guid><title>テスト記事 %d C言語 高速RSS</title><link>http://127.0.0.1:%d/article/%d</link><description>&lt;p&gt;ローカルのテストデータです。本文検索を確認。&lt;/p&gt;</description><pubDate>Sat, 12 Sep 2026 04:00:00 GMT</pubDate></item>",path,i,i,s->port,i);
        fm_string_append(&body,"</channel></rss>");
        if(strstr(path,"/large")) {
            free(body.data);
            body=(FmString){
                0
            };
            for(int i=0;i<4096;i++)fm_string_append(&body,"0123456789abcdef");
        }
        fm_string_printf(&header,"HTTP/1.1 200 OK\r\nContent-Type: application/rss+xml; charset=utf-8\r\nContent-Length: %zu\r\nETag: \"feedman-test-v1\"\r\nLast-Modified: Sat, 12 Sep 2026 04:00:00 GMT\r\nConnection: close\r\n\r\n",body.len);
    }
    send_all(fd,header.data,header.len);
    send_all(fd,body.data,body.len);
    free(body.data);
    free(header.data);
    close(fd);
    atomic_fetch_sub(&s->active,1);
    pthread_mutex_lock(&s->mutex);
    s->workers--;
    pthread_cond_signal(&s->idle);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}
static void *server_thread(void *data) {
    TestServer *s=data;
    while(!atomic_load(&s->stop)) {
        int fd=accept(s->socket,NULL,NULL);
        if(fd<0){
            if(errno==EINTR)continue;
            break;
        }
        struct timeval timeout={
            .tv_sec=5
        };
        setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);
        Connection *c=fm_malloc(sizeof *c);
        *c=(Connection){
            .server=s,.socket=fd
        };
        pthread_mutex_lock(&s->mutex);
        s->workers++;
        pthread_mutex_unlock(&s->mutex);
        pthread_t thread;
        if(pthread_create(&thread,NULL,connection,c)!=0){
            free(c);
            close(fd);
            pthread_mutex_lock(&s->mutex);
            s->workers--;
            pthread_mutex_unlock(&s->mutex);
            continue;
        }
        pthread_detach(thread);
    }
    return NULL;
}
int test_server_start(TestServer *s,int delay_ms) {
    memset(s,0,sizeof *s);
    s->delay_ms=delay_ms;
    atomic_init(&s->stop,false);
    atomic_init(&s->requests,0);
    atomic_init(&s->active,0);
    atomic_init(&s->max_active,0);
    atomic_init(&s->conditional,0);
    pthread_mutex_init(&s->mutex,NULL);
    pthread_cond_init(&s->idle,NULL);
    s->socket=socket(AF_INET,SOCK_STREAM,0);
    if(s->socket<0)return -1;
    int reuse=1;
    setsockopt(s->socket,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
    struct sockaddr_in address={
        .sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=0
    };
    if(bind(s->socket,(struct sockaddr *)&address,sizeof address)<0||listen(s->socket,128)<0){
        close(s->socket);
        return -1;
    }
    socklen_t len=sizeof address;
    getsockname(s->socket,(struct sockaddr *)&address,&len);
    s->port=ntohs(address.sin_port);
    if(pthread_create(&s->thread,NULL,server_thread,s)!=0){
        close(s->socket);
        return -1;
    }
    return 0;
}
void test_server_stop(TestServer *s) {
    atomic_store(&s->stop,true);
    shutdown(s->socket,SHUT_RDWR);
    close(s->socket);
    pthread_join(s->thread,NULL);
    pthread_mutex_lock(&s->mutex);
    while(s->workers)pthread_cond_wait(&s->idle,&s->mutex);
    pthread_mutex_unlock(&s->mutex);
    pthread_mutex_destroy(&s->mutex);
    pthread_cond_destroy(&s->idle);
}
