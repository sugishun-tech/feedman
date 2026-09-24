/* SPDX-License-Identifier: MIT */
#include "reader.h"
#include <libxml/HTMLparser.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
typedef struct {
    GtkTextBuffer *buffer;
    const char *base;
    unsigned breaks,links;
    bool pre;
} Reader;
static void insert(Reader *r,const char *text) {
    if(!text||!*text)return;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(r->buffer,&end);
    gtk_text_buffer_insert(r->buffer,&end,text,-1);
    for(const char *p=text;*p;p++) {
        if(*p=='\n')r->breaks++;
        else if(*p!='\r')r->breaks=0;
    }
}

static void paragraph(Reader *r,unsigned count) {
    while(r->breaks<count)insert(r,"\n");
}

static bool named(xmlNode *n,const char *name) {
    return n&&n->type==XML_ELEMENT_NODE&&!xmlStrcasecmp(n->name,(const xmlChar *)name);
}

static gboolean link_event(GtkTextTag *tag,GObject *object,GdkEvent *event,GtkTextIter *iter,gpointer user) {
    (void)tag;
    (void)iter;
    guint button=0;
    if(gdk_event_get_event_type(event)!=GDK_BUTTON_RELEASE||!gdk_event_get_button(event,&button)||button!=1)return FALSE;
    if(GTK_IS_TEXT_VIEW(object)&&gtk_text_buffer_get_has_selection(gtk_text_view_get_buffer(GTK_TEXT_VIEW(object))))return FALSE;
    GtkWidget *window=gtk_widget_get_toplevel(GTK_WIDGET(object));
    GError *error=NULL;
    if(!gtk_show_uri_on_window(GTK_IS_WINDOW(window)?GTK_WINDOW(window):NULL,user,GDK_CURRENT_TIME,&error)) {
        GtkWidget *dialog=gtk_message_dialog_new(GTK_IS_WINDOW(window)?GTK_WINDOW(window):NULL,GTK_DIALOG_MODAL,GTK_MESSAGE_ERROR,GTK_BUTTONS_CLOSE,"ブラウザーを開けませんでした");
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),"%s",error?error->message:"Unknown error");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        g_clear_error(&error);
    }
    return TRUE;
}

static void free_link(gpointer data,GClosure *closure) {
    (void)closure;
    free(data);
}

static void apply(Reader *r,const char *tag,int start,int finish) {
    GtkTextIter a,b;
    gtk_text_buffer_get_iter_at_offset(r->buffer,&a,start);
    gtk_text_buffer_get_iter_at_offset(r->buffer,&b,finish);
    gtk_text_buffer_apply_tag_by_name(r->buffer,tag,&a,&b);
}

static bool is_block(xmlNode *n) {
    return named(n,"p")||named(n,"div")||named(n,"article")||named(n,"section")||named(n,"blockquote")||named(n,"pre")||named(n,"tr");
}

