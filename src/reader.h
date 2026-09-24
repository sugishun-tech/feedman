/* SPDX-License-Identifier: MIT */
#ifndef FEEDMAN_READER_H
#define FEEDMAN_READER_H
#include <gtk/gtk.h>
#include "feedman.h"
void fm_reader_show(GtkTextView *view,const FmArticle *article,double points);
#endif
