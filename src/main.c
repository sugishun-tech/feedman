/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "feedman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <locale.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
static volatile sig_atomic_t interrupted=0;
static void interrupt_handler(int sig) {
    (void)sig;
    interrupted=1;
}

static void print_progress(size_t done,size_t total,const char *source,void *user) {
    (void)user;
    fprintf(stderr,"[%zu/%zu] %s\n",done,total,source);
}

static void print_stats(const FmStats *s) {
    printf("{\"feeds\":%zu,\"done\":%zu,\"downloaded\":%zu,\"not_modified\":%zu,\"failed\":%zu,\"inserted\":%zu,\"updated\":%zu,\"decoded_bytes\":%llu,\"seconds\":%.6f,\"cancelled\":%s}\n",
    s->total,s->done,s->downloaded,s->unchanged,s->failed,s->inserted,s->updated,(unsigned long long)s->bytes,s->seconds,s->cancelled?"true":"false");
    if(s->errors&&*s->errors)fputs(s->errors,stderr);
}

static bool number(const char *s,int min,int max,int *out) {
    char *end=NULL;
    errno=0;
    long n=strtol(s,&end,10);
    if(errno||end==s||*end||n<min||n>max)return false;
    *out=(int)n;
    return true;
}

static char *xdg_dir(const char *env,const char *fallback) {
    const char *value=getenv(env);
    if(value&&*value=='/')return fm_path_join(value,"feedman");
    const char *home=getenv("HOME");
    if(!home||!*home)home=".";
    char *base=fm_path_join(home,fallback),*path=fm_path_join(base,"feedman");
    free(base);
    return path;
}

static void help(void) {
    puts("feedman " FM_VERSION " — fast, local-first C RSS/Atom reader\n"
    "Usage: feedman [options]\n\n"
    "  --feeds PATH          feeds.txt location (tag URL / URL-only)\n"
    "  --data-dir DIR        database and window state directory\n"
    "  --threads N          concurrent fetch workers, 1..64 (default 8)\n"
    "  --timeout SECONDS    per-feed total timeout, 1..300 (default 30)\n"
    "  --date YYYYMMDD      initial calendar date (also YYYY-MM-DD)\n"
    "  --published          use publication date instead of first fetch date\n"
    "  --fetch              fetch once without GUI; JSON statistics on stdout\n"
    "  --check-feeds        validate subscriptions without network requests\n"
    "  --import-myrss DIR   import legacy data/YYYY/MM/DD/*.json (no GUI)\n"
    "  --stats              print stored article count (no GUI)\n"
    "  --version            print version\n"
    "  --help               show this help\n\n"
    "No network requests are made merely by opening the GUI.\n"
    "Default feeds: ./feeds.txt, otherwise $XDG_CONFIG_HOME/feedman/feeds.txt.\n"
    "Default data: $XDG_DATA_HOME/feedman (or ~/.local/share/feedman).\n"
    "Exit: 0 success, 1 error, 2 partial feed/import failure, 130 cancelled.");
}

