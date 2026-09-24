/* SPDX-License-Identifier: MIT */
#include "feedman.h"
#include <libxml/parser.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
static bool named(const xmlNode *n,const char *name) {
    return n && n->type==XML_ELEMENT_NODE && !xmlStrcmp(n->name,(const xmlChar *)name);
}

static xmlNode *child(xmlNode *n,const char *name) {
    for(xmlNode *c=n?n->children:NULL;c;c=c->next)if(named(c,name))return c;
    return NULL;
}

static char *content(xmlNode *n) {
    xmlChar *s=n?xmlNodeGetContent(n):NULL;
    char *p=fm_strdup((const char *)s);
    xmlFree(s);
    return p;
}

static bool dropped(xmlNode *n) {
    const char *name=(const char *)n->name;
    return name && (!strcasecmp(name,"script")||!strcasecmp(name,"style")||!strcasecmp(name,"head")||
    !strcasecmp(name,"iframe")||!strcasecmp(name,"object")||!strcasecmp(name,"embed")||
    !strcasecmp(name,"svg")||!strcasecmp(name,"form")||!strcasecmp(name,"noscript"));
}

static bool block(xmlNode *n) {
    const char *name=(const char *)n->name;
    return name && (!strcasecmp(name,"p")||!strcasecmp(name,"div")||!strcasecmp(name,"article")||
    !strcasecmp(name,"section")||!strcasecmp(name,"li")||!strcasecmp(name,"blockquote")||
    !strcasecmp(name,"tr")||!strcasecmp(name,"pre")||
    (name[0]=='h' && name[1]>='1' && name[1]<='6' && !name[2]));
}

static void newline(FmString *s) {
    if(s->len && s->data[s->len-1]!='\n')fm_string_append(s,"\n");
}

static void plain_walk(xmlNode *n,FmString *s,unsigned depth,bool pre) {
    if(depth>80)return;
    for(;n;n=n->next) {
        if(n->type==XML_TEXT_NODE || n->type==XML_CDATA_SECTION_NODE) {
            const unsigned char *t=n->content;
            if(!t)continue;
            for(;*t;t++) {
                if(!pre && (*t==' '||*t=='\n'||*t=='\r'||*t=='\t')) {
                    if(s->len && s->data[s->len-1]!=' ' && s->data[s->len-1]!='\n')fm_string_append(s," ");
                }
                else fm_string_append_n(s,(const char *)t,1);
            }
        }
        else if(n->type==XML_ELEMENT_NODE && !dropped(n)) {
            if(named(n,"br")){
                newline(s);
                continue;
            }
            bool b=block(n);
            if(b)newline(s);
            if(named(n,"li"))fm_string_append(s,"• ");
            if(named(n,"img")) {
                xmlChar *alt=xmlGetProp(n,(const xmlChar *)"alt");
                if(alt && *alt){
                    fm_string_append(s,"[画像: ");
                    fm_string_append(s,(const char *)alt);
                    fm_string_append(s,"]");
                }
                xmlFree(alt);
            }
            plain_walk(n->children,s,depth+1,pre||named(n,"pre"));
            if(b)newline(s);
        }
    }
}

char *fm_html_plain(const char *html) {
    if(!html || !*html)return fm_strdup("");
    size_t n=strlen(html);
    if(n>FM_MAX_FEED_BYTES)return fm_strdup("（本文が大きすぎます）");
    htmlDocPtr doc=htmlReadMemory(html,(int)n,NULL,"UTF-8",HTML_PARSE_NONET|HTML_PARSE_NOERROR|HTML_PARSE_NOWARNING|HTML_PARSE_COMPACT);
    if(!doc)return fm_strdup(html);
    FmString s={
        0
    };
    plain_walk(xmlDocGetRootElement(doc),&s,0,false);
    xmlFreeDoc(doc);
    char *raw=fm_string_take(&s),*p=fm_strdup(fm_trim(raw));
    free(raw);
    return p;
}

