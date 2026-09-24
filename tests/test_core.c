/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "feedman.h"
#include "test_server.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
static int checks=0;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}} while(0)
static void remove_tree(const char *path) {
    DIR *d=opendir(path);
    if(!d){
        unlink(path);
        return;
    }
    struct dirent *e;
    while((e=readdir(d))){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
        char *p=fm_path_join(path,e->d_name);
        struct stat st;
        if(lstat(p,&st)==0&&S_ISDIR(st.st_mode))remove_tree(p);
        else unlink(p);
        free(p);
    }
    closedir(d);
    rmdir(path);
}
static char *temporary(void) {
    char *path=fm_strdup("/tmp/feedman-test-XXXXXX");
    CHECK(mkdtemp(path)!=NULL);
    return path;
}
static FmArticles fixture(const char *name) {
    char *path=fm_path_join(FM_TEST_FIXTURES,name),*data=NULL,*error=NULL;
    size_t len=0;
    FmArticles result={
        0
    };
    CHECK(fm_read_file(path,FM_MAX_FEED_BYTES,&data,&len,&error));
    CHECK(fm_parse_feed(data,len,"https://example.com/feed.xml",&result,&error));
    CHECK(error==NULL);
    free(path);
    free(data);
    return result;
}
static void dates(void) {
    char d[11];
    CHECK(fm_date_normalize("20260912",d));
    CHECK(!strcmp(d,"2026-09-12"));
    CHECK(fm_date_normalize("2024-02-29",d));
    CHECK(!fm_date_normalize("20260229",d));
    CHECK(!fm_date_normalize("20261312",d));
    CHECK(!fm_date_normalize("20260900",d));
    CHECK(!fm_date_normalize("20260912x",d));
    CHECK(!fm_date_normalize("00000101",d));
    int64_t a=0,b=0,c=0;
    CHECK(fm_parse_time("2026-09-12T13:00:00+09:00",&a));
    CHECK(fm_parse_time("2026-09-12T04:00:00Z",&b));
    CHECK(fm_parse_time("Sat, 12 Sep 2026 04:00:00 GMT",&c));
    CHECK(a==b&&b==c);
    CHECK(fm_parse_time("2026-09-12T13:00:00.123+0900",&c));
    CHECK(a==c);
    CHECK(!fm_parse_time("2026-02-30T04:00:00Z",&c));
    CHECK(!fm_parse_time("2026-09-12T24:00:00Z",&c));
    CHECK(!fm_parse_time("2026-09-12T04:00:00junk",&c));
    fm_local_day(a,d);
    CHECK(!strcmp(d,"2026-09-12"));
    puts("PASS dates / timezone / invalid input");
}
static void config(void) {
    FmFeeds f={
        0
    };
    char *err=NULL;
    CHECK(fm_feeds_parse("\xef\xbb\xbf# feeds\r\nIT https://example.com/rss\r\n\nhttps://example.org/rss\n経済\thttps://example.net/rss\n- https://example.edu/rss # note\nIT https://example.com/rss\n",&f,&err));
    CHECK(f.len==4);
    CHECK(!strcmp(f.items[0].tag,"IT"));
    CHECK(!strcmp(f.items[1].tag,""));
    CHECK(!strcmp(f.items[2].tag,"経済"));
    CHECK(!strcmp(f.items[3].tag,""));
    fm_feeds_free(&f);
    CHECK(!fm_feeds_parse("x ftp://example.com/a",&f,&err));
    CHECK(strstr(err,"Line 1")!=NULL);
    free(err);
    err=NULL;
    CHECK(!fm_feeds_parse("x https://example.com\ny https://example.com",&f,&err));
    free(err);
    err=NULL;
    CHECK(!fm_feeds_parse("two tags https://example.com",&f,&err));
    free(err);
    err=NULL;
    CHECK(fm_feeds_parse("# no subscriptions",&f,&err));
    CHECK(f.len==0);
    fm_feeds_free(&f);
    CHECK(fm_url_valid("https://example.com/a?b=1#x"));
    CHECK(!fm_url_valid("javascript:alert(1)"));
    CHECK(!fm_url_valid("file:///etc/passwd"));
    CHECK(!fm_url_valid("https://example.com/\r\nx"));
    char *url=fm_url_resolve("../a","https://example.com/news/f.xml");
    CHECK(!strcmp(url,"https://example.com/a"));
    free(url);
    puts("PASS config / tags / URL policy / duplicates");
}
static void parser(void) {
    FmArticles rss=fixture("rss.xml");
    CHECK(rss.len==2);
    CHECK(!strcmp(rss.items[0].key,"stable-id"));
    CHECK(!strcmp(rss.items[0].url,"https://example.com/articles/one"));
    CHECK(strstr(rss.items[0].plain,"京都市")!=NULL);
    CHECK(strstr(rss.items[0].plain,"secret_script")==NULL);
    CHECK(rss.items[1].published==0);
    CHECK(*rss.items[1].key);
    CHECK(!*rss.items[1].url);
    fm_articles_free(&rss);
    FmArticles atom=fixture("atom.xml");
    CHECK(atom.len==2);
    CHECK(!strcmp(atom.items[0].url,"https://example.com/news/section/article.html"));
    CHECK(!strcmp(atom.items[0].title,"Hello Atom"));
    CHECK(strstr(atom.items[0].plain,"日本語の本文")!=NULL);
    CHECK(strstr(atom.items[1].plain,"<script>not code</script>")!=NULL);
    fm_articles_free(&atom);
    FmArticles rdf=fixture("rdf.xml");
    CHECK(rdf.len==1);
    CHECK(rdf.items[0].published!=0);
    fm_articles_free(&rdf);
    char *file=fm_path_join(FM_TEST_FIXTURES,"xxe.xml"),*xml=NULL,*err=NULL;
    size_t len=0;
    CHECK(fm_read_file(file,4096,&xml,&len,&err));
    FmArticles a={
        0
    };
    CHECK(!fm_parse_feed(xml,len,"https://example.com",&a,&err));
    CHECK(strstr(err,"DTD")!=NULL);
    free(err);
    err=NULL;
    free(file);
    free(xml);
    CHECK(!fm_parse_feed("<html>hello</html>",18,"https://example.com",&a,&err));
    free(err);
    err=NULL;
    CHECK(!fm_parse_feed("<rss><channel>",14,"https://example.com",&a,&err));
    free(err);
    char *plain=fm_html_plain("<p>A &amp; B</p><style>secret</style><p>Next<br>line</p>");
    CHECK(strstr(plain,"A & B")!=NULL);
    CHECK(strstr(plain,"secret")==NULL);
    free(plain);
    const char *literal="<rss><channel><item><title>C &amp; &lt;stdio.h&gt;</title><guid>x</guid></item></channel></rss>";
    FmArticles literal_articles={0};char *literal_error=NULL;
    CHECK(fm_parse_feed(literal,strlen(literal),"https://example.com/",&literal_articles,&literal_error));
    CHECK(!strcmp(literal_articles.items[0].title,"C & <stdio.h>"));
    fm_articles_free(&literal_articles);free(literal_error);
    puts("PASS RSS 2 / RSS 1 / Atom / XHTML / XXE / invalid XML");
}
static void database(void) {
    char *dir=temporary(),*path=fm_path_join(dir,"feedman.db"),*err=NULL;
    FmDb db={
        0
    };
    CHECK(fm_db_open(&db,path,true,&err));
    FmFeeds f={
        0
    };
    CHECK(fm_feeds_parse("IT https://example.com/feed.xml",&f,&err));
    CHECK(fm_db_sync_feeds(&db,&f,&err));
    FmArticles a=fixture("rss.xml");
    int64_t day=0;
    CHECK(fm_parse_time("2026-09-25T12:00:00+09:00",&day));
    size_t inserted=0,updated=0;
    CHECK(fm_db_store(&db,&f.items[0],&a,"\"v1\"","",day,&inserted,&updated,&err));
    CHECK(inserted==2&&updated==0);
    CHECK(fm_db_count(&db)==2);
    fm_db_close(&db);
    CHECK(fm_db_open(&db,path,false,&err));
    CHECK(fm_db_store(&db,&f.items[0],&a,"\"v1\"","",day+86400,&inserted,&updated,&err));
    CHECK(inserted==2&&updated==0);
    CHECK(fm_db_count(&db)==2);
    FmQuery q={
        .limit=250
    };
    memcpy(q.day,"2026-09-25",11);
    FmRows rows={
        0
    };
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==2);
    int64_t id=rows.items[0].id;
    CHECK(!strcmp(rows.items[0].tag,"IT"));
    fm_rows_free(&rows);
    CHECK(fm_db_mark_read(&db,id,true,&err));
    q.unread_only=true;
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==1);
    fm_rows_free(&rows);
    q.unread_only=false;
    q.text="京都市";
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==1);
    fm_rows_free(&rows);
    q.text="京都";
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==1);
    fm_rows_free(&rows);
    q.text="%' OR 1=1 --";
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==0);
    fm_rows_free(&rows);
    q.text="\"hello\"";
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==0);
    fm_rows_free(&rows);
    q.text=NULL;
    free(f.items[0].tag);
    f.items[0].tag=fm_strdup("ニュース");
    CHECK(fm_db_sync_feeds(&db,&f,&err));
    q.tag="ニュース";
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==2);
    fm_rows_free(&rows);
    q.tag="IT";
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==0);
    fm_rows_free(&rows);
    q.tag=NULL;
    unsigned mask=0;
    CHECK(fm_db_month(&db,2026,9,false,&mask,&err));
    CHECK(mask==(1u<<24));
    CHECK(fm_db_month(&db,2026,9,true,&mask,&err));
    CHECK((mask&(1u<<11))!=0);
    char neighbor[11];
    CHECK(fm_db_neighbor(&db,"2026-09-24",false,true,neighbor,&err));
    CHECK(!strcmp(neighbor,"2026-09-25"));
    FmArticle article={
        0
    };
    CHECK(fm_db_article(&db,id,&article,&err));
    CHECK(article.read);
    CHECK(article.first_seen==day);
    fm_article_free(&article);
    free(a.items[1].html);
    free(a.items[1].plain);
    a.items[1].html=fm_strdup("<p>changed body</p>");
    a.items[1].plain=fm_strdup("changed body");
    CHECK(fm_db_store(&db,&f.items[0],&a,"\"v2\"","",day+86400,&inserted,&updated,&err));
    CHECK(updated==1);
    q.by_publication=true;
    memcpy(q.day,"2026-09-25",11);
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==1);
    fm_rows_free(&rows);
    CHECK(fm_db_cache_load(&db,&f,&err));
    CHECK(!strcmp(f.items[0].etag,"\"v2\""));
    bool had_fts=db.fts;
    db.fts=false;
    q=(FmQuery){
        .text="京都市",.limit=1
    };
    CHECK(fm_db_rows(&db,&q,&rows,&err));
    CHECK(rows.total==1);
    fm_rows_free(&rows);
    db.fts=had_fts;
    fm_articles_free(&a);
    fm_feeds_free(&f);
    fm_db_close(&db);
    CHECK(err==NULL);
    free(path);
    remove_tree(dir);
    free(dir);
    puts("PASS SQLite / persistent dedup / tags / Japanese search / dates / read state / update");
}
static FmFeeds server_feeds(TestServer *s,int count,const char *prefix) {
    FmString text={
        0
    };
    for(int i=0;i<count;i++)fm_string_printf(&text,"tag%d http://127.0.0.1:%d/%s/%d\n",i,s->port,prefix,i);
    FmFeeds feeds={
        0
    };
    char *error=NULL;
    CHECK(fm_feeds_parse(text.data,&feeds,&error));
    free(text.data);
    return feeds;
}
static void network(void) {
    TestServer server;
    CHECK(test_server_start(&server,30)==0);
    FmFeeds feeds=server_feeds(&server,8,"feed");
    char *dir=temporary(),*path=fm_path_join(dir,"feedman.db"),*err=NULL;
    FmFetchOptions o={
        .threads=4,.timeout=5,.max_bytes=FM_MAX_FEED_BYTES
    };
    atomic_init(&o.cancel,false);
    FmStats s={
        0
    };
    CHECK(fm_fetch_all(path,&feeds,&o,NULL,NULL,&s,&err));
    CHECK(s.inserted==160);
    CHECK(s.failed==0);
    CHECK(atomic_load(&server.max_active)>=2);
    fm_stats_free(&s);
    CHECK(fm_fetch_all(path,&feeds,&o,NULL,NULL,&s,&err));
    CHECK(s.unchanged==8);
    CHECK(s.inserted==0);
    CHECK(s.bytes==0);
    CHECK(atomic_load(&server.conditional)==8);
    fm_stats_free(&s);
    fm_feeds_free(&feeds);
    FmString text={
        0
    };
    fm_string_printf(&text,"ok http://127.0.0.1:%d/redirect\nbad http://127.0.0.1:%d/error\ninvalid http://127.0.0.1:%d/invalid\nblocked http://127.0.0.1:%d/file-redirect",server.port,server.port,server.port,server.port);
    CHECK(fm_feeds_parse(text.data,&feeds,&err));
    free(text.data);
    CHECK(fm_fetch_all(path,&feeds,&o,NULL,NULL,&s,&err));
    CHECK(s.downloaded==1);
    CHECK(s.failed==3);
    CHECK(s.inserted==20);
    fm_stats_free(&s);
    fm_feeds_free(&feeds);
    feeds=server_feeds(&server,1,"large");
    o.max_bytes=1024;
    CHECK(fm_fetch_all(path,&feeds,&o,NULL,NULL,&s,&err));
    CHECK(s.failed==1);
    CHECK(s.inserted==0);
    fm_stats_free(&s);
    fm_feeds_free(&feeds);
    CHECK(err==NULL);
    test_server_stop(&server);
    free(path);
    remove_tree(dir);
    free(dir);
    puts("PASS parallel HTTP / ETag 304 / redirects / partial failures / size limit");
}
typedef struct {
    FmFetchOptions *options;
}
Cancel;
static void *cancel_later(void *data) {
    struct timespec delay={
        .tv_nsec=100000000
    };
    nanosleep(&delay,NULL);
    Cancel *c=data;
    atomic_store(&c->options->cancel,true);
    return NULL;
}
static void cancellation(void) {
    TestServer server;
    CHECK(test_server_start(&server,0)==0);
    FmFeeds feeds=server_feeds(&server,4,"slow");
    char *dir=temporary(),*path=fm_path_join(dir,"feedman.db"),*err=NULL;
    FmFetchOptions o={
        .threads=2,.timeout=5,.max_bytes=FM_MAX_FEED_BYTES
    };
    atomic_init(&o.cancel,false);
    Cancel c={
        .options=&o
    };
    pthread_t thread;
    CHECK(pthread_create(&thread,NULL,cancel_later,&c)==0);
    FmStats s={
        0
    };
    double start=fm_clock();
    CHECK(fm_fetch_all(path,&feeds,&o,NULL,NULL,&s,&err));
    double elapsed=fm_clock()-start;
    pthread_join(thread,NULL);
    CHECK(s.cancelled);
    CHECK(s.inserted==0);
    CHECK(elapsed<4.0);
    fm_stats_free(&s);
    fm_feeds_free(&feeds);
    test_server_stop(&server);
    free(path);
    remove_tree(dir);
    free(dir);
    puts("PASS cancellation / in-flight close safety");
}
static void migration(void) {
    char *dir=temporary(),*old=fm_path_join(dir,"old"),*path=fm_path_join(dir,"feedman.db"),*err=NULL;
    CHECK(fm_mkdirs(old,&err));
    char *file=fm_path_join(old,"one.json");
    CHECK(fm_write_file_atomic(file,"{\"title\":\"old article\",\"link\":\"https://example.com/old\",\"summary\":\"<p>旧本文</p>\",\"date\":\"2026-09-12 13:00\"}",&err));
    FmStats s={
        0
    };
    CHECK(fm_import_myrss(old,path,&s,&err));
    CHECK(s.inserted==1);
    fm_stats_free(&s);
    CHECK(fm_import_myrss(old,path,&s,&err));
    CHECK(s.inserted==0);
    fm_stats_free(&s);
    FmDb db={
        0
    };
    CHECK(fm_db_open(&db,path,false,&err));
    CHECK(fm_db_count(&db)==1);
    fm_db_close(&db);
    free(file);
    free(old);
    free(path);
    remove_tree(dir);
    free(dir);
    puts("PASS legacy JSON import / repeat import");
}
static void benchmark(void) {
    TestServer server;
    CHECK(test_server_start(&server,150)==0);
    FmFeeds feeds=server_feeds(&server,16,"feed");
    double times[2]={
        0
    };
    size_t inserted[2]={
        0
    };
    for(int pass=0;pass<2;pass++) {
        char *dir=temporary(),*path=fm_path_join(dir,"feedman.db"),*error=NULL;
        FmFetchOptions o={
            .threads=pass?8:1,.timeout=10,.max_bytes=FM_MAX_FEED_BYTES
        };
        atomic_init(&o.cancel,false);
        FmStats s={
            0
        };
        CHECK(fm_fetch_all(path,&feeds,&o,NULL,NULL,&s,&error));
        CHECK(s.failed==0);
        times[pass]=s.seconds;
        inserted[pass]=s.inserted;
        fm_stats_free(&s);
        free(path);
        remove_tree(dir);
        free(dir);
    }
    printf("{\"benchmark\":\"local HTTP 16 feeds, 150ms response delay each, 20 entries per feed\",\"threads_1_seconds\":%.6f,\"threads_8_seconds\":%.6f,\"speedup\":%.3f,\"articles_each\":%zu,\"equal_result_count\":%s}\n",times[0],times[1],times[0]/times[1],inserted[0],inserted[0]==inserted[1]?"true":"false");
    fm_feeds_free(&feeds);
    test_server_stop(&server);
}
int main(int argc,char **argv) {
    setenv("TZ","Asia/Tokyo",1);
    tzset();
    fm_global_init();
    if(argc>1&&!strcmp(argv[1],"--benchmark"))benchmark();
    else {
        dates();
        config();
        parser();
        database();
        network();
        cancellation();
        migration();
        printf("PASS all %d checks\n",checks);
    }
    fm_global_cleanup();
    return 0;
}
