/* SPDX-License-Identifier: MIT */
#include "feedman.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
static bool exec_sql(FmDb *db,const char *sql,char **error) {
    char *message=NULL;
    int rc=sqlite3_exec(db->sql,sql,NULL,NULL,&message);
    if(rc!=SQLITE_OK)fm_error(error,"Database: %s",message?message:sqlite3_errmsg(db->sql));
    sqlite3_free(message);
    return rc==SQLITE_OK;
}

static bool prepare(FmDb *db,const char *sql,sqlite3_stmt **s,char **error) {
    if(sqlite3_prepare_v2(db->sql,sql,-1,s,NULL)!=SQLITE_OK) {
        fm_error(error,"Database: %s",sqlite3_errmsg(db->sql));
        return false;
    }
    return true;
}

static bool done(FmDb *db,sqlite3_stmt *s,char **error) {
    if(sqlite3_step(s)!=SQLITE_DONE) {
        fm_error(error,"Database: %s",sqlite3_errmsg(db->sql));
        return false;
    }
    return true;
}

static void text(sqlite3_stmt *s,int i,const char *v) {
    sqlite3_bind_text(s,i,v?v:"",-1,SQLITE_TRANSIENT);
}

static char *column(sqlite3_stmt *s,int i) {
    return fm_strdup((const char *)sqlite3_column_text(s,i));
}

static const char *schema=
"CREATE TABLE IF NOT EXISTS feeds("
"url TEXT PRIMARY KEY, tag TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',"
"etag TEXT NOT NULL DEFAULT '', modified TEXT NOT NULL DEFAULT '', checked_at INTEGER NOT NULL DEFAULT 0);"
"CREATE TABLE IF NOT EXISTS articles("
"id INTEGER PRIMARY KEY, feed_url TEXT NOT NULL REFERENCES feeds(url), entry_key TEXT NOT NULL,"
"title TEXT NOT NULL, url TEXT NOT NULL, html TEXT NOT NULL, search_text TEXT NOT NULL,"
"published INTEGER NOT NULL DEFAULT 0, first_seen INTEGER NOT NULL,"
"published_day TEXT NOT NULL, fetched_day TEXT NOT NULL,"
"is_read INTEGER NOT NULL DEFAULT 0, legacy INTEGER NOT NULL DEFAULT 0,"
"UNIQUE(feed_url,entry_key));"
"CREATE INDEX IF NOT EXISTS articles_fetched ON articles(fetched_day,first_seen DESC,id DESC);"
"CREATE INDEX IF NOT EXISTS articles_published ON articles(published_day,published DESC,id DESC);"
"CREATE INDEX IF NOT EXISTS articles_feed ON articles(feed_url);"
"CREATE INDEX IF NOT EXISTS articles_recent ON articles(first_seen DESC,id DESC);"
"CREATE INDEX IF NOT EXISTS articles_pub_recent ON articles(published DESC,id DESC);"
"PRAGMA user_version=1;";
bool fm_db_open(FmDb *db,const char *path,bool create,char **error) {
    *db=(FmDb){
        0
    };
    int flags=SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX|(create?SQLITE_OPEN_CREATE:0);
    if(sqlite3_open_v2(path,&db->sql,flags,NULL)!=SQLITE_OK) {
        fm_error(error,"Cannot open database: %s",db->sql?sqlite3_errmsg(db->sql):"out of memory");
        fm_db_close(db);
        return false;
    }
    sqlite3_busy_timeout(db->sql,4000);
    sqlite3_stmt *s=NULL;
    if(!prepare(db,"PRAGMA user_version",&s,error))goto fail;
    int version=sqlite3_step(s)==SQLITE_ROW?sqlite3_column_int(s,0):0;
    sqlite3_finalize(s);
    s=NULL;
    if(version>1) {
        fm_error(error,"Database was created by a newer feedman; refusing to modify it");
        goto fail;
    }
    if(!exec_sql(db,"PRAGMA foreign_keys=ON; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-8192;",error))goto fail;
    if(create) {
        if(!exec_sql(db,"PRAGMA journal_mode=WAL;",error) || !exec_sql(db,schema,error))goto fail;
        /* Optional acceleration: trigram supports Japanese substring search.
           If the SQLite build lacks it, queries fall back to literal scanning. */
        char *fts_error=NULL;
        bool newly=false;
        if(prepare(db,"SELECT 1 FROM sqlite_master WHERE name='article_fts'",&s,NULL)) {
            newly=sqlite3_step(s)!=SQLITE_ROW;
            sqlite3_finalize(s);
            s=NULL;
        }
        if(exec_sql(db,"CREATE VIRTUAL TABLE IF NOT EXISTS article_fts USING fts5(search_text, content='articles',content_rowid='id',tokenize='trigram');",&fts_error)) {
            const char *triggers=
            "CREATE TRIGGER IF NOT EXISTS articles_ai AFTER INSERT ON articles BEGIN "
            "INSERT INTO article_fts(rowid,search_text) VALUES(new.id,new.search_text);END;"
            "CREATE TRIGGER IF NOT EXISTS articles_ad AFTER DELETE ON articles BEGIN "
            "INSERT INTO article_fts(article_fts,rowid,search_text) VALUES('delete',old.id,old.search_text);END;"
            "CREATE TRIGGER IF NOT EXISTS articles_au AFTER UPDATE OF search_text ON articles "
            "WHEN old.search_text IS NOT new.search_text BEGIN "
            "INSERT INTO article_fts(article_fts,rowid,search_text) VALUES('delete',old.id,old.search_text);"
            "INSERT INTO article_fts(rowid,search_text) VALUES(new.id,new.search_text);END;";
            if(!exec_sql(db,triggers,error)){
                free(fts_error);
                goto fail;
            }
            if(newly && !exec_sql(db,"INSERT INTO article_fts(article_fts) VALUES('rebuild');",error)){
                free(fts_error);
                goto fail;
            }
        }
        free(fts_error);
    }
    if(!prepare(db,"SELECT 1 FROM sqlite_master WHERE name='article_fts'",&s,error))goto fail;
    db->fts=sqlite3_step(s)==SQLITE_ROW;
    sqlite3_finalize(s);
    return true;
    fail:
    fm_db_close(db);
    return false;
}

