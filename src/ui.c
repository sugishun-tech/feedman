/* SPDX-License-Identifier: MIT */
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <gdk/gdkkeysyms.h>
#include "feedman.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
typedef struct Ui Ui;
typedef enum {
    JOB_LIST,JOB_ARTICLE,JOB_PREV,JOB_NEXT,JOB_READ
} JobKind;
typedef struct {
    Ui *ui;
    JobKind kind;
    atomic_bool cancel;
    FmQuery query;
    char *text,*tag;
    int year,month;
    int64_t id;
    bool sync;
    FmRows rows;
    FmArticle article;
    unsigned marks;
    char neighbor[11];
    char *error;
} Job;
typedef struct {
    Ui *ui;
    FmFetchOptions options;
    FmStats stats;
    FmFeeds feeds;
    char *error;
    GMutex mutex;
    size_t done,total;
    char *current;
} Fetch;
struct Ui {
    FmAppOptions *options;
    GtkWidget *window,*entry,*calendar,*basis,*all_days,*unread,*tag_filter;
    GtkWidget *search_split,*main_split,*article_split,*tree,*list_stack,*list_title,*count;
    GtkWidget *previous,*next,*reader,*open_button,*fetch_button,*cancel_button,*status,*progress,*details;
    GtkWidget *thread_spin,*theme_combo,*layout_combo;
    GtkListStore *store;
    char day[11];
    char *query_text,*selected_tag,*selected_url,*last_errors,*state_path;
    FmArticle current_article;
    int page,vertical_position,horizontal_position,theme,restore_position;
    double font_points;
    bool changing,closing,needs_sync,wide,system_dark,open_when_ready,restore_split;
    unsigned pending;
    guint debounce,fetch_tick;
    Job *list_job,*article_job;
    Fetch *fetch;
    GKeyFile *state;
    GMutex config_mutex;
};
enum {
    COL_ID,COL_TITLE,COL_DATE,COL_SOURCE,COL_WEIGHT,COL_TOOLTIP,N_COLS
};
static void load_list(Ui *u);
static void select_day(Ui *u,const char *day);
static void start_fetch(GtkWidget *widget,gpointer user);
static void cancel_fetch(GtkWidget *widget,gpointer user);
static void show_details(GtkWidget *widget,gpointer user);
static void refresh_filters(Ui *u);
static void open_article(GtkWidget *widget,gpointer user);
static void finish_pending(Ui *u) {
    if(u->pending)u->pending--;
    if(u->closing&&!u->pending)gtk_main_quit();
}

static void status(Ui *u,const char *text) {
    if(!u->closing)gtk_label_set_text(GTK_LABEL(u->status),text?text:"");
}

static GtkWidget *label(const char *text,const char *css) {
    GtkWidget *w=gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(w),0.0f);
    if(css)gtk_style_context_add_class(gtk_widget_get_style_context(w),css);
    return w;
}

static void margin(GtkWidget *w,int value) {
    gtk_widget_set_margin_start(w,value);
    gtk_widget_set_margin_end(w,value);
    gtk_widget_set_margin_top(w,value);
    gtk_widget_set_margin_bottom(w,value);
}

static GtkWidget *button(const char *text,const char *tip,GCallback callback,Ui *u) {
    GtkWidget *w=gtk_button_new_with_label(text);
    if(tip)gtk_widget_set_tooltip_text(w,tip);
    if(callback)g_signal_connect(w,"clicked",callback,u);
    return w;
}

static GtkWidget *scroller(GtkWidget *child) {
    GtkWidget *w=gtk_scrolled_window_new(NULL,NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w),GTK_POLICY_AUTOMATIC,GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(w),child);
    return w;
}

static GtkWidget *panel(void) {
    GtkWidget *p=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
    gtk_style_context_add_class(gtk_widget_get_style_context(p),"panel");
    margin(p,4);
    return p;
}

static void error_dialog(Ui *u,const char *title,const char *message) {
    if(u->closing)return;
    GtkWidget *d=gtk_message_dialog_new(GTK_WINDOW(u->window),GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"%s",title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),"%s",message?message:"");
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void clear_reader(Ui *u,const char *message) {
    GtkTextBuffer *b=gtk_text_buffer_new(NULL);
    gtk_text_buffer_set_text(b,message,-1);
    gtk_text_view_set_buffer(GTK_TEXT_VIEW(u->reader),b);
    g_object_unref(b);
    gtk_widget_set_sensitive(u->open_button,FALSE);
    free(u->selected_url);
    u->selected_url=NULL;
    fm_article_free(&u->current_article);
}

static void job_free(gpointer data) {
    Job *j=data;
    free(j->text);
    free(j->tag);
    free(j->error);
    fm_rows_free(&j->rows);
    fm_article_free(&j->article);
    free(j);
}

static void job_worker(GTask *task,gpointer source,gpointer data,GCancellable *cancel) {
    (void)source;
    (void)cancel;
    Job *j=data;
    FmDb db={
        0
    };
    bool ok=fm_db_open(&db,j->ui->options->db_path,false,&j->error);
    /* Serialize config side effects; canceled older searches must not restore old tags. */
    if(ok&&j->sync) {
        g_mutex_lock(&j->ui->config_mutex);
        if(!atomic_load(&j->cancel)) {
            FmFeeds feeds={0};
            ok=fm_feeds_load(j->ui->options->feeds_path,&feeds,&j->error);
            if(ok)ok=fm_db_sync_feeds(&db,&feeds,&j->error);
            fm_feeds_free(&feeds);
        }
        g_mutex_unlock(&j->ui->config_mutex);
    }
    if(ok&&!atomic_load(&j->cancel)) {
        switch(j->kind) {
            case JOB_LIST:
            ok=fm_db_rows(&db,&j->query,&j->rows,&j->error);
            if(ok&&!atomic_load(&j->cancel))ok=fm_db_month(&db,j->year,j->month,j->query.by_publication,&j->marks,&j->error);
            break;
            case JOB_ARTICLE:ok=fm_db_article(&db,j->id,&j->article,&j->error);
            break;
            case JOB_PREV:case JOB_NEXT:ok=fm_db_neighbor(&db,j->query.day,j->query.by_publication,j->kind==JOB_NEXT,j->neighbor,&j->error);
            break;
            case JOB_READ:ok=fm_db_mark_read(&db,j->id,true,&j->error);
            break;
        }
    }
    fm_db_close(&db);
    g_task_return_boolean(task,ok);
}

