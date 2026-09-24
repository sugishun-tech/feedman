/* SPDX-License-Identifier: MIT */
#ifndef FEEDMAN_H
#define FEEDMAN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include <sqlite3.h>
#define FM_VERSION "1.0.0"
#define FM_MAX_FEED_BYTES (16u * 1024u * 1024u)
#define FM_MAX_ARTICLES 50000u
#define FM_PAGE_SIZE 250
#define FM_MAX_THREADS 64
/* Heap-owned strings/arrays are released by the corresponding *_free function.
 * SQLite handles are confined to the thread which opens them. */
typedef struct {
    char *data;
    size_t len, cap;
} FmString;
typedef struct {
    char *url, *tag, *etag, *modified;
} FmFeed;
typedef struct {
    FmFeed *items;
    size_t len, cap;
} FmFeeds;
typedef struct {
    int64_t id;
    char *key, *title, *url, *html, *plain, *source, *tag;
    int64_t published, first_seen;
    char published_day[11], fetched_day[11];
    bool read, legacy;
} FmArticle;
typedef struct {
    FmArticle *items;
    size_t len, cap;
    char *title;
} FmArticles;
typedef struct {
    sqlite3 *sql;
    bool fts;
} FmDb;
typedef struct {
    char day[11];
    /* Empty means every day. */
    bool by_publication, unread_only;
    const char *text, *tag;
    /* tag == NULL: all; tag == "": untagged. */
    int limit, offset;
    const atomic_bool *cancel;
} FmQuery;
typedef struct {
    int64_t id, published, first_seen;
    char *title, *source, *tag;
    bool read;
} FmRow;
typedef struct {
    FmRow *items;
    size_t len;
    int64_t total;
} FmRows;
typedef struct {
    size_t total, done, downloaded, unchanged, failed, inserted, updated;
    uint64_t bytes;
    double seconds;
    bool cancelled;
    char *errors;
} FmStats;
typedef struct {
    int threads, timeout;
    size_t max_bytes;
    atomic_bool cancel;
    volatile sig_atomic_t *signal_cancel;
} FmFetchOptions;
typedef void (*FmProgress)(size_t done, size_t total, const char *source, void *user);
typedef struct {
    char *feeds_path, *data_dir, *db_path;
    int threads, timeout;
    char date[11];
    bool by_publication, threads_set;
} FmAppOptions;
void *fm_malloc(size_t n);
void *fm_calloc(size_t n, size_t size);
char *fm_strdup(const char *s);
char *fm_strndup(const char *s, size_t n);
void fm_error(char **error, const char *fmt, ...);
void fm_string_append(FmString *s, const char *text);
void fm_string_append_n(FmString *s, const char *text, size_t n);
void fm_string_printf(FmString *s, const char *fmt, ...);
char *fm_string_take(FmString *s);
char *fm_trim(char *s);
bool fm_date_normalize(const char *s, char out[11]);
bool fm_parse_time(const char *s, int64_t *out);
void fm_local_day(int64_t timestamp, char out[11]);
char *fm_format_time(int64_t timestamp);
double fm_clock(void);
bool fm_url_valid(const char *url);
char *fm_url_resolve(const char *url, const char *base);
char *fm_url_host(const char *url);
bool fm_mkdirs(const char *path, char **error);
bool fm_read_file(const char *path, size_t limit, char **out, size_t *len, char **error);
bool fm_write_file_atomic(const char *path, const char *text, char **error);
char *fm_path_join(const char *a, const char *b);
size_t fm_utf8_length(const char *s);
void fm_global_init(void);
void fm_global_cleanup(void);
bool fm_feeds_parse(const char *text, FmFeeds *out, char **error);
bool fm_feeds_load(const char *path, FmFeeds *out, char **error);
void fm_feeds_free(FmFeeds *feeds);
const char *fm_example_feeds(void);
bool fm_parse_feed(const char *xml, size_t length, const char *base,
FmArticles *out, char **error);
char *fm_html_plain(const char *html);
void fm_article_free(FmArticle *article);
void fm_articles_free(FmArticles *articles);
bool fm_db_open(FmDb *db, const char *path, bool create, char **error);
void fm_db_close(FmDb *db);
bool fm_db_sync_feeds(FmDb *db, FmFeeds *feeds, char **error);
bool fm_db_cache_load(FmDb *db, FmFeeds *feeds, char **error);
bool fm_db_store(FmDb *db, const FmFeed *feed, const FmArticles *articles,
const char *etag, const char *modified, int64_t fetched_at,
size_t *inserted, size_t *updated, char **error);
bool fm_db_not_modified(FmDb *db, const FmFeed *feed, const char *etag,
const char *modified, int64_t now, char **error);
bool fm_db_rows(FmDb *db, const FmQuery *query, FmRows *out, char **error);
void fm_rows_free(FmRows *rows);
bool fm_db_article(FmDb *db, int64_t id, FmArticle *out, char **error);
bool fm_db_mark_read(FmDb *db, int64_t id, bool read, char **error);
bool fm_db_month(FmDb *db, int year, int month, bool published,
unsigned *mask, char **error);
bool fm_db_neighbor(FmDb *db, const char *day, bool published,
bool next, char out[11], char **error);
int64_t fm_db_count(FmDb *db);
bool fm_fetch_all(const char *db_path, FmFeeds *feeds, FmFetchOptions *options,
FmProgress progress, void *user, FmStats *stats, char **error);
void fm_stats_free(FmStats *stats);
bool fm_import_myrss(const char *dir, const char *db_path,
FmStats *stats, char **error);
#ifdef FEEDMAN_WITH_GUI
int fm_ui_run(FmAppOptions *options);
#endif
#endif
