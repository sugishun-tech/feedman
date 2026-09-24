/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "feedman.h"
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/uri.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
void *fm_malloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("feedman: out of memory\n", stderr);
        abort();
    }
    return p;
}

void *fm_calloc(size_t n, size_t s) {
    if (s && n > SIZE_MAX / s) abort();
    void *p = calloc(n ? n : 1, s ? s : 1);
    if (!p) {
        fputs("feedman: out of memory\n", stderr);
        abort();
    }
    return p;
}

char *fm_strdup(const char *s) {
    return fm_strndup(s ? s : "", s ? strlen(s) : 0);
}

char *fm_strndup(const char *s, size_t n) {
    char *p = fm_malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void fm_error(char **error, const char *fmt, ...) {
    if (!error || *error) return;
    va_list ap;
    va_start(ap, fmt);
    if (vasprintf(error, fmt, ap) < 0) *error = fm_strdup("Out of memory");
    va_end(ap);
}

void fm_string_append_n(FmString *s, const char *text, size_t n) {
    if (!n) return;
    if (n > SIZE_MAX - s->len - 1) abort();
    size_t need = s->len + n + 1;
    if (need > s->cap) {
        size_t cap = s->cap ? s->cap : 256;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        char *p = realloc(s->data, cap);
        if (!p) abort();
        s->data = p;
        s->cap = cap;
    }
    memcpy(s->data + s->len, text, n);
    s->len += n;
    s->data[s->len] = 0;
}

void fm_string_append(FmString *s, const char *text) {
    if (text) fm_string_append_n(s, text, strlen(text));
}

void fm_string_printf(FmString *s, const char *fmt, ...) {
    char *buf = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&buf, fmt, ap);
    va_end(ap);
    if (n >= 0) fm_string_append_n(s, buf, (size_t)n);
    free(buf);
}

char *fm_string_take(FmString *s) {
    char *p = s->data ? s->data : fm_strdup("");
    *s = (FmString){
        0
    };
    return p;
}

char *fm_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = 0;
    return s;
}

