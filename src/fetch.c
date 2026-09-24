/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "feedman.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
typedef struct {
    size_t index;
    FmArticles articles;
    FmString body;
    char *etag,*modified,*error;
    long status;
    bool end;
} Result;
typedef struct {
    FmFeeds *feeds;
    FmFetchOptions *options;
    atomic_size_t next;
    CURLSH *share;
    pthread_mutex_t share_locks[CURL_LOCK_DATA_LAST];
    pthread_mutex_t queue_mutex;
    pthread_cond_t readable,writable;
    Result **queue;
    size_t qhead,qlen,qcap;
} Run;
typedef struct {
    Run *run;
    Result *result;
} Transfer;
static bool is_cancelled(Run *r) {
    return atomic_load(&r->options->cancel)||(r->options->signal_cancel&&*r->options->signal_cancel);
}

static void share_lock(CURL *handle,curl_lock_data data,curl_lock_access access,void *user) {
    (void)handle;
    (void)access;
    Run *r=user;
    if(data<CURL_LOCK_DATA_LAST)pthread_mutex_lock(&r->share_locks[data]);
}

static void share_unlock(CURL *handle,curl_lock_data data,void *user) {
    (void)handle;
    Run *r=user;
    if(data<CURL_LOCK_DATA_LAST)pthread_mutex_unlock(&r->share_locks[data]);
}

static void push(Run *r,Result *v) {
    pthread_mutex_lock(&r->queue_mutex);
    while(r->qlen==r->qcap)pthread_cond_wait(&r->writable,&r->queue_mutex);
    r->queue[(r->qhead+r->qlen)%r->qcap]=v;
    r->qlen++;
    pthread_cond_signal(&r->readable);
    pthread_mutex_unlock(&r->queue_mutex);
}

static Result *pop(Run *r) {
    pthread_mutex_lock(&r->queue_mutex);
    while(!r->qlen)pthread_cond_wait(&r->readable,&r->queue_mutex);
    Result *v=r->queue[r->qhead];
    r->qhead=(r->qhead+1)%r->qcap;
    r->qlen--;
    pthread_cond_signal(&r->writable);
    pthread_mutex_unlock(&r->queue_mutex);
    return v;
}

static size_t write_body(char *ptr,size_t size,size_t nmemb,void *user) {
    Transfer *t=user;
    if(size && nmemb>SIZE_MAX/size)return 0;
    size_t n=size*nmemb,limit=t->run->options->max_bytes;
    if(!limit || limit>FM_MAX_FEED_BYTES)limit=FM_MAX_FEED_BYTES;
    if(n>limit-t->result->body.len || is_cancelled(t->run))return 0;
    fm_string_append_n(&t->result->body,ptr,n);
    return n;
}

static size_t header(char *ptr,size_t size,size_t nmemb,void *user) {
    Result *r=user;
    if(size&&nmemb>SIZE_MAX/size)return 0;
    size_t n=size*nmemb;
    /* A redirect or an interim response starts a new header block. */
    if(n>=5&&!strncasecmp(ptr,"HTTP/",5)) {
        free(r->etag);
        free(r->modified);
        r->etag=r->modified=NULL;
        return n;
    }
    size_t prefix=0;
    char **dest=NULL;
    if(n>5&&!strncasecmp(ptr,"ETag:",5)){
        prefix=5;
        dest=&r->etag;
    }
    else if(n>14&&!strncasecmp(ptr,"Last-Modified:",14)){
        prefix=14;
        dest=&r->modified;
    }
    if(dest&&n-prefix<=4096) {
        char *raw=fm_strndup(ptr+prefix,n-prefix),*v=fm_trim(raw);
        bool safe=true;
        for(const unsigned char *p=(const unsigned char *)v;*p;p++)if(*p<32||*p==127)safe=false;
        if(safe){
            free(*dest);
            *dest=fm_strdup(v);
        }
        free(raw);
    }
    return n;
}

static int progress_callback(void *user,curl_off_t total,curl_off_t now,curl_off_t utotal,curl_off_t unow) {
    (void)total;
    (void)now;
    (void)utotal;
    (void)unow;
    return is_cancelled(user)?1:0;
}