static void launch_job(Ui *u,Job *j);
static void apply_rows(Ui *u,Job *j) {
    int64_t old_id=u->current_article.id;
    int selected=-1;
    u->changing=true;
    gtk_list_store_clear(u->store);
    for(size_t i=0;i<j->rows.len;i++) {
        FmRow *r=&j->rows.items[i];
        FmString title={
            0
        };
        if(r->tag&&*r->tag)fm_string_printf(&title,"[%s] ",r->tag);
        fm_string_append(&title,r->title);
        char *host=fm_url_host(r->source),*date=fm_format_time(j->query.by_publication?r->published:r->first_seen);
        GtkTreeIter iter;
        gtk_list_store_insert_with_values(u->store,&iter,-1,COL_ID,(gint64)r->id,COL_TITLE,title.data,
        COL_DATE,date,COL_SOURCE,host,COL_WEIGHT,r->read?PANGO_WEIGHT_NORMAL:PANGO_WEIGHT_SEMIBOLD,COL_TOOLTIP,title.data,-1);
        if(r->id==old_id)selected=(int)i;
        free(title.data);
        free(host);
        free(date);
    }
    u->changing=false;
    for(guint d=1;d<=31;d++) {
        if(j->marks&(1u<<(d-1)))gtk_calendar_mark_day(GTK_CALENDAR(u->calendar),d);
        else gtk_calendar_unmark_day(GTK_CALENDAR(u->calendar),d);
    }
    FmString title={
        0
    };
    fm_string_printf(&title,"記事一覧  ·  %s%s",*j->query.day?j->query.day:"全期間",j->query.by_publication?" / 公開日":" / 取得日");
    gtk_label_set_text(GTK_LABEL(u->list_title),title.data);
    free(title.data);
    FmString count={
        0
    };
    if(j->rows.len)fm_string_printf(&count,"%d–%d / %lld 件",u->page*FM_PAGE_SIZE+1,u->page*FM_PAGE_SIZE+(int)j->rows.len,(long long)j->rows.total);
    else fm_string_append(&count,"0 件");
    gtk_label_set_text(GTK_LABEL(u->count),count.data);
    free(count.data);
    gtk_widget_set_sensitive(u->previous,u->page>0);
    gtk_widget_set_sensitive(u->next,(int64_t)(u->page+1)*FM_PAGE_SIZE<j->rows.total);
    gtk_stack_set_visible_child_name(GTK_STACK(u->list_stack),j->rows.len?"list":"empty");
    if(j->rows.len) {
        GtkTreePath *path=gtk_tree_path_new_from_indices(selected>=0?selected:0,-1);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(u->tree),path,NULL,FALSE);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(u->tree),path,NULL,FALSE,0,0);
        gtk_tree_path_free(path);
    }
    else clear_reader(u,"この条件の記事はありません。\n\nカレンダーの太字の日を選ぶか、全期間で検索してください。");
}

static void job_done(GObject *source,GAsyncResult *result,gpointer user) {
    (void)source;
    Ui *u=user;
    Job *j=g_task_get_task_data(G_TASK(result));
    bool ok=g_task_propagate_boolean(G_TASK(result),NULL);
    bool current=true;
    if(j->kind==JOB_LIST||j->kind==JOB_PREV||j->kind==JOB_NEXT) {
        current=u->list_job==j;
        if(current)u->list_job=NULL;
    }
    else if(j->kind==JOB_ARTICLE){
        current=u->article_job==j;
        if(current)u->article_job=NULL;
    }
    if(j->sync&&ok&&current&&!atomic_load(&j->cancel))u->needs_sync=false;
    if(!u->closing&&current&&!atomic_load(&j->cancel)) {
        if(!ok) {
            status(u,j->error?j->error:"データを読み込めませんでした");
        }
        else if(j->kind==JOB_LIST)apply_rows(u,j);
        else if(j->kind==JOB_ARTICLE) {
            fm_article_free(&u->current_article);
            u->current_article=j->article;
            j->article=(FmArticle){
                0
            };
            free(u->selected_url);
            u->selected_url=fm_strdup(u->current_article.url);
            gtk_widget_set_sensitive(u->open_button,fm_url_valid(u->selected_url));
            fm_reader_show(GTK_TEXT_VIEW(u->reader),&u->current_article,u->font_points);
            if(u->open_when_ready){
                u->open_when_ready=false;
                open_article(NULL,u);
            }
            GtkTreeModel *model=NULL;
            GtkTreeIter iter;
            if(gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(u->tree)),&model,&iter))gtk_list_store_set(u->store,&iter,COL_WEIGHT,PANGO_WEIGHT_NORMAL,-1);
            Job *mark=fm_calloc(1,sizeof *mark);
            mark->kind=JOB_READ;
            mark->id=u->current_article.id;
            launch_job(u,mark);
        }
        else if(j->kind==JOB_PREV||j->kind==JOB_NEXT) {
            if(*j->neighbor)select_day(u,j->neighbor);
            else status(u,"その方向に記事のある日はありません。");
        }
    }
    finish_pending(u);
}

static void launch_job(Ui *u,Job *j) {
    j->ui=u;
    atomic_init(&j->cancel,false);
    j->query.cancel=&j->cancel;
    GTask *task=g_task_new(NULL,NULL,job_done,u);
    g_task_set_task_data(task,j,job_free);
    u->pending++;
    g_task_run_in_thread(task,job_worker);
    g_object_unref(task);
}

static void cancel_selection_jobs(Ui *u) {
    if(u->list_job)atomic_store(&u->list_job->cancel,true);
    if(u->article_job)atomic_store(&u->article_job->cancel,true);
}

static void load_list(Ui *u) {
    if(u->closing||u->changing)return;
    cancel_selection_jobs(u);
    Job *j=fm_calloc(1,sizeof *j);
    j->kind=JOB_LIST;
    j->sync=u->needs_sync;
    j->text=fm_strdup(u->query_text);
    j->tag=u->selected_tag?fm_strdup(u->selected_tag):NULL;
    j->query=(FmQuery){
        .by_publication=gtk_combo_box_get_active(GTK_COMBO_BOX(u->basis))==1,
        .unread_only=gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->unread)),.text=j->text,.tag=j->tag,.limit=FM_PAGE_SIZE,.offset=u->page*FM_PAGE_SIZE
    };
    if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(u->all_days)))memcpy(j->query.day,u->day,11);
    guint year,month,day;
    gtk_calendar_get_date(GTK_CALENDAR(u->calendar),&year,&month,&day);
    j->year=(int)year;
    j->month=(int)month+1;
    u->list_job=j;
    launch_job(u,j);
}

static void selection_changed(GtkTreeSelection *selection,gpointer user) {
    Ui *u=user;
    if(u->changing||u->closing)return;
    GtkTreeModel *model=NULL;
    GtkTreeIter iter;
    if(!gtk_tree_selection_get_selected(selection,&model,&iter))return;
    gint64 id=0;
    gtk_tree_model_get(model,&iter,COL_ID,&id,-1);
    u->open_when_ready=false;
    gtk_widget_set_sensitive(u->open_button,FALSE);
    free(u->selected_url);
    u->selected_url=NULL;
    if(u->article_job)atomic_store(&u->article_job->cancel,true);
    Job *j=fm_calloc(1,sizeof *j);
    j->kind=JOB_ARTICLE;
    j->id=id;
    u->article_job=j;
    launch_job(u,j);
}