static bool valid_ymd(int y, int m, int d) {
    static const int md[] = {
        31,28,31,30,31,30,31,31,30,31,30,31
    };
    if (y < 1 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
    int max = md[m-1] + (m == 2 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
    return d <= max;
}

bool fm_date_normalize(const char *s, char out[11]) {
    if (!s) return false;
    size_t n = strlen(s);
    char digits[9] = {
        0
    };
    if (n == 8) memcpy(digits, s, 8);
    else if (n == 10 && s[4] == '-' && s[7] == '-') {
        memcpy(digits,s,4);
        memcpy(digits+4,s+5,2);
        memcpy(digits+6,s+8,2);
    }
    else return false;
    for (int i=0; i<8; i++) if (!isdigit((unsigned char)digits[i])) return false;
    int y = (digits[0]-'0')*1000+(digits[1]-'0')*100+(digits[2]-'0')*10+digits[3]-'0';
    int m = (digits[4]-'0')*10+digits[5]-'0', d = (digits[6]-'0')*10+digits[7]-'0';
    if (!valid_ymd(y,m,d)) return false;
    /* The digits are already validated; direct formatting is bounded and locale-free. */
    memcpy(out,digits,4);out[4]='-';memcpy(out+5,digits+4,2);out[7]='-';memcpy(out+8,digits+6,2);out[10]=0;
    return true;
}

bool fm_parse_time(const char *s, int64_t *out) {
    if (!s || !*s) return false;
    size_t n = strlen(s);
    if (n >= 10 && s[4] == '-' && s[7] == '-') {
        char date[11];
        memcpy(date,s,10);
        date[10]=0;
        if (!fm_date_normalize(date,date)) return false;
        int y=0,m=0,d=0,hh=0,mm=0,ss=0,pos=0;
        sscanf(date,"%d-%d-%d",&y,&m,&d);
        const char *tail=s+10;
        if (*tail == 'T' || *tail == 't' || *tail == ' ') {
            ++tail;
            if (sscanf(tail,"%2d:%2d:%2d%n",&hh,&mm,&ss,&pos) < 3) {
                ss=0;
                pos=0;
                if (sscanf(tail,"%2d:%2d%n",&hh,&mm,&pos) != 2) return false;
            }
            if (hh>23 || mm>59 || ss>59 || hh<0 || mm<0 || ss<0) return false;
            tail += pos;
            if (*tail == '.') {
                ++tail;
                if (!isdigit((unsigned char)*tail)) return false;
                while (isdigit((unsigned char)*tail)) ++tail;
            }
        }
        else if (*tail) return false;
        bool utc=false;
        int offset=0;
        if (*tail == 'Z' || *tail == 'z') {
            utc=true;
            ++tail;
        }
        else if (*tail == '+' || *tail == '-') {
            int sign=(*tail++ == '+') ? 1 : -1, oh=0,om=0;
            size_t tn=strlen(tail);
            if (tn < 2 || !isdigit((unsigned char)tail[0]) || !isdigit((unsigned char)tail[1])) return false;
            oh=(tail[0]-'0')*10+tail[1]-'0';
            tail+=2;
            if (*tail == ':') ++tail;
            if (strlen(tail)<2 || !isdigit((unsigned char)tail[0]) || !isdigit((unsigned char)tail[1])) return false;
            om=(tail[0]-'0')*10+tail[1]-'0';
            tail+=2;
            if (oh>23 || om>59) return false;
            utc=true;
            offset=sign*(oh*3600+om*60);
        }
        while (isspace((unsigned char)*tail)) ++tail;
        if (*tail) return false;
        struct tm tm={
            .tm_year=y-1900,.tm_mon=m-1,.tm_mday=d,.tm_hour=hh,.tm_min=mm,.tm_sec=ss,.tm_isdst=-1
        };
        time_t t=utc ? timegm(&tm) : mktime(&tm);
        if (t == (time_t)-1) return false;
        *out=(int64_t)t-offset;
        return true;
    }
    time_t t=curl_getdate(s,NULL);
    if(t==(time_t)-1) return false;
    *out=(int64_t)t;
    return true;
}

void fm_local_day(int64_t timestamp, char out[11]) {
    time_t t=(time_t)timestamp;
    struct tm tm;
    if (!localtime_r(&t,&tm) || !strftime(out,11,"%Y-%m-%d",&tm)) memcpy(out,"1970-01-01",11);
}

char *fm_format_time(int64_t timestamp) {
    if (!timestamp) return fm_strdup("不明");
    time_t t=(time_t)timestamp;
    struct tm tm;
    char buf[80];
    if (!localtime_r(&t,&tm) || !strftime(buf,sizeof buf,"%Y-%m-%d %H:%M %Z",&tm)) return fm_strdup("不明");
    return fm_strdup(buf);
}

double fm_clock(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec+t.tv_nsec/1e9;
}

bool fm_url_valid(const char *url) {
    if (!url || strlen(url)>16384) return false;
    for (const unsigned char *p=(const unsigned char *)url; *p; p++) if (*p<=32 || *p==127) return false;
    CURLU *u=curl_url();
    if(!u) return false;
    char *scheme=NULL,*host=NULL;
    bool ok=curl_url_set(u,CURLUPART_URL,url,0)==CURLUE_OK &&
    curl_url_get(u,CURLUPART_SCHEME,&scheme,0)==CURLUE_OK &&
    curl_url_get(u,CURLUPART_HOST,&host,0)==CURLUE_OK && host && *host &&
    (!strcasecmp(scheme,"http") || !strcasecmp(scheme,"https"));
    curl_free(scheme);
    curl_free(host);
    curl_url_cleanup(u);
    return ok;
}

char *fm_url_resolve(const char *url, const char *base) {
    if (!url || !*url) return fm_strdup("");
    char *tmp=fm_strdup(url),*trim=fm_trim(tmp);
    xmlChar *v=xmlBuildURI((const xmlChar *)trim,(const xmlChar *)(base?base:""));
    const char *s=v?(const char *)v:trim;
    char *result=fm_strdup(fm_url_valid(s)?s:"");
    xmlFree(v);
    free(tmp);
    return result;
}

char *fm_url_host(const char *url) {
    CURLU *u=curl_url();
    char *host=NULL;
    char *ret=NULL;
    if (u && curl_url_set(u,CURLUPART_URL,url,0)==CURLUE_OK && curl_url_get(u,CURLUPART_HOST,&host,0)==CURLUE_OK)
    ret=fm_strdup(host);
    curl_free(host);
    if(u) curl_url_cleanup(u);
    return ret?ret:fm_strdup("import");
}

bool fm_mkdirs(const char *path, char **error) {
    char *p=fm_strdup(path);
    size_t n=strlen(p);
    for(size_t i=1;i<=n;i++) if(p[i]=='/' || !p[i]) {
        char c=p[i];
        p[i]=0;
        if(*p && mkdir(p,0700)<0 && errno!=EEXIST) {
            fm_error(error,"Cannot create directory: %s",strerror(errno));
            free(p);
            return false;
        }
        struct stat st;
        if (*p && (stat(p,&st)!=0 || !S_ISDIR(st.st_mode))) {
            fm_error(error,"Not a directory: %s",p);
            free(p);
            return false;
        }
        p[i]=c;
    }
    free(p);
    return true;
}

bool fm_read_file(const char *path,size_t limit,char **out,size_t *len,char **error) {
    *out=NULL;
    FILE *f=fopen(path,"rb");
    if(!f) {
        fm_error(error,"Cannot read %s: %s",path,strerror(errno));
        return false;
    }
    FmString s={
        0
    };
    char buf[16384];
    size_t n;
    while((n=fread(buf,1,sizeof buf,f))>0) {
        if(n>limit-s.len) {
            fm_error(error,"File exceeds %zu bytes",limit);
            free(s.data);
            fclose(f);
            return false;
        }
        fm_string_append_n(&s,buf,n);
    }
    if(ferror(f)) {
        fm_error(error,"Read error: %s",strerror(errno));
        free(s.data);
        fclose(f);
        return false;
    }
    fclose(f);
    if(len)*len=s.len;
    *out=fm_string_take(&s);
    return true;
}

bool fm_write_file_atomic(const char *path,const char *text,char **error) {
    FmString name={
        0
    };
    fm_string_printf(&name,"%s.tmp.XXXXXX",path);
    int fd=mkstemp(name.data);
    if(fd<0) {
        fm_error(error,"Cannot create file: %s",strerror(errno));
        free(name.data);
        return false;
    }
    size_t len=strlen(text),pos=0;
    bool ok=true;
    while(pos<len) {
        ssize_t n=write(fd,text+pos,len-pos);
        if(n<0 && errno==EINTR) continue;
        if(n<=0) {
            ok=false;
            break;
        }
        pos+=(size_t)n;
    }
    if(ok && fsync(fd)!=0) ok=false;
    if(close(fd)!=0) ok=false;
    if(ok && rename(name.data,path)!=0) ok=false;
    if(!ok) {
        fm_error(error,"Cannot save file: %s",strerror(errno));
        unlink(name.data);
    }
    free(name.data);
    return ok;
}

char *fm_path_join(const char *a,const char *b) {
    FmString s={
        0
    };
    fm_string_append(&s,a);
    if(s.len && s.data[s.len-1]!='/')fm_string_append(&s,"/");
    fm_string_append(&s,b);
    return fm_string_take(&s);
}

size_t fm_utf8_length(const char *s) {
    size_t n=0;
    if(s) for(const unsigned char *p=(const unsigned char *)s;*p;p++) if((*p&0xc0)!=0x80)n++;
    return n;
}

static xmlParserInputPtr no_external(const char *url,const char *id,xmlParserCtxtPtr ctxt) {
    (void)url;
    (void)id;
    (void)ctxt;
    return NULL;
}

void fm_global_init(void) {
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK) {
        fputs("Cannot initialize libcurl\n",stderr);
        exit(1);
    }
    xmlInitParser();
    xmlSetExternalEntityLoader(no_external);
    signal(SIGPIPE,SIG_IGN);
}

void fm_global_cleanup(void) {
    xmlCleanupParser();
    curl_global_cleanup();
}