static struct curl_slist *conditional(struct curl_slist *headers,const char *name,const char *value) {
    if(!value||!*value||strlen(value)>4096||strchr(value,'\r')||strchr(value,'\n'))return headers;
    FmString s={
        0
    };
    fm_string_printf(&s,"%s: %s",name,value);
    struct curl_slist *result=curl_slist_append(headers,s.data);
    free(s.data);
    return result?result:headers;
}

static void perform(Run *run,CURL *curl,Result *r) {
    FmFeed *feed=&run->feeds->items[r->index];
    if(!curl){
        r->error=fm_strdup("Cannot allocate an HTTP handle");
        return;
    }
    curl_easy_reset(curl);
    Transfer transfer={
        .run=run,.result=r
    };
    char error_buffer[CURL_ERROR_SIZE]={
        0
    };
    struct curl_slist *headers=NULL;
    headers=curl_slist_append(headers,"Accept: application/atom+xml, application/rss+xml, application/xml, text/xml;q=0.9, */*;q=0.1");
    headers=conditional(headers,"If-None-Match",feed->etag);
    headers=conditional(headers,"If-Modified-Since",feed->modified);
    curl_easy_setopt(curl,CURLOPT_URL,feed->url);
    curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"feedman/" FM_VERSION " (RSS/Atom reader)");
    curl_easy_setopt(curl,CURLOPT_ACCEPT_ENCODING,"");
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_SHARE,run->share);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(curl,CURLOPT_MAXREDIRS,5L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS_STR,"http,https");
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS_STR,"http,https");
#else
    curl_easy_setopt(curl,CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
    curl_easy_setopt(curl,CURLOPT_REDIR_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L);
    if(curl_version_info(CURLVERSION_NOW)->features&CURL_VERSION_HTTP2)
    curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,(long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,(long)(run->options->timeout>0?run->options->timeout:30));
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,32L);
    curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,15L);
    curl_easy_setopt(curl,CURLOPT_TCP_KEEPALIVE,1L);
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write_body);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&transfer);
    curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,header);
    curl_easy_setopt(curl,CURLOPT_HEADERDATA,r);
    curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress_callback);
    curl_easy_setopt(curl,CURLOPT_XFERINFODATA,run);
    curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,error_buffer);
    CURLcode rc=curl_easy_perform(curl);
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&r->status);
    if(rc!=CURLE_OK) {
        /* curl's detailed error can include a credential-bearing URL. Keep the
           public progress/error log to a host plus the stable error category. */
        fm_error(&r->error,"%s%s",curl_easy_strerror(rc),r->body.len>=run->options->max_bytes?" (size limit)":"");
    }
    else if(r->status==304) {
        /* No body parse, no article write. */
    }
    else if(r->status<200||r->status>=300)fm_error(&r->error,"HTTP %ld",r->status);
    else if(!is_cancelled(run)) {
        char *effective=NULL;
        curl_easy_getinfo(curl,CURLINFO_EFFECTIVE_URL,&effective);
        fm_parse_feed(r->body.data,r->body.len,effective?effective:feed->url,&r->articles,&r->error);
    }
    curl_slist_free_all(headers);
}

static void *worker(void *user) {
    Run *run=user;
    CURL *curl=curl_easy_init();
    while(!is_cancelled(run)) {
        size_t i=atomic_fetch_add(&run->next,1);
        if(i>=run->feeds->len)break;
        Result *r=fm_calloc(1,sizeof *r);
        r->index=i;
        perform(run,curl,r);
        push(run,r);
    }
    if(curl)curl_easy_cleanup(curl);
    Result *end=fm_calloc(1,sizeof *end);
    end->end=true;
    push(run,end);
    return NULL;
}

static void result_free(Result *r) {
    fm_articles_free(&r->articles);
    free(r->body.data);
    free(r->etag);
    free(r->modified);
    free(r->error);
    free(r);
}

void fm_stats_free(FmStats *s) {
    free(s->errors);
    *s=(FmStats){
        0
    };
}