static char *one_line(char *input) {
    char *s=input;
    for(char *p=s;*p;p++) if(*p=='\n'||*p=='\r'||*p=='\t')*p=' ';
    char *p=fm_strdup(fm_trim(s));
    free(s);
    return p;
}

static char *inner_xml(xmlNode *n) {
    xmlBufferPtr buffer=xmlBufferCreate();
    if(!buffer)return fm_strdup("");
    for(xmlNode *c=n?n->children:NULL;c;c=c->next)xmlNodeDump(buffer,n->doc,c,0,0);
    char *ret=fm_strdup((const char *)xmlBufferContent(buffer));
    xmlBufferFree(buffer);
    return ret;
}

static char *atom_body(xmlNode *n) {
    if(!n)return fm_strdup("");
    xmlChar *type=xmlGetProp(n,(const xmlChar *)"type");
    char *ret=NULL;
    if(type && !xmlStrcmp(type,(const xmlChar *)"xhtml"))ret=inner_xml(n);
    else if(type && (!xmlStrcmp(type,(const xmlChar *)"html")||!xmlStrcmp(type,(const xmlChar *)"text/html")))ret=content(n);
    else {
        char *text=content(n);
        xmlChar *safe=xmlEncodeSpecialChars(n->doc,(const xmlChar *)text);
        FmString buf={
            0
        };
        fm_string_append(&buf,"<p>");
        fm_string_append(&buf,(const char *)safe);
        fm_string_append(&buf,"</p>");
        ret=fm_string_take(&buf);
        xmlFree(safe);
        free(text);
    }
    xmlFree(type);
    return ret;
}
/* RSS titles and default Atom titles are text, not arbitrary HTML. */
static char *title_text(xmlNode *n,bool atom) {
    if(!atom)return one_line(content(n));
    xmlChar *type=n?xmlGetProp(n,(const xmlChar *)"type"):NULL;
    bool html=type&&(!xmlStrcmp(type,(const xmlChar *)"html")||!xmlStrcmp(type,(const xmlChar *)"xhtml"));
    xmlFree(type);
    if(!html)return one_line(content(n));
    char *markup=atom_body(n),*plain=fm_html_plain(markup);
    free(markup);
    return one_line(plain);
}

static char *resolve_node(xmlNode *n,const char *url,const char *fallback) {
    xmlChar *base=n?xmlNodeGetBase(n->doc,n):NULL;
    char *ret=fm_url_resolve(url,base?(const char *)base:fallback);
    xmlFree(base);
    return ret;
}

static char *article_link(xmlNode *entry,bool atom,const char *base) {
    if(atom) {
        for(xmlNode *n=entry->children;n;n=n->next) if(named(n,"link")) {
            xmlChar *rel=xmlGetProp(n,(const xmlChar *)"rel"),*href=xmlGetProp(n,(const xmlChar *)"href");
            bool alternate=!rel||!xmlStrcmp(rel,(const xmlChar *)"alternate");
            char *url=alternate?resolve_node(n,(const char *)href,base):NULL;
            xmlFree(rel);
            xmlFree(href);
            if(url && *url)return url;
            free(url);
        }
        return fm_strdup("");
    }
    xmlNode *link=child(entry,"link");
    char *text=content(link),*url=resolve_node(link,text,base);
    free(text);
    if(*url)return url;
    xmlNode *guid=child(entry,"guid");
    xmlChar *perma=guid?xmlGetProp(guid,(const xmlChar *)"isPermaLink"):NULL;
    if(!perma || xmlStrcasecmp(perma,(const xmlChar *)"false")) {
        free(url);
        text=content(guid);
        url=resolve_node(guid,text,base);
        free(text);
    }
    xmlFree(perma);
    return url;
}

void fm_article_free(FmArticle *a) {
    free(a->key);
    free(a->title);
    free(a->url);
    free(a->html);
    free(a->plain);
    free(a->source);
    free(a->tag);
    *a=(FmArticle){
        0
    };
}

void fm_articles_free(FmArticles *a) {
    for(size_t i=0;i<a->len;i++)fm_article_free(&a->items[i]);
    free(a->items);
    free(a->title);
    *a=(FmArticles){
        0
    };
}