static void select_day(Ui *u,const char *day) {
    int year,month,d;
    if(sscanf(day,"%d-%d-%d",&year,&month,&d)!=3)return;
    u->changing=true;
    char entered[11];
    if(fm_date_normalize(gtk_entry_get_text(GTK_ENTRY(u->entry)),entered)&&strcmp(entered,day))gtk_entry_set_text(GTK_ENTRY(u->entry),day);
    memcpy(u->day,day,11);
    gtk_calendar_select_month(GTK_CALENDAR(u->calendar),(guint)month-1,(guint)year);
    gtk_calendar_select_day(GTK_CALENDAR(u->calendar),(guint)d);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u->all_days),FALSE);
    u->changing=false;
    u->page=0;
    load_list(u);
}

static gboolean apply_search(gpointer user) {
    Ui *u=user;
    u->debounce=0;
    if(u->closing)return G_SOURCE_REMOVE;
    const char *text=gtk_entry_get_text(GTK_ENTRY(u->entry));
    char day[11];
    size_t n=strlen(text);
    bool date_shape=n==8;
    for(size_t i=0;date_shape&&i<n;i++)if(text[i]<'0'||text[i]>'9')date_shape=false;
    date_shape|=n==10&&text[4]=='-'&&text[7]=='-';
    GtkStyleContext *style=gtk_widget_get_style_context(u->entry);
    if(date_shape) {
        if(!fm_date_normalize(text,day)){
            gtk_style_context_add_class(style,"error");
            status(u,"存在しない日付です。例: 20260912 または 2026-09-12");
            return G_SOURCE_REMOVE;
        }
        gtk_style_context_remove_class(style,"error");
        free(u->query_text);
        u->query_text=fm_strdup("");
        select_day(u,day);
        status(u,"指定日へ移動しました。日付は取得日 / 公開日の選択に従います。");
    }
    else {
        gtk_style_context_remove_class(style,"error");
        free(u->query_text);
        u->query_text=fm_strdup(text);
        u->page=0;
        load_list(u);
        if(!u->fetch)status(u,*text?"本文・タイトルを検索中。対象日はカレンダーまたは「全期間」で選べます。":"待機中 · 起動や閲覧だけでは通信しません");
    }
    return G_SOURCE_REMOVE;
}

static void search_changed(GtkEditable *entry,gpointer user) {
    (void)entry;
    Ui *u=user;
    if(u->changing||u->closing)return;
    if(u->debounce)g_source_remove(u->debounce);
    u->debounce=g_timeout_add(180,apply_search,u);
}

static void search_now(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    if(u->debounce){
        g_source_remove(u->debounce);
        u->debounce=0;
    }
    apply_search(u);
}

static void day_changed(GtkCalendar *calendar,gpointer user) {
    Ui *u=user;
    if(u->changing)return;
    guint y,m,d;
    gtk_calendar_get_date(calendar,&y,&m,&d);
    if(y<1||y>9999||m>11||!d||d>31)return;
    snprintf(u->day,sizeof u->day,"%04u-%02u-%02u",y,m+1,d);
    u->changing=true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(u->all_days),FALSE);
    char normalized[11];
    if(fm_date_normalize(gtk_entry_get_text(GTK_ENTRY(u->entry)),normalized))gtk_entry_set_text(GTK_ENTRY(u->entry),u->day);
    u->changing=false;
    u->page=0;
    load_list(u);
}

static void month_changed(GtkCalendar *calendar,gpointer user) {
    Ui *u=user;
    if(u->changing)return;
    guint y,m,d;
    gtk_calendar_get_date(calendar,&y,&m,&d);
    if(y<1||y>9999||m>11||d>31)return;
    if(!d)d=1;
    char date[11];
    snprintf(date,sizeof date,"%04u-%02u-%02u",y,m+1,d);
    if(!fm_date_normalize(date,date))snprintf(date,sizeof date,"%04u-%02u-01",y,m+1);
    select_day(u,date);
}

static void filters_changed(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    if(u->changing)return;
    free(u->selected_tag);
    u->selected_tag=NULL;
    int active=gtk_combo_box_get_active(GTK_COMBO_BOX(u->tag_filter));
    if(active==1)u->selected_tag=fm_strdup("");
    else if(active>1) {
        gchar *t=gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(u->tag_filter));
        u->selected_tag=fm_strdup(t);
        g_free(t);
    }
    u->page=0;
    load_list(u);
}

static void today(GtkWidget *widget,gpointer user) {
    (void)widget;
    char day[11];
    fm_local_day((int64_t)time(NULL),day);
    select_day(user,day);
}

static void neighbor(Ui *u,bool next) {
    if(u->closing)return;
    if(u->list_job)atomic_store(&u->list_job->cancel,true);
    Job *j=fm_calloc(1,sizeof *j);
    j->kind=next?JOB_NEXT:JOB_PREV;
    j->query.by_publication=gtk_combo_box_get_active(GTK_COMBO_BOX(u->basis))==1;
    memcpy(j->query.day,u->day,11);
    u->list_job=j;
    launch_job(u,j);
}

static void previous_day(GtkWidget *w,gpointer user){
    (void)w;
    neighbor(user,false);
}

static void next_day(GtkWidget *w,gpointer user){
    (void)w;
    neighbor(user,true);
}

static void previous_page(GtkWidget *w,gpointer user){
    (void)w;
    Ui *u=user;
    if(u->page>0){
        u->page--;
        load_list(u);
    }
}

static void next_page(GtkWidget *w,gpointer user){
    (void)w;
    Ui *u=user;
    u->page++;
    load_list(u);
}

static void open_article(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    if(!fm_url_valid(u->selected_url))return;
    GError *error=NULL;
    if(!gtk_show_uri_on_window(GTK_WINDOW(u->window),u->selected_url,GDK_CURRENT_TIME,&error)) {
        error_dialog(u,"ブラウザーを開けませんでした",error?error->message:"");
        g_clear_error(&error);
    }
}

static void row_activated(GtkTreeView *tree,GtkTreePath *path,GtkTreeViewColumn *column,gpointer user) {
    (void)tree;
    (void)path;
    (void)column;
    Ui *u=user;
    if(u->article_job)u->open_when_ready=true;
    else open_article(NULL,u);
}

static void zoom(Ui *u,double delta) {
    u->font_points+=delta;
    if(u->font_points<9)u->font_points=9;
    if(u->font_points>28)u->font_points=28;
    if(u->current_article.id)fm_reader_show(GTK_TEXT_VIEW(u->reader),&u->current_article,u->font_points);
}

static void zoom_in(GtkWidget *w,gpointer user){
    (void)w;
    zoom(user,1);
}

static void zoom_out(GtkWidget *w,gpointer user){
    (void)w;
    zoom(user,-1);
}