static void walk(Reader *r,xmlNode *node,unsigned depth) {
    if(depth>64)return;
    for(xmlNode *n=node;n;n=n->next) {
        if(n->type==XML_TEXT_NODE||n->type==XML_CDATA_SECTION_NODE) {
            if(r->pre)insert(r,(const char *)n->content);
            else {
                FmString text={
                    0
                };
                bool space=false;
                for(const unsigned char *p=n->content;p&&*p;p++) {
                    if(*p=='\n'||*p=='\t'||*p=='\r'||*p==' ') {
                        if(!space)fm_string_append(&text," ");
                        space=true;
                    }
                    else {
                        fm_string_append_n(&text,(const char *)p,1);
                        space=false;
                    }
                }
                insert(r,text.data);
                free(text.data);
            }
            continue;
        }
        if(n->type!=XML_ELEMENT_NODE)continue;
        if(named(n,"head")||named(n,"script")||named(n,"style")||named(n,"iframe")||named(n,"object")||named(n,"embed")||named(n,"svg")||named(n,"form")||named(n,"noscript"))continue;
        if(named(n,"br")){
            insert(r,"\n");
            continue;
        }
        if(named(n,"hr")){
            paragraph(r,2);
            insert(r,"────────────────────────\n");
            continue;
        }
        if(named(n,"img")) {
            xmlChar *alt=xmlGetProp(n,(const xmlChar *)"alt");
            if(alt&&*alt){
                insert(r,"[画像: ");
                insert(r,(const char *)alt);
                insert(r,"]");
            }
            xmlFree(alt);
            continue;
        }
        bool block=is_block(n);
        bool heading=n->name&&n->name[0]=='h'&&n->name[1]>='1'&&n->name[1]<='6'&&!n->name[2];
        if(block||heading)paragraph(r,2);
        if(named(n,"li")){
            paragraph(r,1);
            insert(r,"  • ");
        }
        int start=gtk_text_buffer_get_char_count(r->buffer);
        bool old_pre=r->pre;
        r->pre|=named(n,"pre");
        walk(r,n->children,depth+1);
        r->pre=old_pre;
        int finish=gtk_text_buffer_get_char_count(r->buffer);
        if(heading)apply(r,"heading",start,finish);
        if(named(n,"b")||named(n,"strong"))apply(r,"bold",start,finish);
        if(named(n,"i")||named(n,"em"))apply(r,"italic",start,finish);
        if(named(n,"code")||named(n,"pre"))apply(r,"mono",start,finish);
        if(named(n,"blockquote"))apply(r,"quote",start,finish);
        if(named(n,"a")&&r->links<512) {
            xmlChar *href=xmlGetProp(n,(const xmlChar *)"href");
            char *url=fm_url_resolve((const char *)href,r->base);
            xmlFree(href);
            if(*url) {
                char name[32];
                snprintf(name,sizeof name,"link-%u",++r->links);
                GtkTextTag *tag=gtk_text_buffer_create_tag(r->buffer,name,"underline",PANGO_UNDERLINE_SINGLE,NULL);
                g_signal_connect_data(tag,"event",G_CALLBACK(link_event),url,free_link,0);
                apply(r,name,start,finish);
            }
            else free(url);
        }
        if(block||heading)paragraph(r,2);
        else if(named(n,"li"))paragraph(r,1);
    }
}

void fm_reader_show(GtkTextView *view,const FmArticle *a,double points) {
    GtkTextBuffer *buffer=gtk_text_buffer_new(NULL);
    gtk_text_buffer_create_tag(buffer,"base","size-points",points,NULL);
    gtk_text_buffer_create_tag(buffer,"title","weight",PANGO_WEIGHT_BOLD,"scale",1.45,"pixels-below-lines",8,NULL);
    gtk_text_buffer_create_tag(buffer,"meta","scale",0.85,NULL);
    gtk_text_buffer_create_tag(buffer,"heading","weight",PANGO_WEIGHT_BOLD,"scale",1.15,NULL);
    gtk_text_buffer_create_tag(buffer,"bold","weight",PANGO_WEIGHT_BOLD,NULL);
    gtk_text_buffer_create_tag(buffer,"italic","style",PANGO_STYLE_ITALIC,NULL);
    gtk_text_buffer_create_tag(buffer,"mono","family","monospace",NULL);
    gtk_text_buffer_create_tag(buffer,"quote","left-margin",18,"style",PANGO_STYLE_ITALIC,NULL);
    Reader r={
        .buffer=buffer,.base=a->url&&*a->url?a->url:a->source
    };
    if(a->tag&&*a->tag){
        insert(&r,"[");
        insert(&r,a->tag);
        insert(&r,"] ");
    }
    insert(&r,a->title?a->title:"");
    apply(&r,"title",0,gtk_text_buffer_get_char_count(buffer));
    paragraph(&r,2);
    int meta=gtk_text_buffer_get_char_count(buffer);
    char *p=fm_format_time(a->published),*f=fm_format_time(a->first_seen),*host=fm_url_host(a->source?a->source:"");
    FmString info={
        0
    };
    fm_string_printf(&info,"%s\n公開 %s　 /　取得 %s%s",host,p,f,a->legacy?"（旧データの取込日）":"");
    insert(&r,info.data);
    apply(&r,"meta",meta,gtk_text_buffer_get_char_count(buffer));
    paragraph(&r,2);
    free(info.data);
    free(p);
    free(f);
    free(host);
    if(a->html&&*a->html) {
        htmlDocPtr doc=htmlReadMemory(a->html,(int)strlen(a->html),r.base,"UTF-8",HTML_PARSE_NONET|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING|HTML_PARSE_COMPACT);
        if(doc){
            walk(&r,xmlDocGetRootElement(doc),0);
            xmlFreeDoc(doc);
        }
        else insert(&r,a->html);
    }
    else insert(&r,"フィードに本文が含まれていません。「元記事を開く」で閲覧できます。");
    apply(&r,"base",0,gtk_text_buffer_get_char_count(buffer));
    gtk_text_view_set_buffer(view,buffer);
    g_object_unref(buffer);
}