bool fm_fetch_all(const char *db_path,FmFeeds *feeds,FmFetchOptions *options,FmProgress progress,void *user,FmStats *stats,char **error) {
    *stats=(FmStats){
        .total=feeds->len
    };
    double start=fm_clock();
    if(options->threads<1||options->threads>FM_MAX_THREADS) {
        fm_error(error,"Threads must be between 1 and %d",FM_MAX_THREADS);
        return false;
    }
    FmString lock_path={
        0
    };
    fm_string_printf(&lock_path,"%s.fetch.lock",db_path);
    int lockfd=open(lock_path.data,O_CREAT|O_RDWR|O_CLOEXEC,0600);
    free(lock_path.data);
    if(lockfd<0 || flock(lockfd,LOCK_EX|LOCK_NB)<0) {
        fm_error(error,"Another fetch/import is running, or the data directory is not writable");
        if(lockfd>=0)close(lockfd);
        return false;
    }
    FmDb db={
        0
    };
    bool ok=false;
    if(!fm_db_open(&db,db_path,true,error)||!fm_db_sync_feeds(&db,feeds,error)||!fm_db_cache_load(&db,feeds,error))goto finish;
    if(!feeds->len){
        ok=true;
        goto finish;
    }
    int count=options->threads;
    if((size_t)count>feeds->len)count=(int)feeds->len;
    Run run={
        .feeds=feeds,.options=options,.qcap=(size_t)count
    };
    atomic_init(&run.next,0);
    run.queue=fm_calloc(run.qcap,sizeof *run.queue);
    pthread_mutex_init(&run.queue_mutex,NULL);
    pthread_cond_init(&run.readable,NULL);
    pthread_cond_init(&run.writable,NULL);
    for(int i=0;i<CURL_LOCK_DATA_LAST;i++)pthread_mutex_init(&run.share_locks[i],NULL);
    run.share=curl_share_init();
    if(run.share) {
        curl_share_setopt(run.share,CURLSHOPT_LOCKFUNC,share_lock);
        curl_share_setopt(run.share,CURLSHOPT_UNLOCKFUNC,share_unlock);
        curl_share_setopt(run.share,CURLSHOPT_USERDATA,&run);
        curl_share_setopt(run.share,CURLSHOPT_SHARE,CURL_LOCK_DATA_DNS);
        curl_share_setopt(run.share,CURLSHOPT_SHARE,CURL_LOCK_DATA_SSL_SESSION);
    }
    pthread_t *threads=fm_calloc((size_t)count,sizeof *threads);
    int created=0;
    for(int i=0;i<count;i++) {
        int rc=pthread_create(&threads[i],NULL,worker,&run);
        if(rc){
            fm_error(error,"Cannot create worker: %s",strerror(rc));
            atomic_store(&options->cancel,true);
            break;
        }
        created++;
    }
    int ended=0;
    FmString errors={
        0
    };
    int64_t now=(int64_t)time(NULL);
    bool db_ok=true;
    while(ended<created) {
        Result *r=pop(&run);
        if(r->end){
            ended++;
            result_free(r);
            continue;
        }
        FmFeed *f=&feeds->items[r->index];
        stats->done++;
        stats->bytes+=r->body.len;
        char *host=fm_url_host(f->url);
        if(r->error && !is_cancelled(&run)) {
            stats->failed++;
            fm_string_printf(&errors,"%zu [%s] %s: %s\n",r->index+1,f->tag,host,r->error);
        }
        else if(!is_cancelled(&run) && db_ok) {
            if(r->status==304) {
                db_ok=fm_db_not_modified(&db,f,r->etag,r->modified,now,error);
                if(db_ok)stats->unchanged++;
            }
            else {
                db_ok=fm_db_store(&db,f,&r->articles,r->etag,r->modified,now,&stats->inserted,&stats->updated,error);
                if(db_ok)stats->downloaded++;
            }
            if(!db_ok)atomic_store(&options->cancel,true);
        }
        if(progress)progress(stats->done,stats->total,host,user);
        free(host);
        result_free(r);
    }
    for(int i=0;i<created;i++)pthread_join(threads[i],NULL);
    stats->cancelled=is_cancelled(&run);
    stats->errors=fm_string_take(&errors);
    free(threads);
    if(run.share)curl_share_cleanup(run.share);
    for(int i=0;i<CURL_LOCK_DATA_LAST;i++)pthread_mutex_destroy(&run.share_locks[i]);
    pthread_mutex_destroy(&run.queue_mutex);
    pthread_cond_destroy(&run.readable);
    pthread_cond_destroy(&run.writable);
    free(run.queue);
    ok=created==count && db_ok;
    finish:
    fm_db_close(&db);
    flock(lockfd,LOCK_UN);
    close(lockfd);
    stats->seconds=fm_clock()-start;
    return ok;
}