static void refresh_filters(Ui *u) {
    FmFeeds feeds={
        0
    };
    char *error=NULL;
    if(!fm_feeds_load(u->options->feeds_path,&feeds,&error)) {
        status(u,error);
        free(error);
        return;
    }
    bool was_changing=u->changing;
    u->changing=true;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(u->tag_filter));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->tag_filter),"すべてのタグ");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->tag_filter),"タグなし");
    int active=u->selected_tag?(!*u->selected_tag?1:0):0,index=2;
    GHashTable *seen=g_hash_table_new(g_str_hash,g_str_equal);
    for(size_t i=0;i<feeds.len;i++) {
        const char *tag=feeds.items[i].tag;
        if(!*tag||g_hash_table_contains(seen,tag))continue;
        g_hash_table_add(seen,(gpointer)tag);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->tag_filter),tag);
        if(u->selected_tag&&!strcmp(u->selected_tag,tag))active=index;
        index++;
    }
    /* Imported archives have a distinct tag because old JSON has no feed URL. */
    if(!g_hash_table_contains(seen,"import")) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->tag_filter),"import");
        if(u->selected_tag&&!strcmp(u->selected_tag,"import"))active=index;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(u->tag_filter),active);
    if(active==0){
        free(u->selected_tag);
        u->selected_tag=NULL;
    }
    g_hash_table_destroy(seen);
    fm_feeds_free(&feeds);
    u->changing=was_changing;
}

static void edit_feeds(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    char *text=NULL,*error=NULL;
    if(!fm_read_file(u->options->feeds_path,4u*1024u*1024u,&text,NULL,&error)) {
        error_dialog(u,"feeds.txt を開けませんでした",error);
        free(error);
        return;
    }
    GtkWidget *dialog=gtk_dialog_new_with_buttons("フィードの設定",GTK_WINDOW(u->window),GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"キャンセル",GTK_RESPONSE_CANCEL,"検証して保存",GTK_RESPONSE_ACCEPT,NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog),760,480);
    GtkWidget *box=gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    margin(box,16);
    GtkWidget *hint=label("1行につき「タグ URL」。タグなしはURLだけ。# で始まる行はコメントです。",NULL);
    gtk_label_set_line_wrap(GTK_LABEL(hint),TRUE);
    gtk_box_pack_start(GTK_BOX(box),hint,FALSE,FALSE,6);
    GtkWidget *view=gtk_text_view_new();
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view),TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),GTK_WRAP_NONE);
    GtkTextBuffer *buffer=gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buffer,text,-1);
    free(text);
    GtkWidget *scroll=scroller(view);
    gtk_box_pack_start(GTK_BOX(box),scroll,TRUE,TRUE,8);
    GtkWidget *path=label(u->options->feeds_path,"dim-label");
    gtk_label_set_selectable(GTK_LABEL(path),TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(path),PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(box),path,FALSE,FALSE,0);
    gtk_widget_show_all(dialog);
    while(gtk_dialog_run(GTK_DIALOG(dialog))==GTK_RESPONSE_ACCEPT) {
        GtkTextIter start,end;
        gtk_text_buffer_get_bounds(buffer,&start,&end);
        gchar *new_text=gtk_text_buffer_get_text(buffer,&start,&end,FALSE);
        FmFeeds feeds={
            0
        };
        error=NULL;
        bool ok=fm_feeds_parse(new_text,&feeds,&error);
        fm_feeds_free(&feeds);
        if(ok)ok=fm_write_file_atomic(u->options->feeds_path,new_text,&error);
        g_free(new_text);
        if(ok) {
            u->needs_sync=true;
            refresh_filters(u);
            load_list(u);
            status(u,"設定を保存しました。記事の取得は「取得」を押したときだけ実行します。");
            break;
        }
        error_dialog(u,"保存できませんでした",error);
        free(error);
    }
    gtk_widget_destroy(dialog);
}

static void reload_feeds(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    u->needs_sync=true;
    refresh_filters(u);
    load_list(u);
    status(u,"feeds.txt を再読込しました。ネットワーク取得はしていません。");
}

static void fetch_progress(size_t done,size_t total,const char *source,void *user) {
    Fetch *f=user;
    g_mutex_lock(&f->mutex);
    f->done=done;
    f->total=total;
    free(f->current);
    f->current=fm_strdup(source);
    g_mutex_unlock(&f->mutex);
}

static void fetch_free(gpointer data) {
    Fetch *f=data;
    fm_feeds_free(&f->feeds);
    fm_stats_free(&f->stats);
    free(f->error);
    free(f->current);
    g_mutex_clear(&f->mutex);
    free(f);
}

static void fetch_worker(GTask *task,gpointer source,gpointer data,GCancellable *cancellable) {
    (void)source;
    (void)cancellable;
    Fetch *f=data;
    bool ok=fm_feeds_load(f->ui->options->feeds_path,&f->feeds,&f->error);
    if(ok) {
        g_mutex_lock(&f->mutex);
        f->total=f->feeds.len;
        g_mutex_unlock(&f->mutex);
        ok=fm_fetch_all(f->ui->options->db_path,&f->feeds,&f->options,fetch_progress,f,&f->stats,&f->error);
    }
    g_task_return_boolean(task,ok);
}

static gboolean poll_fetch(gpointer user) {
    Ui *u=user;
    if(u->closing||!u->fetch){
        u->fetch_tick=0;
        return G_SOURCE_REMOVE;
    }
    Fetch *f=u->fetch;
    g_mutex_lock(&f->mutex);
    size_t done=f->done,total=f->total;
    char *host=fm_strdup(f->current);
    g_mutex_unlock(&f->mutex);
    FmString message={
        0
    };
    fm_string_printf(&message,"取得中 %zu / %zu  ·  %s",done,total,host);
    status(u,message.data);
    if(total)gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(u->progress),(double)done/(double)total);
    else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(u->progress));
    free(message.data);
    free(host);
    return G_SOURCE_CONTINUE;
}

static void fetch_done(GObject *source,GAsyncResult *result,gpointer user) {
    (void)source;
    Ui *u=user;
    Fetch *f=g_task_get_task_data(G_TASK(result));
    bool ok=g_task_propagate_boolean(G_TASK(result),NULL);
    if(u->fetch_tick){
        g_source_remove(u->fetch_tick);
        u->fetch_tick=0;
    }
    u->fetch=NULL;
    if(!u->closing) {
        gtk_widget_set_sensitive(u->fetch_button,TRUE);
        gtk_widget_hide(u->cancel_button);
        gtk_widget_hide(u->progress);
        FmString message={
            0
        };
        if(!ok)fm_string_printf(&message,"取得エラー: %s",f->error?f->error:"不明なエラー");
        else if(!f->stats.total)fm_string_append(&message,"フィードがありません。「フィード設定」でURLを追加してください。");
        else fm_string_printf(&message,"%s · 新規 %zu / 更新 %zu / 変更なし %zu / 失敗 %zu · %.2f 秒",f->stats.cancelled?"停止しました":"取得完了",f->stats.inserted,f->stats.updated,f->stats.unchanged,f->stats.failed,f->stats.seconds);
        status(u,message.data);
        free(message.data);
        free(u->last_errors);
        FmString errors={
            0
        };
        if(f->error)fm_string_printf(&errors,"%s\n",f->error);
        fm_string_append(&errors,f->stats.errors);
        u->last_errors=fm_string_take(&errors);
        gtk_widget_set_visible(u->details,*u->last_errors!=0);
        u->needs_sync=true;
        refresh_filters(u);
        load_list(u);
    }
    finish_pending(u);
}

