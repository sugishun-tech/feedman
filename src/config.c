/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "feedman.h"
#include <libxml/xmlstring.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
const char *fm_example_feeds(void) {
    return "# feedman: optional-tag URL (UTF-8)\n"
    "# Tags cannot contain whitespace. URL-only lines are also accepted.\n"
    "IT https://www.techmeme.com/feed.xml\n"
    "経済 https://www.cnbc.com/id/10000664/device/rss/rss.html\n";
}

void fm_feeds_free(FmFeeds *f) {
    for(size_t i=0;i<f->len;i++) {
        free(f->items[i].url);
        free(f->items[i].tag);
        free(f->items[i].etag);
        free(f->items[i].modified);
    }
    free(f->items);
    *f=(FmFeeds){
        0
    };
}

bool fm_feeds_parse(const char *text,FmFeeds *out,char **error) {
    *out=(FmFeeds){
        0
    };
    if(!text || !xmlCheckUTF8((const xmlChar *)text)) {
        fm_error(error,"feeds.txt must be valid UTF-8");
        return false;
    }
    if(strlen(text)>4u*1024u*1024u) {
        fm_error(error,"feeds.txt exceeds 4 MiB");
        return false;
    }
    char *copy=fm_strdup(text),*line=copy;
    size_t line_no=0;
    if(!strncmp(line,"\xef\xbb\xbf",3))line+=3;
    while(line) {
        char *end=strchr(line,'\n');
        if(end)*end=0;
        ++line_no;
        char *s=fm_trim(line);
        if(*s && *s!='#') {
            char *first=s;
            while(*s&&!isspace((unsigned char)*s))s++;
            char *rest=s;
            if(*s){
                *s++=0;
                rest=fm_trim(s);
            }
            const char *url=NULL,*tag="";
            if(fm_url_valid(first)) {
                url=first;
                if(*rest && *rest!='#')goto invalid;
            }
            else {
                tag=first;
                url=rest;
                s=rest;
                while(*s&&!isspace((unsigned char)*s))s++;
                if(*s){
                    *s++=0;
                    s=fm_trim(s);
                    if(*s && *s!='#')goto invalid;
                }
                if(!fm_url_valid(url))goto invalid;
                if(!strcmp(tag,"-"))tag="";
            }
            if(strlen(tag)>256 || !*url)goto invalid;
            for(const unsigned char *p=(const unsigned char *)tag;*p;p++) if(*p<32 || *p==127)goto invalid;
            bool duplicate=false;
            for(size_t i=0;i<out->len;i++) if(!strcmp(url,out->items[i].url)) {
                if(strcmp(tag,out->items[i].tag)) {
                    fm_error(error,"Line %zu: the same URL has different tags",line_no);
                    goto fail;
                }
                duplicate=true;
                break;
            }
            if(!duplicate) {
                if(out->len>=10000) {
                    fm_error(error,"Too many feeds (maximum 10000)");
                    goto fail;
                }
                if(out->len==out->cap) {
                    out->cap=out->cap?out->cap*2:16;
                    FmFeed *p=realloc(out->items,out->cap*sizeof *p);
                    if(!p)abort();
                    out->items=p;
                }
                out->items[out->len++]=(FmFeed){
                    .url=fm_strdup(url),.tag=fm_strdup(tag)
                };
            }
        }
        line=end?end+1:NULL;
        continue;
        invalid:
        fm_error(error,"Line %zu: expected [tag] http(s)://URL; tags cannot contain spaces",line_no);
        goto fail;
    }
    free(copy);
    return true;
    fail:
    free(copy);
    fm_feeds_free(out);
    return false;
}

bool fm_feeds_load(const char *path,FmFeeds *out,char **error) {
    char *text=NULL;
    size_t length=0;
    if(!fm_read_file(path,4u*1024u*1024u,&text,&length,error))return false;
    if(memchr(text,0,length)) {
        free(text);
        fm_error(error,"feeds.txt contains a NUL byte");
        return false;
    }
    bool ok=fm_feeds_parse(text,out,error);
    free(text);
    return ok;
}
