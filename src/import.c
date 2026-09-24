/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "feedman.h"
#include <json-c/json.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
typedef struct {
    FmDb *db;
    FmStats *stats;
    int64_t now;
    FmString warnings;
    size_t visited;
} Import;
static const char *jstring(json_object *obj,const char *key) {
    json_object *v=NULL;
    if(json_object_object_get_ex(obj,key,&v)&&json_object_is_type(v,json_type_string))return json_object_get_string(v);
    return "";
}

static bool import_file(Import *im,const char *path,char **error) {
    im->stats->total++;
    char *text=NULL;
    size_t len=0;
    char *local=NULL;
    if(!fm_read_file(path,2u*1024u*1024u,&text,&len,&local))goto invalid;
    json_tokener *tok=json_tokener_new();
    json_object *obj=json_tokener_parse_ex(tok,text,(int)len);
    bool valid=json_tokener_get_error(tok)==json_tokener_success && obj && json_object_is_type(obj,json_type_object);
    json_tokener_free(tok);
    free(text);
    text=NULL;
    if(!valid) {
        if(obj)json_object_put(obj);
        fm_error(&local,"Invalid JSON");
        goto invalid;
    }
    const char *link=jstring(obj,"link"),*title=jstring(obj,"title");
    if(!fm_url_valid(link)||!*title) {
        json_object_put(obj);
        fm_error(&local,"Missing title or HTTP(S) link");
        goto invalid;
    }
    char *host=fm_url_host(link);
    FmString source={
        0
    };
    fm_string_printf(&source,"myrss-import:%s",host);
    free(host);
    FmFeed feed={
        .url=source.data,.tag="import"
    };
    FmFeeds feeds={
        .items=&feed,.len=1
    };
    if(!fm_db_sync_feeds(im->db,&feeds,error)) {
        free(source.data);
        json_object_put(obj);
        return false;
    }
    FmArticle a={
        .key=fm_strdup(link),.title=fm_strdup(title),.url=fm_strdup(link),.html=fm_strdup(jstring(obj,"summary")),.first_seen=im->now,.legacy=true
    };
    fm_parse_time(jstring(obj,"date"),&a.published);
    a.plain=fm_html_plain(a.html);
    FmArticles articles={
        .items=&a,.len=1,.title="myrss import"
    };
    bool ok=fm_db_store(im->db,&feed,&articles,NULL,NULL,im->now,&im->stats->inserted,&im->stats->updated,error);
    fm_article_free(&a);
    free(source.data);
    json_object_put(obj);
    im->stats->done++;
    return ok;
    invalid:
    im->stats->failed++;
    im->stats->done++;
    fm_string_printf(&im->warnings,"Skipped JSON: %s (%s)\n",path,local?local:"invalid");
    free(local);
    free(text);
    return true;
}

static bool walk(Import *im,const char *path,unsigned depth,char **error) {
    if(depth>8){
        fm_error(error,"Import directory is too deeply nested");
        return false;
    }
    DIR *dir=opendir(path);
    if(!dir){
        fm_error(error,"Cannot open import directory: %s",path);
        return false;
    }
    struct dirent *entry;
    bool ok=true;
    while(ok&&(entry=readdir(dir))) {
        if(entry->d_name[0]=='.')continue;
        if(++im->visited>1000000){
            fm_error(error,"Import directory contains too many entries");
            ok=false;
            break;
        }
        char *file=fm_path_join(path,entry->d_name);
        struct stat st;
        if(lstat(file,&st)!=0){
            free(file);
            continue;
        }
        if(S_ISDIR(st.st_mode))ok=walk(im,file,depth+1,error);
        else if(S_ISREG(st.st_mode)) {
            size_t n=strlen(entry->d_name);
            if(n>5&&!strcmp(entry->d_name+n-5,".json"))ok=import_file(im,file,error);
        }
        free(file);
    }
    closedir(dir);
    return ok;
}

bool fm_import_myrss(const char *dir,const char *db_path,FmStats *stats,char **error) {
    *stats=(FmStats){
        0
    };
    double start=fm_clock();
    FmDb db={
        0
    };
    FmString lock={
        0
    };
    fm_string_printf(&lock,"%s.fetch.lock",db_path);
    int fd=open(lock.data,O_CREAT|O_RDWR|O_CLOEXEC,0600);
    free(lock.data);
    if(fd<0||flock(fd,LOCK_EX|LOCK_NB)<0){
        fm_error(error,"Another fetch/import is running");
        if(fd>=0)close(fd);
        return false;
    }
    bool ok=fm_db_open(&db,db_path,true,error);
    Import im={
        .db=&db,.stats=stats,.now=(int64_t)time(NULL)
    };
    if(ok)ok=walk(&im,dir,0,error);
    stats->errors=fm_string_take(&im.warnings);
    stats->seconds=fm_clock()-start;
    fm_db_close(&db);
    flock(fd,LOCK_UN);
    close(fd);
    return ok;
}