static void start_fetch(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    if(u->fetch||u->closing)return;
    Fetch *f=fm_calloc(1,sizeof *f);
    f->ui=u;
    f->options=(FmFetchOptions){
        .threads=gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(u->thread_spin)),.timeout=u->options->timeout,.max_bytes=FM_MAX_FEED_BYTES
    };
    atomic_init(&f->options.cancel,false);
    g_mutex_init(&f->mutex);
    u->fetch=f;
    gtk_widget_set_sensitive(u->fetch_button,FALSE);
    gtk_widget_show(u->cancel_button);
    gtk_widget_show(u->progress);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(u->progress),0);
    status(u,"取得を開始しています…");
    GTask *task=g_task_new(NULL,NULL,fetch_done,u);
    g_task_set_task_data(task,f,fetch_free);
    u->pending++;
    g_task_run_in_thread(task,fetch_worker);
    g_object_unref(task);
    u->fetch_tick=g_timeout_add(120,poll_fetch,u);
}

static void cancel_fetch(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    if(u->fetch){
        atomic_store(&u->fetch->options.cancel,true);
        status(u,"取得を停止しています。保存済みの記事は残ります。");
    }
}

static void show_details(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    GtkWidget *d=gtk_dialog_new_with_buttons("取得結果の詳細",GTK_WINDOW(u->window),GTK_DIALOG_MODAL|GTK_DIALOG_DESTROY_WITH_PARENT,"閉じる",GTK_RESPONSE_CLOSE,NULL);
    gtk_window_set_default_size(GTK_WINDOW(d),720,400);
    GtkWidget *view=gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view),GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),u->last_errors?u->last_errors:"エラーはありません。",-1);
    GtkWidget *scroll=scroller(view);
    margin(scroll,16);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))),scroll,TRUE,TRUE,0);
    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static int state_int(Ui *u,const char *name,int fallback,int min,int max) {
    GError *error=NULL;
    int value=g_key_file_get_integer(u->state,"window",name,&error);
    if(error){
        g_clear_error(&error);
        return fallback;
    }
    return value>=min&&value<=max?value:fallback;
}

static void theme_changed(GtkComboBox *combo,gpointer user) {
    Ui *u=user;
    u->theme=gtk_combo_box_get_active(combo);
    gboolean dark=u->theme==2||(u->theme==0&&u->system_dark);
    g_object_set(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",dark,NULL);
}

static void article_allocated(GtkWidget *widget,gpointer allocation,gpointer user) {
    (void)widget;
    (void)allocation;
    Ui *u=user;
    if(u->restore_split){
        u->restore_split=false;
        gtk_paned_set_position(GTK_PANED(u->article_split),u->restore_position);
    }
}

static void layout_changed(GtkComboBox *combo,gpointer user) {
    Ui *u=user;
    if(u->changing)return;
    if(u->wide)u->horizontal_position=gtk_paned_get_position(GTK_PANED(u->article_split));
    else u->vertical_position=gtk_paned_get_position(GTK_PANED(u->article_split));
    u->wide=gtk_combo_box_get_active(combo)==1;
    u->restore_position=u->wide?u->horizontal_position:u->vertical_position;
    u->restore_split=true;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(u->article_split),u->wide?GTK_ORIENTATION_HORIZONTAL:GTK_ORIENTATION_VERTICAL);
}

static void reset_layout(GtkWidget *widget,gpointer user) {
    (void)widget;
    Ui *u=user;
    u->vertical_position=330;
    u->horizontal_position=470;
    gtk_paned_set_position(GTK_PANED(u->search_split),88);
    gtk_paned_set_position(GTK_PANED(u->main_split),310);
    gtk_paned_set_position(GTK_PANED(u->article_split),u->wide?u->horizontal_position:u->vertical_position);
}

static void save_state(Ui *u) {
    int width,height;
    gtk_window_get_size(GTK_WINDOW(u->window),&width,&height);
    if(!gtk_window_is_maximized(GTK_WINDOW(u->window))) {
        g_key_file_set_integer(u->state,"window","width",width);
        g_key_file_set_integer(u->state,"window","height",height);
    }
    g_key_file_set_integer(u->state,"window","maximized",gtk_window_is_maximized(GTK_WINDOW(u->window))?1:0);
    g_key_file_set_integer(u->state,"window","search",gtk_paned_get_position(GTK_PANED(u->search_split)));
    g_key_file_set_integer(u->state,"window","sidebar",gtk_paned_get_position(GTK_PANED(u->main_split)));
    if(u->wide)u->horizontal_position=gtk_paned_get_position(GTK_PANED(u->article_split));
    else u->vertical_position=gtk_paned_get_position(GTK_PANED(u->article_split));
    g_key_file_set_integer(u->state,"window","list_vertical",u->vertical_position);
    g_key_file_set_integer(u->state,"window","list_horizontal",u->horizontal_position);
    g_key_file_set_integer(u->state,"window","wide",u->wide?1:0);
    g_key_file_set_integer(u->state,"window","theme",u->theme);
    g_key_file_set_integer(u->state,"window","font",(int)u->font_points);
    g_key_file_set_integer(u->state,"window","threads",gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(u->thread_spin)));
    g_key_file_set_integer(u->state,"window","published",gtk_combo_box_get_active(GTK_COMBO_BOX(u->basis)));
    gsize length=0;
    gchar *text=g_key_file_to_data(u->state,&length,NULL);
    char *error=NULL;
    if(!fm_write_file_atomic(u->state_path,text,&error)) {
        fprintf(stderr,"feedman: %s\n",error);
        free(error);
    }
    g_free(text);
}

static gboolean close_window(GtkWidget *widget,GdkEvent *event,gpointer user) {
    (void)widget;
    (void)event;
    Ui *u=user;
    if(u->closing)return TRUE;
    save_state(u);
    u->closing=true;
    if(u->debounce){
        g_source_remove(u->debounce);
        u->debounce=0;
    }
    if(u->fetch_tick){
        g_source_remove(u->fetch_tick);
        u->fetch_tick=0;
    }
    cancel_selection_jobs(u);
    if(u->fetch)atomic_store(&u->fetch->options.cancel,true);
    gtk_widget_hide(u->window);
    if(!u->pending)gtk_main_quit();
    return TRUE;
}

