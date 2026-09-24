/* Included only into the dedicated FEEDMAN_UI_TESTING build, never the app.
 * Exercise actual GTK widgets and asynchronous callbacks under Xvfb. */
#define UI_CHECK(x) do { if(!(x)){fprintf(stderr,"UI FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static gboolean ui_test_tick(gpointer data) {
    Ui *u=data;
    static unsigned step=0,ticks=0;
    UI_CHECK(++ticks<600);
    if(u->pending||u->debounce)return G_SOURCE_CONTINUE;
    switch(step++) {
        case 0: {
            UI_CHECK(u->current_article.id!=0);
            UI_CHECK(gtk_tree_model_iter_n_children(GTK_TREE_MODEL(u->store),NULL)==250);
            UI_CHECK(!strcmp(u->current_article.tag,"IT"));
            GtkTreeIter it;
            gchar *title=NULL;
            UI_CHECK(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(u->store),&it));
            gtk_tree_model_get(GTK_TREE_MODEL(u->store),&it,COL_TITLE,&title,-1);
            UI_CHECK(strncmp(title,"[IT] ",5)==0);
            g_free(title);
            next_page(NULL,u);
            break;
        }
        case 1:
        UI_CHECK(gtk_tree_model_iter_n_children(GTK_TREE_MODEL(u->store),NULL)==50);
        gtk_combo_box_set_active(GTK_COMBO_BOX(u->basis),1);
        gtk_entry_set_text(GTK_ENTRY(u->entry),"20260912");
        search_now(NULL,u);
        break;
        case 2:
        UI_CHECK(!strcmp(u->day,"2026-09-12"));
        UI_CHECK(u->current_article.id!=0);
        gtk_entry_set_text(GTK_ENTRY(u->entry),"20260230");
        search_now(NULL,u);
        UI_CHECK(!strcmp(u->day,"2026-09-12"));
        gtk_entry_set_text(GTK_ENTRY(u->entry),"京都市");
        search_now(NULL,u);
        break;
        case 3:
        UI_CHECK(gtk_tree_model_iter_n_children(GTK_TREE_MODEL(u->store),NULL)==1);
        UI_CHECK(strstr(u->current_article.html,"京都市")!=NULL);
        gtk_paned_set_position(GTK_PANED(u->main_split),390);
        gtk_paned_set_position(GTK_PANED(u->search_split),115);
        gtk_combo_box_set_active(GTK_COMBO_BOX(u->layout_combo),1);
        zoom(u,2);
        break;
        case 4:
        UI_CHECK(u->wide);
        gtk_paned_set_position(GTK_PANED(u->article_split),420);
        break;
        case 5: {
            UI_CHECK(gtk_paned_get_position(GTK_PANED(u->main_split))==390);
            UI_CHECK(gtk_paned_get_position(GTK_PANED(u->article_split))==420);
            UI_CHECK(u->wide&&u->font_points==14);
            save_state(u);
            UI_CHECK(state_int(u,"sidebar",0,0,6000)==390);
            UI_CHECK(state_int(u,"search",0,0,3000)==115);
            UI_CHECK(state_int(u,"wide",0,0,1)==1);
            UI_CHECK(state_int(u,"font",0,0,30)==14);
            GKeyFile *on_disk=g_key_file_new();
            UI_CHECK(g_key_file_load_from_file(on_disk,u->state_path,G_KEY_FILE_NONE,NULL));
            UI_CHECK(g_key_file_get_integer(on_disk,"window","sidebar",NULL)==390);
            UI_CHECK(g_key_file_get_integer(on_disk,"window","wide",NULL)==1);
            UI_CHECK(g_key_file_get_integer(on_disk,"window","font",NULL)==14);
            g_key_file_unref(on_disk);
            puts("PASS GTK: date jump / invalid date / Japanese search / tag prefix / pagination / selection / pane positions / orientation / font / saved state");
            /* Queue work immediately before closing: exercise lifetime and cancellation. */
            gtk_entry_set_text(GTK_ENTRY(u->entry),"");
            search_now(NULL,u);
            close_window(u->window,NULL,u);
            return G_SOURCE_REMOVE;
        }
        default:exit(1);
    }
    return G_SOURCE_CONTINUE;
}
#undef UI_CHECK