void fm_db_close(FmDb *db) {
    if(db->sql)sqlite3_close(db->sql);
    *db=(FmDb){
        0
    };
}

bool fm_db_sync_feeds(FmDb *db,FmFeeds *feeds,char **error) {
    if(!exec_sql(db,"BEGIN IMMEDIATE",error))return false;
    sqlite3_stmt *s=NULL;
    if(!prepare(db,"INSERT INTO feeds(url,tag) VALUES(?1,?2) ON CONFLICT(url) DO UPDATE SET tag=excluded.tag WHERE tag IS NOT excluded.tag",&s,error))goto fail;
    for(size_t i=0;i<feeds->len;i++) {
        text(s,1,feeds->items[i].url);
        text(s,2,feeds->items[i].tag);
        if(!done(db,s,error))goto fail;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    sqlite3_finalize(s);
    return exec_sql(db,"COMMIT",error);
    fail:
    sqlite3_finalize(s);
    exec_sql(db,"ROLLBACK",NULL);
    return false;
}

bool fm_db_cache_load(FmDb *db,FmFeeds *feeds,char **error) {
    sqlite3_stmt *s=NULL;
    if(!prepare(db,"SELECT etag,modified FROM feeds WHERE url=?1",&s,error))return false;
    for(size_t i=0;i<feeds->len;i++) {
        text(s,1,feeds->items[i].url);
        int rc=sqlite3_step(s);
        if(rc==SQLITE_ROW) {
            free(feeds->items[i].etag);
            free(feeds->items[i].modified);
            feeds->items[i].etag=column(s,0);
            feeds->items[i].modified=column(s,1);
        }
        else if(rc!=SQLITE_DONE) {
            fm_error(error,"Database: %s",sqlite3_errmsg(db->sql));
            sqlite3_finalize(s);
            return false;
        }
        sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    return true;
}

static bool update_cache(FmDb *db,const FmFeed *feed,const char *etag,const char *modified,const char *title_value,int64_t now,bool not_modified,char **error) {
    sqlite3_stmt *s=NULL;
    const char *sql=not_modified?
    "UPDATE feeds SET etag=CASE WHEN ?2='' THEN etag ELSE ?2 END,modified=CASE WHEN ?3='' THEN modified ELSE ?3 END,checked_at=?4 WHERE url=?1":
    "UPDATE feeds SET etag=?2,modified=?3,checked_at=?4,title=?5 WHERE url=?1";
    if(!prepare(db,sql,&s,error))return false;
    text(s,1,feed->url);
    text(s,2,etag);
    text(s,3,modified);
    sqlite3_bind_int64(s,4,now);
    if(!not_modified)text(s,5,title_value);
    bool ok=done(db,s,error);
    sqlite3_finalize(s);
    return ok;
}

bool fm_db_not_modified(FmDb *db,const FmFeed *feed,const char *etag,const char *modified,int64_t now,char **error) {
    return update_cache(db,feed,etag,modified,NULL,now,true,error);
}

bool fm_db_store(FmDb *db,const FmFeed *feed,const FmArticles *articles,const char *etag,const char *modified,int64_t fetched_at,size_t *inserted,size_t *updated,char **error) {
    size_t ni=0,nu=0;
    sqlite3_stmt *find=NULL,*ins=NULL,*upd=NULL;
    if(!exec_sql(db,"BEGIN IMMEDIATE",error))return false;
    if(!prepare(db,"SELECT id,first_seen FROM articles WHERE feed_url=?1 AND entry_key=?2",&find,error))goto fail;
    if(!prepare(db,"INSERT INTO articles(feed_url,entry_key,title,url,html,search_text,published,first_seen,published_day,fetched_day,legacy) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",&ins,error))goto fail;
    if(!prepare(db,"UPDATE articles SET title=?3,url=?4,html=?5,search_text=?6,published=?7,published_day=?9 WHERE feed_url=?1 AND entry_key=?2 AND (title IS NOT ?3 OR url IS NOT ?4 OR html IS NOT ?5 OR published IS NOT ?7)",&upd,error))goto fail;
    for(size_t i=0;i<articles->len;i++) {
        const FmArticle *a=&articles->items[i];
        char fd[11],pd[11];
        int64_t first=a->first_seen?a->first_seen:fetched_at;
        fm_local_day(first,fd);
        fm_local_day(a->published?a->published:first,pd);
        text(find,1,feed->url);
        text(find,2,a->key);
        int rc=sqlite3_step(find);
        if(rc!=SQLITE_ROW && rc!=SQLITE_DONE){
            fm_error(error,"Database lookup: %s",sqlite3_errmsg(db->sql));
            goto fail;
        }
        bool exists=rc==SQLITE_ROW;
        if(exists && !a->published) fm_local_day(sqlite3_column_int64(find,1),pd);
        sqlite3_reset(find);
        sqlite3_stmt *s=exists?upd:ins;
        FmString search={
            0
        };
        fm_string_printf(&search,"%s\n%s",a->title,a->plain?a->plain:"");
        text(s,1,feed->url);
        text(s,2,a->key);
        text(s,3,a->title);
        text(s,4,a->url);
        text(s,5,a->html);
        text(s,6,search.data);
        sqlite3_bind_int64(s,7,a->published);
        if(!exists)sqlite3_bind_int64(s,8,first);
        text(s,9,pd);
        if(!exists){
            text(s,10,fd);
            sqlite3_bind_int(s,11,a->legacy);
        }
        bool ok=done(db,s,error);
        free(search.data);
        if(!ok)goto fail;
        if(exists)nu+=(size_t)sqlite3_changes(db->sql);
        else ni++;
        sqlite3_reset(s);
        sqlite3_clear_bindings(s);
    }
    if(!update_cache(db,feed,etag,modified,articles->title,fetched_at,false,error))goto fail;
    sqlite3_finalize(find);
    sqlite3_finalize(ins);
    sqlite3_finalize(upd);
    if(!exec_sql(db,"COMMIT",error)){
        exec_sql(db,"ROLLBACK",NULL);
        return false;
    }
    if(inserted)*inserted+=ni;
    if(updated)*updated+=nu;
    return true;
    fail:
    sqlite3_finalize(find);
    sqlite3_finalize(ins);
    sqlite3_finalize(upd);
    exec_sql(db,"ROLLBACK",NULL);
    return false;
}

static int cancelled(void *data) {
    const atomic_bool *cancel=data;
    return cancel&&atomic_load(cancel);
}

static char *fts_literal(const char *text_value) {
    FmString s={
        0
    };
    fm_string_append(&s,"\"");
    for(const char *p=text_value;*p;p++) {
        if(*p=='"')fm_string_append(&s,"\"");
        fm_string_append_n(&s,p,1);
    }
    fm_string_append(&s,"\"");
    return fm_string_take(&s);
}

bool fm_db_rows(FmDb *db,const FmQuery *q,FmRows *out,char **error) {
    *out=(FmRows){
        0
    };
    sqlite3_stmt *s=NULL;
    FmString where={
        0
    };
    bool has_text=q->text&&*q->text,fts=has_text&&db->fts&&fm_utf8_length(q->text)>=3;
    const char *day=q->by_publication?"a.published_day":"a.fetched_day";
    fm_string_append(&where," FROM articles a JOIN feeds f ON f.url=a.feed_url WHERE 1");
    if(*q->day)fm_string_printf(&where," AND %s=?1",day);
    if(q->tag)fm_string_append(&where," AND f.tag=?2");
    if(q->unread_only)fm_string_append(&where," AND a.is_read=0");
    if(has_text)fm_string_append(&where,fts?" AND a.id IN(SELECT rowid FROM article_fts WHERE article_fts MATCH ?3)":" AND instr(lower(a.search_text),lower(?3))>0");
    sqlite3_progress_handler(db->sql,1000,cancelled,(void *)q->cancel);
    char *term=fts?fts_literal(q->text):fm_strdup(q->text);
    FmString sql={
        0
    };
    fm_string_append(&sql,"SELECT count(*)");
    fm_string_append(&sql,where.data);
    if(!prepare(db,sql.data,&s,error))goto fail;
    if(*q->day)text(s,1,q->day);
    if(q->tag)text(s,2,q->tag);
    if(has_text)text(s,3,term);
    if(sqlite3_step(s)!=SQLITE_ROW) {
        fm_error(error,"Query interrupted or failed: %s",sqlite3_errmsg(db->sql));
        goto fail;
    }
    out->total=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);
    s=NULL;
    free(sql.data);
    sql=(FmString){
        0
    };
    fm_string_append(&sql,"SELECT a.id,a.title,a.feed_url,f.tag,a.published,a.first_seen,a.is_read");
    fm_string_append(&sql,where.data);
    fm_string_printf(&sql," ORDER BY %s DESC,a.id DESC LIMIT ?4 OFFSET ?5",q->by_publication?"a.published":"a.first_seen");
    if(!prepare(db,sql.data,&s,error))goto fail;
    if(*q->day)text(s,1,q->day);
    if(q->tag)text(s,2,q->tag);
    if(has_text)text(s,3,term);
    int limit=q->limit>0?q->limit:FM_PAGE_SIZE;
    if(limit>10000)limit=10000;
    sqlite3_bind_int(s,4,limit);
    sqlite3_bind_int(s,5,q->offset>0?q->offset:0);
    out->items=fm_calloc((size_t)limit,sizeof *out->items);
    int rc;
    while((rc=sqlite3_step(s))==SQLITE_ROW) {
        FmRow *r=&out->items[out->len++];
        r->id=sqlite3_column_int64(s,0);
        r->title=column(s,1);
        r->source=column(s,2);
        r->tag=column(s,3);
        r->published=sqlite3_column_int64(s,4);
        r->first_seen=sqlite3_column_int64(s,5);
        r->read=sqlite3_column_int(s,6)!=0;
    }
    if(rc!=SQLITE_DONE){
        fm_error(error,"Query interrupted or failed: %s",sqlite3_errmsg(db->sql));
        goto fail;
    }
    sqlite3_finalize(s);
    sqlite3_progress_handler(db->sql,0,NULL,NULL);
    free(sql.data);
    free(where.data);
    free(term);
    return true;
    fail:
    sqlite3_finalize(s);
    sqlite3_progress_handler(db->sql,0,NULL,NULL);
    free(sql.data);
    free(where.data);
    free(term);
    fm_rows_free(out);
    return false;
}

void fm_rows_free(FmRows *rows) {
    for(size_t i=0;i<rows->len;i++){
        free(rows->items[i].title);
        free(rows->items[i].source);
        free(rows->items[i].tag);
    }
    free(rows->items);
    *rows=(FmRows){
        0
    };
}

bool fm_db_article(FmDb *db,int64_t id,FmArticle *out,char **error) {
    *out=(FmArticle){
        0
    };
    sqlite3_stmt *s=NULL;
    if(!prepare(db,"SELECT a.entry_key,a.title,a.url,a.html,a.feed_url,f.tag,a.published,a.first_seen,a.published_day,a.fetched_day,a.is_read,a.legacy FROM articles a JOIN feeds f ON a.feed_url=f.url WHERE a.id=?1",&s,error))return false;
    sqlite3_bind_int64(s,1,id);
    if(sqlite3_step(s)!=SQLITE_ROW) {
        fm_error(error,"Article no longer exists");
        sqlite3_finalize(s);
        return false;
    }
    out->id=id;
    out->key=column(s,0);
    out->title=column(s,1);
    out->url=column(s,2);
    out->html=column(s,3);
    out->source=column(s,4);
    out->tag=column(s,5);
    out->published=sqlite3_column_int64(s,6);
    out->first_seen=sqlite3_column_int64(s,7);
    snprintf(out->published_day,11,"%s",sqlite3_column_text(s,8));
    snprintf(out->fetched_day,11,"%s",sqlite3_column_text(s,9));
    out->read=sqlite3_column_int(s,10)!=0;
    out->legacy=sqlite3_column_int(s,11)!=0;
    sqlite3_finalize(s);
    return true;
}

bool fm_db_mark_read(FmDb *db,int64_t id,bool read,char **error) {
    sqlite3_stmt *s=NULL;
    if(!prepare(db,"UPDATE articles SET is_read=?2 WHERE id=?1 AND is_read<>?2",&s,error))return false;
    sqlite3_bind_int64(s,1,id);
    sqlite3_bind_int(s,2,read);
    bool ok=done(db,s,error);
    sqlite3_finalize(s);
    return ok;
}

bool fm_db_month(FmDb *db,int year,int month,bool published,unsigned *mask,char **error) {
    *mask=0;
    char lo[11],hi[11];
    if(year<1||year>9999||month<1||month>12){
        fm_error(error,"Invalid calendar month");
        return false;
    }
    snprintf(lo,sizeof lo,"%04d-%02d-01",year,month);
    if(month==12){
        year++;
        month=1;
    }
    else month++;
    if(year>9999)memcpy(hi,"9999-12-32",11);
    else snprintf(hi,sizeof hi,"%04d-%02d-01",year,month);
    FmString sql={
        0
    };
    const char *col=published?"published_day":"fetched_day";
    fm_string_printf(&sql,"SELECT DISTINCT %s FROM articles WHERE %s>=?1 AND %s<?2",col,col,col);
    sqlite3_stmt *s=NULL;
    bool ok=prepare(db,sql.data,&s,error);
    free(sql.data);
    if(!ok)return false;
    text(s,1,lo);
    text(s,2,hi);
    int rc;
    while((rc=sqlite3_step(s))==SQLITE_ROW){
        const char *d=(const char *)sqlite3_column_text(s,0);
        int n=atoi(d+8);
        if(n>=1&&n<=31)*mask|=1u<<(n-1);
    }
    if(rc!=SQLITE_DONE)fm_error(error,"Calendar query failed: %s",sqlite3_errmsg(db->sql));
    sqlite3_finalize(s);
    return rc==SQLITE_DONE;
}

bool fm_db_neighbor(FmDb *db,const char *day,bool published,bool next,char out[11],char **error) {
    out[0]=0;
    const char *col=published?"published_day":"fetched_day";
    FmString sql={
        0
    };
    fm_string_printf(&sql,"SELECT %s FROM articles WHERE %s %s ?1 ORDER BY %s %s LIMIT 1",col,col,next?">":"<",col,next?"ASC":"DESC");
    sqlite3_stmt *s=NULL;
    bool ok=prepare(db,sql.data,&s,error);
    free(sql.data);
    if(!ok)return false;
    text(s,1,day);
    int rc=sqlite3_step(s);
    if(rc==SQLITE_ROW)snprintf(out,11,"%s",sqlite3_column_text(s,0));
    else if(rc!=SQLITE_DONE)fm_error(error,"Calendar query failed: %s",sqlite3_errmsg(db->sql));
    sqlite3_finalize(s);
    return rc==SQLITE_ROW||rc==SQLITE_DONE;
}

int64_t fm_db_count(FmDb *db) {
    sqlite3_stmt *s=NULL;
    int64_t n=0;
    if(prepare(db,"SELECT count(*) FROM articles",&s,NULL)&&sqlite3_step(s)==SQLITE_ROW)n=sqlite3_column_int64(s,0);
    sqlite3_finalize(s);
    return n;
}