static gboolean key_pressed(GtkWidget *widget,GdkEvent *event,gpointer user) {
    (void)widget;
    Ui *u=user;
    guint key=0;
    GdkModifierType mods=0;
    gdk_event_get_keyval(event,&key);
    gdk_event_get_state(event,&mods);
    if((mods&GDK_CONTROL_MASK)&&(key==GDK_KEY_l||key==GDK_KEY_f)) {
        gtk_widget_grab_focus(u->entry);
        gtk_editable_select_region(GTK_EDITABLE(u->entry),0,-1);
        return TRUE;
    }
    if((mods&GDK_CONTROL_MASK)&&key==GDK_KEY_q){
        close_window(u->window,NULL,u);
        return TRUE;
    }
    if((mods&GDK_CONTROL_MASK)&&key==GDK_KEY_o){
        open_article(NULL,u);
        return TRUE;
    }
    if((mods&GDK_CONTROL_MASK)&&(key==GDK_KEY_plus||key==GDK_KEY_equal||key==GDK_KEY_KP_Add)){
        zoom(u,1);
        return TRUE;
    }
    if((mods&GDK_CONTROL_MASK)&&(key==GDK_KEY_minus||key==GDK_KEY_KP_Subtract)){
        zoom(u,-1);
        return TRUE;
    }
    if((mods&GDK_CONTROL_MASK)&&key==GDK_KEY_0){
        zoom(u,12-u->font_points);
        return TRUE;
    }
    if(key==GDK_KEY_F5){
        start_fetch(NULL,u);
        return TRUE;
    }
    if(key==GDK_KEY_Escape&&u->fetch){
        cancel_fetch(NULL,u);
        return TRUE;
    }
    if((mods&GDK_MOD1_MASK)&&key==GDK_KEY_Left){
        neighbor(u,false);
        return TRUE;
    }
    if((mods&GDK_MOD1_MASK)&&key==GDK_KEY_Right){
        neighbor(u,true);
        return TRUE;
    }
    return FALSE;
}