int main(int argc,char **argv) {
    setlocale(LC_ALL,"");
    umask(0077);
    FmAppOptions options={
        .threads=8,.timeout=30
    };
    bool fetch=false,check=false,stats_only=false;
    char *import=NULL,*error=NULL;
    int code=0;
    static const struct option long_options[]={
        {
            "feeds",required_argument,NULL,'f'
        },{
            "data-dir",required_argument,NULL,'d'
        },
        {
            "threads",required_argument,NULL,'j'
        },{
            "timeout",required_argument,NULL,'t'
        },
        {
            "date",required_argument,NULL,'D'
        },{
            "published",no_argument,NULL,'p'
        },
        {
            "fetch",no_argument,NULL,'F'
        },{
            "check-feeds",no_argument,NULL,'c'
        },
        {
            "import-myrss",required_argument,NULL,'i'
        },{
            "stats",no_argument,NULL,'s'
        },
        {
            "version",no_argument,NULL,'v'
        },{
            "help",no_argument,NULL,'h'
        },{
            NULL,0,NULL,0
        }
    };
    int opt;
    while((opt=getopt_long(argc,argv,"",long_options,NULL))!=-1) {
        switch(opt) {
            case 'f': free(options.feeds_path);
            options.feeds_path=fm_strdup(optarg);
            break;
            case 'd': free(options.data_dir);
            options.data_dir=fm_strdup(optarg);
            break;
            case 'j': options.threads_set=true;
            if(!number(optarg,1,64,&options.threads)){
                fm_error(&error,"--threads must be 1..64");
                goto bad;
            }
            break;
            case 't': if(!number(optarg,1,300,&options.timeout)){
                fm_error(&error,"--timeout must be 1..300");
                goto bad;
            }
            break;
            case 'D': if(!fm_date_normalize(optarg,options.date)){
                fm_error(&error,"Invalid calendar date: %s",optarg);
                goto bad;
            }
            break;
            case 'p': options.by_publication=true;
            break;
            case 'F': fetch=true;
            break;
            case 'c': check=true;
            break;
            case 's':stats_only=true;
            break;
            case 'i':import=optarg;
            break;
            case 'v':puts("feedman " FM_VERSION);
            goto cleanup;
            case 'h':help();
            goto cleanup;
            default:code=1;
            goto cleanup;
        }
    }
    if(optind<argc){
        fm_error(&error,"Unexpected positional argument");
        goto bad;
    }
    if((int)fetch+(int)check+(int)stats_only+(import?1:0)>1){
        fm_error(&error,"Choose one of --fetch, --check-feeds, --stats, --import-myrss");
        goto bad;
    }
    fm_global_init();
    if(!options.feeds_path) {
        if(access("feeds.txt",F_OK)==0)options.feeds_path=fm_strdup("feeds.txt");
        else {
            char *config=xdg_dir("XDG_CONFIG_HOME",".config");
            if(!fm_mkdirs(config,&error)){
                free(config);
                goto shutdown;
            }
            options.feeds_path=fm_path_join(config,"feeds.txt");
            free(config);
            if(access(options.feeds_path,F_OK)!=0&&!fm_write_file_atomic(options.feeds_path,fm_example_feeds(),&error))goto shutdown;
        }
    }
    if(!options.data_dir)options.data_dir=xdg_dir("XDG_DATA_HOME",".local/share");
    if(!check && !fm_mkdirs(options.data_dir,&error))goto shutdown;
    options.db_path=fm_path_join(options.data_dir,"feedman.db");
    if(import) {
        FmStats stats={
            0
        };
        bool ok=fm_import_myrss(import,options.db_path,&stats,&error);
        print_stats(&stats);
        code=ok?(stats.failed?2:0):1;
        fm_stats_free(&stats);
    }
    else if(stats_only) {
        FmDb db={
            0
        };
        if(fm_db_open(&db,options.db_path,false,&error))printf("{\"articles\":%lld,\"fts5_trigram\":%s}\n",(long long)fm_db_count(&db),db.fts?"true":"false");
        fm_db_close(&db);
    }
    else if(fetch||check) {
        FmFeeds feeds={
            0
        };
        if(!fm_feeds_load(options.feeds_path,&feeds,&error))goto shutdown;
        if(check)printf("OK: %zu unique feeds\n",feeds.len);
        else {
            signal(SIGINT,interrupt_handler);
            signal(SIGTERM,interrupt_handler);
            FmFetchOptions fo={
                .threads=options.threads,.timeout=options.timeout,.max_bytes=FM_MAX_FEED_BYTES,.signal_cancel=&interrupted
            };
            atomic_init(&fo.cancel,false);
            FmStats stats={
                0
            };
            bool ok=fm_fetch_all(options.db_path,&feeds,&fo,print_progress,NULL,&stats,&error);
            print_stats(&stats);
            code=!ok?1:(stats.cancelled?130:(stats.failed?2:0));
            fm_stats_free(&stats);
        }
        fm_feeds_free(&feeds);
    }
    else {
#ifdef FEEDMAN_WITH_GUI
        code=fm_ui_run(&options);
#else
        fm_error(&error,"Built without GUI. Reconfigure with -DFEEDMAN_GUI=ON or use --fetch.");
#endif
    }
    shutdown:
    fm_global_cleanup();
    bad:
    if(error){
        fprintf(stderr,"feedman: %s\n",error);
        code=1;
    }
    cleanup:
    free(error);
    free(options.feeds_path);
    free(options.data_dir);
    free(options.db_path);
    return code;
}