bool fm_parse_feed(const char *xml,size_t length,const char *base,FmArticles *out,char **error) {
    *out=(FmArticles){
        0
    };
    if(!length || length>FM_MAX_FEED_BYTES || length>INT_MAX) {
        fm_error(error,"Empty or oversized feed");
        return false;
    }
    xmlDocPtr doc=xmlReadMemory(xml,(int)length,base,NULL,XML_PARSE_NONET|XML_PARSE_NOERROR|XML_PARSE_NOWARNING|XML_PARSE_COMPACT);
    if(!doc) {
        fm_error(error,"Invalid XML (not an RSS/Atom feed)");
        return false;
    }
    /* The global loader denies every external entity, including file://. No
       NOENT, DTDLOAD or HUGE flags are used. DTD-bearing feeds are rejected. */
    if(doc->intSubset || doc->extSubset) {
        fm_error(error,"DTD/entity declarations are not allowed");
        xmlFreeDoc(doc);
        return false;
    }
    xmlNode *root=xmlDocGetRootElement(doc),*parent=NULL;
    bool atom=named(root,"feed");
    if(atom)parent=root;
    else if(named(root,"rss"))parent=child(root,"channel");
    else if(named(root,"RDF"))parent=root;
    if(!parent) {
        fm_error(error,"Unsupported feed root (RSS 1/2 or Atom required)");
        xmlFreeDoc(doc);
        return false;
    }
    out->title=title_text(child(atom?root:(named(root,"RDF")?child(root,"channel"):parent),"title"),atom);
    for(xmlNode *entry=parent->children;entry;entry=entry->next) {
        if(!named(entry,atom?"entry":"item"))continue;
        if(out->len>=FM_MAX_ARTICLES) {
            fm_error(error,"Feed exceeds %u entries",FM_MAX_ARTICLES);
            goto fail;
        }
        FmArticle a={
            0
        };
        a.title=title_text(child(entry,"title"),atom);
        a.url=article_link(entry,atom,base);
        if(!*a.title) {
            free(a.title);
            a.title=fm_strdup(*a.url?a.url:"（タイトルなし）");
        }
        xmlNode *date=child(entry,atom?"published":"pubDate");
        if(!date)date=child(entry,atom?"updated":"date");
        char *date_text=content(date);
        fm_parse_time(date_text,&a.published);
        a.key=content(child(entry,atom?"id":"guid"));
        char *trimmed=fm_strdup(fm_trim(a.key));
        free(a.key);
        a.key=trimmed;
        if(!*a.key) {
            free(a.key);
            if(*a.url)a.key=fm_strdup(a.url);
            else {
                FmString key={
                    0
                };
                fm_string_printf(&key,"fallback:%s\x1f%s",a.title,date_text);
                a.key=fm_string_take(&key);
            }
        }
        free(date_text);
        if(atom) {
            xmlNode *body=child(entry,"content");
            if(!body)body=child(entry,"summary");
            a.html=atom_body(body);
        }
        else {
            xmlNode *body=NULL;
            for(xmlNode *n=entry->children;n;n=n->next) if(named(n,"encoded") && n->ns && n->ns->href &&
            !xmlStrcmp(n->ns->href,(const xmlChar *)"http://purl.org/rss/1.0/modules/content/")){
                body=n;
                break;
            }
            if(!body)body=child(entry,"description");
            a.html=content(body);
        }
        if(strlen(a.html)>2u*1024u*1024u || strlen(a.title)>65536 || strlen(a.key)>65536) {
            fm_article_free(&a);
            fm_error(error,"Oversized article field");
            goto fail;
        }
        a.plain=fm_html_plain(a.html);
        if(out->len==out->cap) {
            out->cap=out->cap?out->cap*2:64;
            FmArticle *p=realloc(out->items,out->cap*sizeof *p);
            if(!p)abort();
            out->items=p;
        }
        out->items[out->len++]=a;
    }
    xmlFreeDoc(doc);
    return true;
    fail:
    xmlFreeDoc(doc);
    fm_articles_free(out);
    return false;
}