static void add_text_column(Ui *u,const char *name,int model_column,int width,bool expand) {
    GtkCellRenderer *renderer=gtk_cell_renderer_text_new();
    g_object_set(renderer,"ellipsize",PANGO_ELLIPSIZE_END,"ypad",6,"xpad",10,NULL);
    GtkTreeViewColumn *col=gtk_tree_view_column_new_with_attributes(name,renderer,"text",model_column,NULL);
    if(model_column==COL_TITLE)gtk_tree_view_column_add_attribute(col,renderer,"weight",COL_WEIGHT);
    gtk_tree_view_column_set_resizable(col,TRUE);
    gtk_tree_view_column_set_sizing(col,GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(col,width);
    gtk_tree_view_column_set_expand(col,expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(u->tree),col);
}

static GtkWidget *settings_menu(Ui *u) {
    GtkWidget *menu=gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(menu),"表示・設定");
    GtkWidget *popover=gtk_popover_new(menu),*box=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
    margin(box,16);
    gtk_container_add(GTK_CONTAINER(popover),box);
    gtk_box_pack_start(GTK_BOX(box),label("表示と取得", "section-title"),FALSE,FALSE,0);
    GtkWidget *row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,12);
    gtk_box_pack_start(GTK_BOX(row),label("並列取得数",NULL),TRUE,TRUE,0);
    u->thread_spin=gtk_spin_button_new_with_range(1,64,1);
    int threads=u->options->threads;
    if(!u->options->threads_set)threads=state_int(u,"threads",threads,1,64);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(u->thread_spin),threads);
    gtk_box_pack_end(GTK_BOX(row),u->thread_spin,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(box),row,FALSE,FALSE,0);
    u->layout_combo=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->layout_combo),"一覧の下に本文");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->layout_combo),"一覧の右に本文");
    gtk_combo_box_set_active(GTK_COMBO_BOX(u->layout_combo),u->wide?1:0);
    g_signal_connect(u->layout_combo,"changed",G_CALLBACK(layout_changed),u);
    gtk_box_pack_start(GTK_BOX(box),u->layout_combo,FALSE,FALSE,0);
    u->theme_combo=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->theme_combo),"システムのテーマ");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->theme_combo),"ライト");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->theme_combo),"ダーク");
    gtk_combo_box_set_active(GTK_COMBO_BOX(u->theme_combo),u->theme);
    g_signal_connect(u->theme_combo,"changed",G_CALLBACK(theme_changed),u);
    gtk_box_pack_start(GTK_BOX(box),u->theme_combo,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(box),button("パネルの初期配置に戻す",NULL,G_CALLBACK(reset_layout),u),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(box),button("フィード設定を編集",NULL,G_CALLBACK(edit_feeds),u),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(box),button("feeds.txt を再読込",NULL,G_CALLBACK(reload_feeds),u),FALSE,FALSE,0);
    GtkWidget *hint=label("Ctrl+L  検索　 F5  取得\nAlt+← / →  記事のある日へ\nCtrl+＋ / −  本文の文字サイズ\nCtrl+O  元記事　 Ctrl+Q  終了","dim-label");
    gtk_box_pack_start(GTK_BOX(box),hint,FALSE,FALSE,4);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu),popover);
    gtk_widget_show_all(box);
    return menu;
}
#ifdef FEEDMAN_UI_TESTING
#include "../tests/ui_checks.inc"
#endif
int fm_ui_run(FmAppOptions *options) {
    if(!gtk_init_check(NULL,NULL)){
        fputs("feedman: GTK display unavailable. Use --fetch for headless operation.\n",stderr);
        return 1;
    }
    FmDb db={
        0
    };
    char *error=NULL;
    if(!fm_db_open(&db,options->db_path,true,&error)){
        fprintf(stderr,"feedman: %s\n",error);
        free(error);
        return 1;
    }
    fm_db_close(&db);
    Ui *u=fm_calloc(1,sizeof *u);
    g_mutex_init(&u->config_mutex);
    u->options=options;
    u->query_text=fm_strdup("");
    u->state_path=fm_path_join(options->data_dir,"ui.ini");
    u->state=g_key_file_new();
    g_key_file_load_from_file(u->state,u->state_path,G_KEY_FILE_NONE,NULL);
    u->wide=state_int(u,"wide",0,0,1)!=0;
    u->theme=state_int(u,"theme",0,0,2);
    u->font_points=state_int(u,"font",12,9,28);
    u->vertical_position=state_int(u,"list_vertical",330,60,3000);
    u->horizontal_position=state_int(u,"list_horizontal",470,100,6000);
    gboolean dark=FALSE;
    g_object_get(gtk_settings_get_default(),"gtk-application-prefer-dark-theme",&dark,NULL);
    u->system_dark=dark!=FALSE;
    u->window=gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(u->window),"feedman");
    gtk_window_set_icon_name(GTK_WINDOW(u->window),"application-rss+xml");
    gtk_window_set_default_size(GTK_WINDOW(u->window),state_int(u,"width",1320,760,10000),state_int(u,"height",860,560,10000));
    gtk_widget_set_size_request(u->window,760,560);
    g_signal_connect(u->window,"delete-event",G_CALLBACK(close_window),u);
    g_signal_connect(u->window,"key-press-event",G_CALLBACK(key_pressed),u);
    GtkCssProvider *css=gtk_css_provider_new();
    const char *style="window { font-family: \"Noto Sans CJK JP\", \"Noto Sans\", sans-serif; font-size: 14px; } .panel { background-color: @theme_base_color; border: 1px solid alpha(@theme_fg_color,0.13); border-radius: 8px; padding: 12px; }"
    ".section-title { font-weight: 700; } .dim-label { opacity: 0.70; } button { min-height: 26px; padding: 5px 10px; } entry { min-height: 30px; }"
    "paned > separator { min-width: 8px; min-height: 8px; border-radius: 5px; background: alpha(@theme_fg_color,0.07); }"
    "paned > separator:hover { background: alpha(@theme_selected_bg_color,0.7); }"
    "treeview { font-size: 14px; } calendar { font-size: 13px; padding: 2px; } .reader { padding: 0; } .statusbar { padding: 7px 14px; }";
    gtk_css_provider_load_from_data(css,style,-1,NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),GTK_STYLE_PROVIDER(css),GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    GtkWidget *header=gtk_header_bar_new();
    gtk_header_bar_set_title(GTK_HEADER_BAR(header),"feedman");
    gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),"ローカルに保存して、落ち着いて読む");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header),TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(u->window),header);
    u->fetch_button=button("取得  F5","feeds.txt のURLを並列取得します。起動時には取得しません。",G_CALLBACK(start_fetch),u);
    gtk_style_context_add_class(gtk_widget_get_style_context(u->fetch_button),"suggested-action");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header),u->fetch_button);
    u->cancel_button=button("停止","Esc: 取得を停止",G_CALLBACK(cancel_fetch),u);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(header),u->cancel_button);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header),settings_menu(u));
    GtkWidget *root=gtk_box_new(GTK_ORIENTATION_VERTICAL,0);
    gtk_container_add(GTK_CONTAINER(u->window),root);
    u->search_split=gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    gtk_paned_set_wide_handle(GTK_PANED(u->search_split),TRUE);
    gtk_box_pack_start(GTK_BOX(root),u->search_split,TRUE,TRUE,6);
    GtkWidget *search_panel=panel();
    GtkWidget *search_title=label("日付へ移動 / 記事を検索","section-title");
    gtk_box_pack_start(GTK_BOX(search_panel),search_title,FALSE,FALSE,0);
    GtkWidget *search_row=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    u->entry=gtk_search_entry_new();
    gtk_widget_set_name(u->entry,"feedman-search");
    gtk_entry_set_placeholder_text(GTK_ENTRY(u->entry),"20260912 で日付移動 / タイトル・本文のキーワード");
    gtk_entry_set_max_length(GTK_ENTRY(u->entry),256);
    gtk_box_pack_start(GTK_BOX(search_row),u->entry,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(search_row),button("移動・検索",NULL,G_CALLBACK(search_now),u),FALSE,FALSE,0);
    u->all_days=gtk_check_button_new_with_label("全期間");
    u->unread=gtk_check_button_new_with_label("未読のみ");
    gtk_box_pack_start(GTK_BOX(search_row),u->all_days,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(search_row),u->unread,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(search_panel),search_row,FALSE,FALSE,0);
    GtkWidget *search_scroll=scroller(search_panel);
    gtk_paned_pack1(GTK_PANED(u->search_split),search_scroll,FALSE,TRUE);
    u->main_split=gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_wide_handle(GTK_PANED(u->main_split),TRUE);
    gtk_paned_pack2(GTK_PANED(u->search_split),u->main_split,TRUE,TRUE);
    GtkWidget *sidebar=panel();
    gtk_box_pack_start(GTK_BOX(sidebar),label("アーカイブ","section-title"),FALSE,FALSE,0);
    u->basis=gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->basis),"取得日で見る");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(u->basis),"公開日で見る");
    gtk_combo_box_set_active(GTK_COMBO_BOX(u->basis),options->by_publication?1:state_int(u,"published",0,0,1));
    gtk_box_pack_start(GTK_BOX(sidebar),u->basis,FALSE,FALSE,0);
    u->calendar=gtk_calendar_new();
    gtk_widget_set_name(u->calendar,"feedman-calendar");
    gtk_calendar_set_display_options(GTK_CALENDAR(u->calendar),GTK_CALENDAR_SHOW_HEADING|GTK_CALENDAR_SHOW_DAY_NAMES);
    gtk_box_pack_start(GTK_BOX(sidebar),u->calendar,FALSE,FALSE,0);
    GtkWidget *legend=label("太字の日付に記事があります。\n取得日 = 初めて保存した日","dim-label");
    gtk_label_set_line_wrap(GTK_LABEL(legend),TRUE);
    gtk_box_pack_start(GTK_BOX(sidebar),legend,FALSE,FALSE,0);
    GtkWidget *nav=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    gtk_box_pack_start(GTK_BOX(nav),button("前の記録","Alt+←: 前の記事のある日へ",G_CALLBACK(previous_day),u),TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(nav),button("次の記録","Alt+→: 次の記事のある日へ",G_CALLBACK(next_day),u),TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(sidebar),nav,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(sidebar),button("今日",NULL,G_CALLBACK(today),u),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(sidebar),label("タグで絞り込む","section-title"),FALSE,FALSE,8);
    u->tag_filter=gtk_combo_box_text_new();
    gtk_box_pack_start(GTK_BOX(sidebar),u->tag_filter,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(sidebar),button("フィード設定",NULL,G_CALLBACK(edit_feeds),u),FALSE,FALSE,0);
    GtkWidget *help_text=label("境界線をドラッグして\n各パネルの大きさを調整できます。\n配置は終了時に保存されます。","dim-label");
    gtk_label_set_line_wrap(GTK_LABEL(help_text),TRUE);
    gtk_box_pack_end(GTK_BOX(sidebar),help_text,FALSE,FALSE,8);
    gtk_paned_pack1(GTK_PANED(u->main_split),scroller(sidebar),FALSE,TRUE);
    u->article_split=gtk_paned_new(u->wide?GTK_ORIENTATION_HORIZONTAL:GTK_ORIENTATION_VERTICAL);
    g_signal_connect(u->article_split,"size-allocate",G_CALLBACK(article_allocated),u);
    gtk_paned_set_wide_handle(GTK_PANED(u->article_split),TRUE);
    gtk_paned_pack2(GTK_PANED(u->main_split),u->article_split,TRUE,TRUE);
    GtkWidget *list_panel=panel(),*list_header=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    u->list_title=label("記事一覧","section-title");
    gtk_label_set_ellipsize(GTK_LABEL(u->list_title),PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(list_header),u->list_title,TRUE,TRUE,0);
    u->count=label("読み込み中…","dim-label");
    gtk_box_pack_end(GTK_BOX(list_header),u->count,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(list_panel),list_header,FALSE,FALSE,0);
    u->store=gtk_list_store_new(N_COLS,G_TYPE_INT64,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_STRING,G_TYPE_INT,G_TYPE_STRING);
    u->tree=gtk_tree_view_new_with_model(GTK_TREE_MODEL(u->store));
    gtk_tree_view_set_fixed_height_mode(GTK_TREE_VIEW(u->tree),TRUE);
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(u->tree),COL_TOOLTIP);
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(u->tree)),GTK_SELECTION_BROWSE);
    add_text_column(u,"記事名",COL_TITLE,360,true);
    add_text_column(u,"日時",COL_DATE,170,false);
    add_text_column(u,"配信元",COL_SOURCE,150,false);
    u->list_stack=gtk_stack_new();
    gtk_stack_set_homogeneous(GTK_STACK(u->list_stack),FALSE);
    gtk_stack_add_named(GTK_STACK(u->list_stack),scroller(u->tree),"list");
    GtkWidget *empty=gtk_label_new("記事がありません\n\n日付・タグ・検索条件を変更するか、\n「取得」でフィードを読み込んでください。");
    gtk_style_context_add_class(gtk_widget_get_style_context(empty),"dim-label");
    gtk_stack_add_named(GTK_STACK(u->list_stack),empty,"empty");
    gtk_box_pack_start(GTK_BOX(list_panel),u->list_stack,TRUE,TRUE,0);
    GtkWidget *pages=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,6);
    u->previous=button("前の250件",NULL,G_CALLBACK(previous_page),u);
    u->next=button("次の250件",NULL,G_CALLBACK(next_page),u);
    gtk_box_pack_end(GTK_BOX(pages),u->next,FALSE,FALSE,0);
    gtk_box_pack_end(GTK_BOX(pages),u->previous,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(list_panel),pages,FALSE,FALSE,0);
    gtk_paned_pack1(GTK_PANED(u->article_split),list_panel,TRUE,TRUE);
    GtkWidget *reader_panel=panel(),*reader_header=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,8);
    gtk_box_pack_start(GTK_BOX(reader_header),label("記事内容","section-title"),TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(reader_header),button("A−","本文を小さく (Ctrl+-)",G_CALLBACK(zoom_out),u),FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(reader_header),button("A＋","本文を大きく (Ctrl++)",G_CALLBACK(zoom_in),u),FALSE,FALSE,0);
    u->open_button=button("元記事を開く","Ctrl+O: 外部ブラウザーで開く",G_CALLBACK(open_article),u);
    gtk_box_pack_end(GTK_BOX(reader_header),u->open_button,FALSE,FALSE,0);
    gtk_box_pack_start(GTK_BOX(reader_panel),reader_header,FALSE,FALSE,0);
    u->reader=gtk_text_view_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(u->reader),"reader");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(u->reader),FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(u->reader),FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(u->reader),GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(u->reader),14);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(u->reader),14);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(u->reader),12);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(u->reader),18);
    gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(u->reader),5);
    gtk_box_pack_start(GTK_BOX(reader_panel),scroller(u->reader),TRUE,TRUE,0);
    gtk_paned_pack2(GTK_PANED(u->article_split),reader_panel,TRUE,TRUE);
    GtkWidget *statusbar=gtk_box_new(GTK_ORIENTATION_HORIZONTAL,10);
    gtk_style_context_add_class(gtk_widget_get_style_context(statusbar),"statusbar");
    u->status=label("待機中 · 起動や閲覧だけでは通信しません","dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(u->status),PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(statusbar),u->status,TRUE,TRUE,0);
    u->details=button("詳細",NULL,G_CALLBACK(show_details),u);
    gtk_box_pack_end(GTK_BOX(statusbar),u->details,FALSE,FALSE,0);
    u->progress=gtk_progress_bar_new();
    gtk_widget_set_size_request(u->progress,130,-1);
    gtk_box_pack_end(GTK_BOX(statusbar),u->progress,FALSE,FALSE,0);
    gtk_box_pack_end(GTK_BOX(root),statusbar,FALSE,FALSE,0);
    g_signal_connect(u->entry,"changed",G_CALLBACK(search_changed),u);
    g_signal_connect(u->entry,"activate",G_CALLBACK(search_now),u);
    g_signal_connect(u->calendar,"day-selected",G_CALLBACK(day_changed),u);
    g_signal_connect(u->calendar,"month-changed",G_CALLBACK(month_changed),u);
    g_signal_connect(u->basis,"changed",G_CALLBACK(filters_changed),u);
    g_signal_connect(u->tag_filter,"changed",G_CALLBACK(filters_changed),u);
    g_signal_connect(u->all_days,"toggled",G_CALLBACK(filters_changed),u);
    g_signal_connect(u->unread,"toggled",G_CALLBACK(filters_changed),u);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(u->tree)),"changed",G_CALLBACK(selection_changed),u);
    g_signal_connect(u->tree,"row-activated",G_CALLBACK(row_activated),u);
    gtk_paned_set_position(GTK_PANED(u->search_split),state_int(u,"search",88,0,3000));
    gtk_paned_set_position(GTK_PANED(u->main_split),state_int(u,"sidebar",310,0,6000));
    gtk_paned_set_position(GTK_PANED(u->article_split),u->wide?u->horizontal_position:u->vertical_position);
    theme_changed(GTK_COMBO_BOX(u->theme_combo),u);
    clear_reader(u,"記事を選択すると、ここに内容を表示します。");
    u->changing=true;
    refresh_filters(u);
    char initial_day[11];
    if(*options->date)memcpy(initial_day,options->date,11);
    else fm_local_day((int64_t)time(NULL),initial_day);
    int y,m,d;
    sscanf(initial_day,"%d-%d-%d",&y,&m,&d);
    gtk_calendar_select_month(GTK_CALENDAR(u->calendar),(guint)m-1,(guint)y);
    gtk_calendar_select_day(GTK_CALENDAR(u->calendar),(guint)d);
    memcpy(u->day,initial_day,11);
    u->changing=false;
    u->needs_sync=true;
    gtk_widget_show_all(u->window);
    gtk_widget_hide(u->cancel_button);
    gtk_widget_hide(u->progress);
    gtk_widget_hide(u->details);
    if(state_int(u,"maximized",0,0,1))gtk_window_maximize(GTK_WINDOW(u->window));
    load_list(u);
#ifdef FEEDMAN_UI_TESTING
    g_timeout_add(40,ui_test_tick,u);
#endif
    gtk_main();
    gtk_widget_destroy(u->window);
    g_object_unref(u->store);
    g_key_file_unref(u->state);
    free(u->state_path);
    free(u->query_text);
    free(u->selected_tag);
    free(u->selected_url);
    free(u->last_errors);
    fm_article_free(&u->current_article);
    g_mutex_clear(&u->config_mutex);
    free(u);
    return 0;
}
