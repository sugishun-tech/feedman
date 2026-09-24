/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "feedman.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
int main(void) {
    setenv("TZ","UTC",1);
    tzset();
    fm_global_init();
    char root[]="/tmp/feedman-ui-test-XXXXXX";
    if(!mkdtemp(root))return 1;
    char *path=fm_path_join(root,"feedman.db"),*feeds_path=fm_path_join(root,"feeds.txt"),*error=NULL;
    FmFeeds feeds={
        0
    };
    FmDb db={
        0
    };
    FmArticles articles={
        0
    };
    bool ok=fm_write_file_atomic(feeds_path,"IT https://example.com/rss\n",&error);
    if(ok)ok=fm_feeds_load(feeds_path,&feeds,&error);
    if(ok)ok=fm_db_open(&db,path,true,&error);
    if(ok)ok=fm_db_sync_feeds(&db,&feeds,&error);
    FmString xml={
        0
    };
    fm_string_append(&xml,"<rss><channel><title>UI fixtures</title>");
    for(int i=0;i<300;i++)fm_string_printf(&xml,"<item><guid>%d</guid><title>UI article %d</title><link>https://example.com/%d</link><pubDate>Sat, 12 Sep 2026 04:00:00 GMT</pubDate><description>%s</description></item>",i,i,i,i==5?"京都市の本文":"本文のテスト");
    fm_string_append(&xml,"</channel></rss>");
    if(ok)ok=fm_parse_feed(xml.data,xml.len,feeds.items[0].url,&articles,&error);
    if(ok)ok=fm_db_store(&db,&feeds.items[0],&articles,"","",(int64_t)time(NULL),NULL,NULL,&error);
    fm_db_close(&db);
    free(xml.data);
    fm_articles_free(&articles);
    fm_feeds_free(&feeds);
    int result=1;
    if(ok){
        FmAppOptions options={
            .feeds_path=feeds_path,.data_dir=root,.db_path=path,.threads=8,.timeout=30
        };
        result=fm_ui_run(&options);
    }
    else fprintf(stderr,"UI fixture setup: %s\n",error?error:"unknown error");
    const char *files[]={
        "feedman.db","feedman.db-wal","feedman.db-shm","ui.ini","feeds.txt"
    };
    for(size_t i=0;i<sizeof files/sizeof *files;i++){
        char *f=fm_path_join(root,files[i]);
        unlink(f);
        free(f);
    }
    rmdir(root);
    free(error);
    free(path);
    free(feeds_path);
    fm_global_cleanup();
    return result;
}
