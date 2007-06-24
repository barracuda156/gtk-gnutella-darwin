/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup gtk
 * @file
 *
 * GUI filtering functions.
 *
 * @author Raphael Manfredi
 * @author Richard Eckart
 * @date 2001-2003
 */

#include "gtk/gui.h"

#include "search_cb.h"

#include "gtk/bitzi.h"
#include "gtk/columns.h"
#include "gtk/drag.h"
#include "gtk/gtk-missing.h"
#include "gtk/misc.h"
#include "gtk/notebooks.h"
#include "gtk/search.h"
#include "gtk/settings.h"
#include "gtk/statusbar.h"

#include "if/gui_property.h"
#include "if/gui_property_priv.h"
#include "if/bridge/ui2c.h"
#include "if/core/bitzi.h"
#include "if/core/sockets.h"

#include "lib/atoms.h"
#include "lib/magnet.h"
#include "lib/misc.h"
#include "lib/glib-missing.h"
#include "lib/iso3166.h"
#include "lib/tm.h"
#include "lib/url.h"
#include "lib/utf8.h"
#include "lib/walloc.h"

#include "lib/override.h"		/* Must be the last header included */

RCSID("$Id$")

static GList *searches;	/**< List of search structs */

static GtkTreeView *tree_view_search;
static GtkNotebook *notebook_search_results;
static GtkButton *button_search_clear;

static gboolean search_gui_shutting_down = FALSE;

/*
 * Private function prototypes.
 */
static void gui_search_create_tree_view(GtkWidget ** sw,
				GtkTreeView ** tv, gpointer udata);

/*
 * If no search are currently allocated
 */
static GtkTreeView *default_search_tree_view;
GtkWidget *default_scrolled_window;


/** For cyclic updates of the tooltip. */
static tree_view_motion_t *tvm_search;

struct result_data {
	GtkTreeIter iter;

	record_t *record;
	const gchar *meta;	/**< Atom */
	guint children;		/**< count of children */
	guint32 rank;		/**< for stable sorting */
	enum gui_color color;
};

static inline struct result_data *
get_result_data(GtkTreeModel *model, GtkTreeIter *iter)
{
	static const GValue zero_value;
	GValue value = zero_value;
	struct result_data *rd;

	gtk_tree_model_get_value(model, iter, 0, &value);
	rd = g_value_get_pointer(&value);
	record_check(rd->record);
	g_assert(rd->record->refcount > 0);
	return rd;
}

gpointer
search_gui_get_record(GtkTreeModel *model, GtkTreeIter *iter)
{
	return get_result_data(model, iter)->record;
}

void
search_gui_set_data(GtkTreeModel *model, struct result_data *rd)
{
	static const GValue zero_value;
	GValue value = zero_value;

	g_value_init(&value, G_TYPE_POINTER);
	g_value_set_pointer(&value, rd);
	gtk_tree_store_set_value(GTK_TREE_STORE(model), &rd->iter, 0, &value);
}

static void
search_gui_synchronize_list(GtkTreeModel *model)
{
	GtkTreeIter iter;
	gboolean valid;
	GList *list;

	valid = gtk_tree_model_get_iter_first(model, &iter);
	for (list = searches; list != NULL; list = g_list_next(list)) {
		g_assert(valid);
		list->data = NULL;
    	gtk_tree_model_get(model, &iter, c_sl_sch, &list->data, (-1));
		g_assert(list->data);
		valid = gtk_tree_model_iter_next(model, &iter);
	}
}

static void
on_search_list_row_deleted(GtkTreeModel *model, GtkTreePath *unused_path,
	gpointer unused_udata)
{
	(void) unused_path;
	(void) unused_udata;

	search_gui_synchronize_list(model);
	search_gui_option_menu_searches_update();
}

void
on_search_list_column_clicked(GtkTreeViewColumn *column, gpointer unused_udata)
{
	(void) unused_udata;
	
	search_gui_synchronize_list(gtk_tree_view_get_model(
		GTK_TREE_VIEW(column->tree_view)));
	search_gui_option_menu_searches_update();
}


/**
 * Callback handler used with gtk_tree_model_foreach() to record the current
 * rank/position in tree enabling stable sorting. 
 */
gboolean
search_gui_update_rank(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer udata)
{
	guint32 *rank_ptr = udata;
	struct result_data *data;

	(void) path;
	
	data = get_result_data(model, iter);
	data->rank = *rank_ptr;
	(*rank_ptr)++;
	return FALSE;
}

static void
cell_renderer(GtkTreeViewColumn *column, GtkCellRenderer *cell, 
	GtkTreeModel *model, GtkTreeIter *iter, gpointer udata)
{
	const struct result_data *data;
    const struct results_set *rs;
	const gchar *text;
	enum c_sr_columns id;

	if (!gtk_tree_view_column_get_visible(column))
		return;

	text = NULL;	/* default to nothing */
	id = GPOINTER_TO_UINT(udata);
	data = get_result_data(model, iter);
    rs = data->record->results_set;

	switch (id) {
	case c_sr_filename:
		text = data->record->utf8_name;
		break;
	case c_sr_ext:
		text = data->record->ext;
		break;
	case c_sr_meta:
		text = data->meta;
		break;
	case c_sr_vendor:
		if (!(ST_LOCAL & rs->status))
			text = lookup_vendor_name(rs->vcode);
		break;
	case c_sr_info:
		text = data->record->info;
		break;
	case c_sr_size:
		text = compact_size(data->record->size, show_metric_units());
		break;
	case c_sr_count:
		text = data->children ? uint32_to_string(1 + data->children) : NULL;
		break;
	case c_sr_loc:
		if (ISO3166_INVALID != rs->country)
			text = iso3166_country_cc(rs->country);
		break;
	case c_sr_charset:
		if (!(ST_LOCAL & rs->status))
			text = data->record->charset;
		break;
	case c_sr_route:
		text = search_gui_get_route(rs);
		break;
	case c_sr_protocol:
		if (!((ST_LOCAL | ST_BROWSE) & rs->status))
			text = ST_UDP & rs->status ? "UDP" : "TCP";
		break;
	case c_sr_hops:
		if (!((ST_LOCAL | ST_BROWSE) & rs->status))
			text = uint32_to_string(rs->hops);
		break;
	case c_sr_ttl:
		if (!((ST_LOCAL | ST_BROWSE) & rs->status))
			text = uint32_to_string(rs->ttl);
		break;
	case c_sr_spam:
		if (SR_SPAM & data->record->flags) {
			text = "S";	/* Spam */
		} else if (ST_SPAM & rs->status) {
			text = "maybe";	/* maybe spam */
		}
		break;
	case c_sr_owned:
		if (SR_OWNED & data->record->flags) {
			text = _("owned");
		} else if (SR_PARTIAL & data->record->flags) {
			text = _("partial");
		} else if (SR_SHARED & data->record->flags) {
			text = _("shared");
		}
		break;
	case c_sr_hostile:
		if (ST_HOSTILE & rs->status) {
			text = "H";
		}
		break;
	case c_sr_sha1:
		if (data->record->sha1) {
			text = sha1_base32(data->record->sha1);
		}
		break;
	case c_sr_ctime:
		if (data->record->create_time) {
			text = timestamp_to_string(data->record->create_time);
		}
		break;
	case c_sr_num:
		g_assert_not_reached();
		break;
	}
	g_object_set(cell,
		"text", text,
		"foreground-gdk", gui_color_get(data->color),
		"background-gdk", (void *) 0,
		(void *) 0);
}

static GtkCellRenderer *
create_cell_renderer(gfloat xalign)
{
	GtkCellRenderer *renderer;
	
	renderer = gtk_cell_renderer_text_new();
	gtk_cell_renderer_text_set_fixed_height_from_font(
		GTK_CELL_RENDERER_TEXT(renderer), TRUE);
	g_object_set(renderer,
		"mode",		GTK_CELL_RENDERER_MODE_INERT,
		"xalign",	xalign,
		"ypad",		(guint) GUI_CELL_RENDERER_YPAD,
		(void *) 0);

	return renderer;
}

static GtkTreeViewColumn *
add_column(
	GtkTreeView *tv,
	const gchar *name,
	gint id,
	gint width,
	gfloat xalign,
	GtkTreeCellDataFunc cell_data_func,
	gint fg_col,
	gint bg_col)
{
    GtkTreeViewColumn *column;
	GtkCellRenderer *renderer;

	renderer = create_cell_renderer(xalign);
	g_object_set(G_OBJECT(renderer),
		"foreground-set",	TRUE,
		"background-set",	TRUE,
		(void *) 0);

	if (cell_data_func) {
		column = gtk_tree_view_column_new_with_attributes(name, renderer,
					(void *) 0);
		gtk_tree_view_column_set_cell_data_func(column, renderer,
			cell_data_func, GUINT_TO_POINTER(id), NULL);
	} else {
		column = gtk_tree_view_column_new_with_attributes(name, renderer,
					"text", id, (void *) 0);
	}

	if (fg_col >= 0)
		gtk_tree_view_column_add_attribute(column, renderer,
			"foreground-gdk", fg_col);
	if (bg_col >= 0)
		gtk_tree_view_column_add_attribute(column, renderer,
			"background-gdk", bg_col);
			
	g_object_set(column,
		"fixed-width", MAX(1, width),
		"min-width", 1,
		"reorderable", FALSE,
		"resizable", TRUE,
		"sizing", GTK_TREE_VIEW_COLUMN_FIXED,
		(void *) 0);
	
    gtk_tree_view_append_column(tv, column);

	return column;
}

static struct result_data *
find_parent(search_t *search, const struct result_data *rd)
{
	struct result_data *parent;

	/* NOTE: rd->record is not checked due to find_parent2() */
	parent = g_hash_table_lookup(search->parents, rd);
	if (parent) {
		record_check(parent->record);
	}
	return parent;
}

static struct result_data *
find_parent2(search_t *search, const struct sha1 *sha1, filesize_t filesize)
{
	struct result_data key;
	record_t rc;

	g_return_val_if_fail(sha1, NULL);

	rc.sha1 = sha1;
	rc.size = filesize;
	key.record = &rc;
  	return find_parent(search, &key);
}

static void
result_data_free(search_t *search, struct result_data *rd)
{
	record_check(rd->record);

	atom_str_free_null(&rd->meta);

	g_assert(g_hash_table_lookup(search->dups, rd->record) != NULL);
	g_hash_table_remove(search->dups, rd->record);
	search_gui_unref_record(rd->record);

	search_gui_unref_record(rd->record);
	/*
	 * rd->record may point to freed memory now if this was the last reference
	 */

	wfree(rd, sizeof *rd);
}

static gboolean
prepare_remove_record(GtkTreeModel *model, GtkTreePath *unused_path,
	GtkTreeIter *iter, gpointer udata)
{
	struct result_data *rd;
	record_t *rc;
	search_t *search;

	(void) unused_path;

	search = udata;
	rd = get_result_data(model, iter);
	rc = rd->record;

	if (rc->sha1) {
		struct result_data *parent;
		
		parent = find_parent(search, rd);
		if (rd == parent) {
			g_hash_table_remove(search->parents, rd);
		} else if (parent) {
			parent->children--;
			search_gui_set_data(model, parent);
		}
	}
	result_data_free(search, rd);
	return FALSE;
}

static void
search_gui_clear_queue(search_t *search)
{
	if (slist_length(search->queue) > 0) {
		slist_iter_t *iter;

		iter = slist_iter_on_head(search->queue);
		while (slist_iter_has_item(iter)) {
			struct result_data *rd;

			rd = slist_iter_current(iter);
			slist_iter_remove(iter);
			result_data_free(search, rd);
		}
		slist_iter_free(&iter);
	}
}

/**
 * Reset internal search model.
 * Called when a search is restarted, for example.
 */
void
search_gui_reset_search(search_t *sch)
{
	search_gui_clear_search(sch);
}

static gboolean
on_leave_notify(GtkWidget *widget, GdkEventCrossing *unused_event,
		gpointer unused_udata)
{
	(void) unused_event;
	(void) unused_udata;

	search_update_tooltip(GTK_TREE_VIEW(widget), NULL);
	return FALSE;
}

static gboolean
gui_search_update_tab_label_cb(gpointer p)
{
	struct search *sch = p;
	
	return gui_search_update_tab_label(sch);
}

static void
search_gui_clear_ctree(search_t *search)
{
	GtkTreeModel *model;

	search_gui_start_massive_update(search);

	model = gtk_tree_view_get_model(search->tree);
	gtk_tree_model_foreach(model, prepare_remove_record, search);
	gtk_tree_store_clear(GTK_TREE_STORE(model));

	search_gui_end_massive_update(search);
}

/**
 * Clear all results from search.
 */
void
search_gui_clear_search(search_t *search)
{
	g_assert(search);
	g_assert(search->dups);

	search_gui_clear_ctree(search);
	search_gui_clear_queue(search);
	g_assert(0 == g_hash_table_size(search->dups));
	g_assert(0 == g_hash_table_size(search->parents));

	search->items = 0;
	search->unseen_items = 0;
	guc_search_update_items(search->search_handle, search->items);
}

/**
 * Remove the search from the list of searches and free all
 * associated ressources (including filter and gui stuff).
 */
void
search_gui_close_search(search_t *search)
{
	g_return_if_fail(search);

    /*
     * We remove the search immeditaly from the list of searches,
     * because some of the following calls (may) depend on
     * "searches" holding only the remaining searches.
     * We may not free any ressources of "search" yet, because
     * the same calls may still need them!.
     *      --BLUE 26/05/2002
     */

	if (tvm_search && search == search_gui_get_current_search()) {
		tree_view_motion_clear_callback(search->tree, tvm_search);
		tvm_search = NULL;
	}
 	searches = g_list_remove(searches, search);
	search_gui_option_menu_searches_update();

	search_gui_clear_search(search);
    search_gui_remove_search(search);
	filter_close_search(search);

	g_hash_table_destroy(search->dups);
	search->dups = NULL;
	g_hash_table_destroy(search->parents);
	search->parents = NULL;

    guc_search_close(search->search_handle);

	g_assert(0 == slist_length(search->queue));
	slist_free(&search->queue);

	wfree(search, sizeof *search);
}

static void
search_gui_clear_sorting(search_t *sch)
{
	GtkTreeModel *model;
	gint id;
		
	g_return_if_fail(sch);

	model = gtk_tree_view_get_model(sch->tree);
	g_return_if_fail(model);

#if GTK_CHECK_VERSION(2,6,0)
	id = GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID;
#else
	id = c_sr_count;
#endif /* Gtk+ >= 2.6.0 */
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
		id, GTK_SORT_ASCENDING);
}

static void
search_gui_restore_sorting(search_t *sch)
{
	GtkTreeModel *model;

	g_return_if_fail(sch);

	model = gtk_tree_view_get_model(sch->tree);
	g_return_if_fail(model);

	if (
		SORT_NONE != sch->sort_order &&
		sch->sort_col >= 0 &&
		(guint) sch->sort_col < SEARCH_RESULTS_VISIBLE_COLUMNS
	) {
		gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
			sch->sort_col, SORT_ASC == sch->sort_order
							? GTK_SORT_ASCENDING : GTK_SORT_DESCENDING);
	} else {
		search_gui_clear_sorting(sch);
	}
}

static gchar *
get_local_file_url(GtkWidget *widget)
{
	const struct result_data *data;
	const gchar *pathname;
	GtkTreeModel *model;
   	GtkTreeIter iter;

	g_return_val_if_fail(widget, NULL);
	if (!drag_get_iter(GTK_TREE_VIEW(widget), &model, &iter))
		return NULL;

	data = get_result_data(model, &iter);
	if (!(ST_LOCAL & data->record->results_set->status))
		return NULL;
	
	pathname = data->record->tag;
	if (NULL == pathname)
		return NULL;
	 
	return url_from_absolute_path(pathname);
}

guint
search_gui_file_hash(gconstpointer key)
{
	const struct result_data *rd = key;
	const record_t *rc = rd->record;
	guint hash;

	hash = rc->size;
	hash ^= rc->size >> 31;
	hash ^= rc->sha1 ? sha1_hash(rc->sha1) : 0;
	return hash;
}

gint
search_gui_file_eq(gconstpointer p, gconstpointer q)
{
	const struct result_data *rd_a = p, *rd_b = q;
	const record_t *a = rd_a->record, *b = rd_b->record;

	return a->sha1 == b->sha1 && a->size == b->size;
}


/**
 * @returns TRUE if search was sucessfully created and FALSE if an error
 * 			occured. If the "search" argument is not NULL a pointer to the new
 *			search is stored there.
 */
gboolean
search_gui_new_search_full(const gchar *query_str,
	time_t create_time, guint lifetime, guint32 reissue_timeout,
	gint sort_col, gint sort_order, flag_t flags, search_t **search)
{
	static const search_t zero_sch;
	const gchar *error_str;
	struct query *query;
	search_t *sch;
	gnet_search_t sch_id;
	GtkListStore *model;
	GtkTreeIter iter;
	gboolean is_only_search;
	
	if (search) {
		*search = NULL;
	}
	query = search_gui_handle_query(query_str, flags, &error_str);
	if (!query) {
		if (error_str) {
			statusbar_gui_warning(5, "%s", error_str);
			return FALSE;
		} else {
			return TRUE;
		}
	}
	g_assert(query);
	g_assert(query->text);
	
	sch_id = guc_search_new(query->text, create_time, lifetime,
				reissue_timeout, flags);
	if ((gnet_search_t) -1 == sch_id) {
		/*
		 * An invalidly encoded SHA1 is already detected by
		 * search_gui_query_parse(), so a too short query is the only reason
		 * this may fail at the moment.
		 */
		statusbar_gui_warning(5, "%s",
			_("The normalized search text is too short."));
		search_gui_query_free(&query);
		return FALSE;
	}

	sch = walloc(sizeof *sch);
	*sch = zero_sch;

	if (sort_col >= 0 && (guint) sort_col < SEARCH_RESULTS_VISIBLE_COLUMNS)
		sch->sort_col = sort_col;
	else
		sch->sort_col = -1;

	switch (sort_order) {
	case SORT_ASC:
	case SORT_DESC:
		sch->sort_order = sort_order;
		break;
	default:
		sch->sort_order = SORT_NONE;
	}
 
	sch->search_handle = sch_id;
	sch->dups = g_hash_table_new(search_gui_hash_func,
						search_gui_hash_key_compare);
	sch->parents = g_hash_table_new(search_gui_file_hash, search_gui_file_eq);
	sch->queue = slist_new();

	search_gui_filter_new(sch, query->rules);

	/* Create a new TreeView if needed, or use the default TreeView */

	if (searches) {
		/* We have to create a new TreeView for this search */
		gui_search_create_tree_view(&sch->scrolled_window,
			&sch->tree, sch);
		gtk_object_set_user_data(GTK_OBJECT(sch->scrolled_window), sch);
		gtk_notebook_append_page(GTK_NOTEBOOK(notebook_search_results),
			 sch->scrolled_window, NULL);
	} else {
		/* There are no searches currently, we can use the default TreeView */

		if (default_scrolled_window && default_search_tree_view) {
			sch->scrolled_window = default_scrolled_window;
			sch->tree = default_search_tree_view;

			default_search_tree_view = NULL;
			default_scrolled_window = NULL;
		} else
			g_warning("new_search():"
				" No current search but no default tree_view !?");

		gtk_object_set_user_data(GTK_OBJECT(sch->scrolled_window), sch);
	}

	search_gui_restore_sorting(sch);

	/* Add the search to the TreeView in pane on the left */
	model = GTK_LIST_STORE(gtk_tree_view_get_model(tree_view_search));
	gtk_list_store_append(model, &iter);
	gtk_list_store_set(model, &iter,
		c_sl_name, search_gui_query(sch),
		c_sl_hit, 0,
		c_sl_new, 0,
		c_sl_sch, sch,
		c_sl_fg, NULL,
		c_sl_bg, NULL,
		(-1));

	sch->tab_updating = gtk_timeout_add(TAB_UPDATE_TIME * 1000,
							gui_search_update_tab_label_cb, sch);

    if (!searches) {
		gtk_notebook_set_tab_label_text(
            GTK_NOTEBOOK(notebook_search_results),
            gtk_notebook_get_nth_page(
				GTK_NOTEBOOK(notebook_search_results), 0), _("(no search)"));
    }
	
	gtk_widget_set_sensitive(gui_main_window_lookup("button_search_close"),
		TRUE);
	
	is_only_search = NULL == searches;
	searches = g_list_append(searches, sch);
	search_gui_option_menu_searches_update();

	if (search_gui_update_expiry(sch) || 0 == (SEARCH_F_ENABLED & flags)) {
		if (search_gui_is_enabled(sch))
			guc_search_stop(sch->search_handle);
	} else {
		if (!search_gui_is_enabled(sch))
			guc_search_start(sch->search_handle);
	}
	gui_search_update_tab_label(sch);

	/*
	 * Make new search the current search, unless it's a browse-host search:
	 * we need to initiate the download and only if everything is OK will
	 * we be able to move to the newly created search.
	 *
	 * If the browse host is the only search in the list, it must be made
	 * the current search though, since the code relies on one always being
	 * set when the list of searches is not empty.
	 */
	
	if (
		is_only_search ||
		(!search_gui_is_browse(sch) && GUI_PROPERTY(search_jump_to_created))
	) {
		search_gui_set_current_search(sch);
	} else {
		gui_search_force_update_tab_label(sch, tm_time());
	}
	if (search) {
		*search = sch;
	}
	search_gui_query_free(&query);
	if (search_gui_is_local(sch)) {
		drag_attach(GTK_WIDGET(sch->tree), get_local_file_url);
	}
	return TRUE;
}

/**
 * Search results.
 */
static gint
search_gui_cmp_size(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;
	
	(void) unused_udata;
	
	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = CMP(d1->record->size, d2->record->size);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_count(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = CMP(d1->children, d2->children);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static inline gint
search_gui_cmp_strings(const gchar *a, const gchar *b)
{
	if (a && b) {
		return a == b ? 0 : strcmp(a, b);
	} else {
		return a ? 1 : (b ? -1 : 0);
	}
}

static gint
search_gui_cmp_filename(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = search_gui_cmp_strings(d1->record->utf8_name, d2->record->utf8_name);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}


static gint
search_gui_cmp_sha1(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = search_gui_cmp_sha1s(d1->record->sha1, d2->record->sha1);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_ctime(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = delta_time(d1->record->create_time, d2->record->create_time);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_charset(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = search_gui_cmp_strings(d1->record->charset, d2->record->charset);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}


static gint
search_gui_cmp_ext(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = search_gui_cmp_strings(d1->record->ext, d2->record->ext);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_meta(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = search_gui_cmp_strings(d1->meta, d2->meta);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}


static gint
search_gui_cmp_country(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);

	ret = CMP(d1->record->results_set->country,
					d2->record->results_set->country);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_vendor(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = CMP(d1->record->results_set->vcode.be32,
				d2->record->results_set->vcode.be32);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_info(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = search_gui_cmp_strings(d1->record->info, d2->record->info);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}


static gint
search_gui_cmp_route(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	const struct results_set *rs1, *rs2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	rs1 = d1->record->results_set;
	rs2 = d2->record->results_set;
	ret = host_addr_cmp(rs1->last_hop, rs2->last_hop);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_hops(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	const struct results_set *rs1, *rs2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	rs1 = d1->record->results_set;
	rs2 = d2->record->results_set;
	ret = CMP(rs1->hops, rs2->hops);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_ttl(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	const struct results_set *rs1, *rs2;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	rs1 = d1->record->results_set;
	rs2 = d2->record->results_set;
	ret = CMP(rs1->ttl, rs2->ttl);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_protocol(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	const struct results_set *rs1, *rs2;
	const guint32 mask = ST_UDP;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	rs1 = d1->record->results_set;
	rs2 = d2->record->results_set;
	ret = CMP(mask & rs1->status, mask & rs2->status);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_owned(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	const struct results_set *rs1, *rs2;
	const guint32 mask = SR_OWNED | SR_SHARED;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	rs1 = d1->record->results_set;
	rs2 = d2->record->results_set;
	ret = CMP(mask & d1->record->flags, mask & d2->record->flags);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_hostile(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	const struct results_set *rs1, *rs2;
	const guint32 mask = ST_HOSTILE;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	rs1 = d1->record->results_set;
	rs2 = d2->record->results_set;
	ret = CMP(mask & rs1->status, mask & rs2->status);
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

static gint
search_gui_cmp_spam(
    GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer unused_udata)
{
	const struct result_data *d1, *d2;
	guint32 mask = SR_SPAM;
	gint ret;

	(void) unused_udata;

	d1 = get_result_data(model, a);
	d2 = get_result_data(model, b);
	ret = CMP(mask & d1->record->flags, mask & d2->record->flags);
	if (0 == ret) {
		const struct results_set *rs1, *rs2;
		rs1 = d1->record->results_set;
		rs2 = d2->record->results_set;
		mask = ST_SPAM;
		ret = CMP(mask & rs1->status, mask & rs2->status);
	}
	return 0 != ret ? ret : CMP(d1->rank, d2->rank);
}

void
search_gui_add_record(search_t *sch, record_t *rc, enum gui_color color)
{
	static const struct result_data zero_data;
	struct result_data *data;

	record_check(rc);

	data = walloc(sizeof *data);
	*data = zero_data;
	data->color = color;
	data->record = rc;
	search_gui_ref_record(rc);

	slist_append(sch->queue, data);
}

const record_t *
search_gui_get_record_at_path(GtkTreeView *tv, GtkTreePath *path)
{
	const GList *l = search_gui_get_searches();
	const struct result_data *data;
	GtkTreeModel *model;
	GtkTreeIter iter;
	search_t *sch = NULL;

	for (/* NOTHING */; NULL != l; l = g_list_next(l)) {
		const search_t *s = l->data;
		if (tv == GTK_TREE_VIEW(s->tree)) {
			sch = l->data;
			break;
		}
	}
	g_return_val_if_fail(NULL != sch, NULL);

	model = GTK_TREE_MODEL(gtk_tree_view_get_model(tv));
	if (gtk_tree_model_get_iter(model, &iter, path)) {
		data = get_result_data(model, &iter);
		return data->record;
	} else {
		return NULL;
	}
}

void
search_gui_set_clear_button_sensitive(gboolean flag)
{
	gtk_widget_set_sensitive(GTK_WIDGET(button_search_clear), flag);
}

/* ----------------------------------------- */


static void
search_gui_magnet_add_source(struct magnet_resource *magnet, record_t *record)
{
	struct results_set *rs;

	g_assert(magnet);
	g_assert(record);
	
	rs = record->results_set;

	if (ST_FIREWALL & rs->status) {
		if (rs->proxies) {
			gnet_host_t host;

			host = gnet_host_vec_get(rs->proxies, 0);
			magnet_add_sha1_source(magnet, record->sha1,
				gnet_host_get_addr(&host), gnet_host_get_port(&host),
				rs->guid);
		}
	} else {
		magnet_add_sha1_source(magnet, record->sha1, rs->addr, rs->port, NULL);
	}

	if (record->alt_locs) {
		gint i, n;

		n = gnet_host_vec_count(record->alt_locs);
		n = MIN(10, n);

		for (i = 0; i < n; i++) {
			gnet_host_t host;

			host = gnet_host_vec_get(record->alt_locs, i);
			magnet_add_sha1_source(magnet, record->sha1,
				gnet_host_get_addr(&host), gnet_host_get_port(&host), NULL);
		}
	}
}

gchar *
search_gui_get_magnet(GtkTreeModel *model, GtkTreeIter *iter)
{
	GtkTreeIter parent_iter;
	struct result_data *parent;
	struct magnet_resource *magnet;
	gchar *url;

	magnet = magnet_resource_new();

	if (gtk_tree_model_iter_parent(model, &parent_iter, iter)) {
		iter = &parent_iter;
	}
	parent = get_result_data(model, iter);

	magnet_set_display_name(magnet, parent->record->utf8_name);

	if (parent->record->sha1) {
		GtkTreeIter child;

		magnet_set_sha1(magnet, parent->record->sha1);
		magnet_set_filesize(magnet, parent->record->size);

		search_gui_magnet_add_source(magnet, parent->record);
		
		if (gtk_tree_model_iter_children(model, &child, &parent->iter)) {
			do {	
				struct result_data *data;

				data = get_result_data(model, &child);
				g_assert(data);
				search_gui_magnet_add_source(magnet, data->record);
			} while (gtk_tree_model_iter_next(model, &child));
		}
	}

	url = magnet_to_string(magnet);
	magnet_resource_free(&magnet);
	return url;
}

static void
download_selected_file(GtkTreeModel *model, GtkTreeIter *iter, GSList **sl)
{
	struct result_data *rd;

	g_assert(model != NULL);
	g_assert(iter != NULL);

	if (sl) {
		*sl = g_slist_prepend(*sl, w_tree_iter_copy(iter));
	}

	rd = get_result_data(model, iter);
	search_gui_download(rd->record);

	if (SR_DOWNLOADED & rd->record->flags) {
		rd->color = GUI_COLOR_DOWNLOADING;
		/* Re-store the parent to refresh the display/sorting */
		search_gui_set_data(model, rd);
	}
}

static void
remove_selected_file(gpointer iter_ptr, gpointer model_ptr)
{
	GtkTreeModel *model = model_ptr;
	GtkTreeIter *iter = iter_ptr;
	GtkTreeIter child;
	struct result_data *rd;
	record_t *rc;
	search_t *search = search_gui_get_current_search();

	g_assert(search->items > 0);
	search->items--;

	rd = get_result_data(model, iter);
	rc = rd->record;

	/* First get the record, it must be unreferenced at the end */
	g_assert(rc->refcount > 1);

	if (gtk_tree_model_iter_nth_child(model, &child, iter, 0)) {
		struct result_data *child_data, tmp;
		guint children;

		child_data = get_result_data(model, &child);

		/*
		 * Copy the contents of the first child's row into the parent's row
		 */

		children = rd->children;
		tmp = *rd;
		*rd = *child_data;
		*child_data = tmp;

		rd->iter = *iter;
		rd->children = children;
		atom_str_change(&rd->meta, child_data->meta);

		/* And remove the child's row */
		iter = &child;
	} else {
		/*
		 * The row has no children, it's either a child or a top-level node
		 * without children.
		 */
	}
	prepare_remove_record(model, NULL, iter, search);
	gtk_tree_store_remove(GTK_TREE_STORE(model), iter);
	w_tree_iter_free(iter_ptr);
}

struct selection_ctx {
	GtkTreeView *tv;
	GSList **iters;
};

static void
download_selected_all_files(GtkTreeModel *model, GtkTreePath *path,
		GtkTreeIter *iter, gpointer data)
{
	struct selection_ctx *ctx = data;

	g_assert(ctx);
	g_assert(iter);

	download_selected_file(model, iter, ctx->iters);
    if (!gtk_tree_view_row_expanded(ctx->tv, path)) {
        GtkTreeIter child;
        gint i = 0;

        while (gtk_tree_model_iter_nth_child(model, &child, iter, i)) {
			download_selected_file(model, &child, ctx->iters);
            i++;
        }
	}
}

static void
collect_all_iters(GtkTreeModel *model, GtkTreePath *path,
	GtkTreeIter *iter, gpointer data)
{
	struct selection_ctx *ctx = data;

	g_assert(ctx != NULL);
	g_assert(ctx->iters != NULL);

	*ctx->iters = g_slist_prepend(*ctx->iters, w_tree_iter_copy(iter));
    if (
            gtk_tree_model_iter_has_child(model, iter) &&
            !gtk_tree_view_row_expanded(ctx->tv, path)
    ) {
        GtkTreeIter child;
        gint i = 0;

        while (gtk_tree_model_iter_nth_child(model, &child, iter, i)) {
			*ctx->iters = g_slist_prepend(*ctx->iters,
								w_tree_iter_copy(&child));
            i++;
        }
	}
}

struct menu_helper {
	gint page;
	GtkTreeIter iter;
};

static gboolean
search_gui_menu_select_helper(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	struct menu_helper *mh = data;
	gpointer value;
	gint page;

	(void) path;
	gtk_tree_model_get(model, iter, 1, &value, (-1));
	page = GPOINTER_TO_INT(value);
	if (page == mh->page) {
		mh->iter = *iter;
		return TRUE;
	}
	return FALSE;
}

static void
search_gui_menu_select(gint page)
{
	GtkTreeView *treeview;
	GtkTreeModel *model;
	GtkTreeSelection *selection;
	struct menu_helper mh;

	mh.page = page;

	treeview = GTK_TREE_VIEW(gui_main_window_lookup("treeview_menu"));
	model = GTK_TREE_MODEL(gtk_tree_view_get_model(treeview));
	gtk_tree_model_foreach(model, search_gui_menu_select_helper, &mh);
	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_select_iter(selection, &mh.iter);
}

void
search_gui_download_files(void)
{
	search_t *search = search_gui_get_current_search();
	GSList *sl = NULL;
	struct selection_ctx ctx;
    gboolean clear;

	if (NULL == search)
		return;

	search_gui_start_massive_update(search);

	/* FIXME: This has to be GUI (not a core) property! */
    gnet_prop_get_boolean_val(PROP_SEARCH_REMOVE_DOWNLOADED, &clear);

	ctx.tv = GTK_TREE_VIEW(search->tree);
	ctx.iters = clear ? &sl : NULL;
	gtk_tree_selection_selected_foreach(gtk_tree_view_get_selection(ctx.tv),
		download_selected_all_files, &ctx);

	if (sl) {
		GtkTreeModel *model;

		model = gtk_tree_view_get_model(ctx.tv);
		g_slist_foreach(sl, remove_selected_file, model);
    	g_slist_free(sl);
	}

	search_gui_end_massive_update(search);

    gui_search_force_update_tab_label(search, tm_time());
    search_gui_update_items(search);
    guc_search_update_items(search->search_handle, search->items);
}


void
search_gui_discard_files(void)
{
	search_t *search = search_gui_get_current_search();
	GSList *sl = NULL;
	struct selection_ctx ctx;

	if (NULL == search)
		return;

	search_gui_start_massive_update(search);

	ctx.tv = GTK_TREE_VIEW(search->tree);
	ctx.iters = &sl;
	gtk_tree_selection_selected_foreach(gtk_tree_view_get_selection(ctx.tv),
		collect_all_iters, &ctx);

	if (sl) {
		GtkTreeModel *model;

		model = gtk_tree_view_get_model(ctx.tv);
		g_slist_foreach(sl, remove_selected_file, model);
    	g_slist_free(sl);
	}

	search_gui_end_massive_update(search);

    gui_search_force_update_tab_label(search, tm_time());
    search_gui_update_items(search);
    guc_search_update_items(search->search_handle, search->items);
}

/***
 *** Private functions
 ***/

static void
add_list_columns(GtkTreeView *tv)
{
	static const struct {
		const gchar * const title;
		const gint id;
		const gfloat align;
	} columns[] = {
		{ N_("Search"), c_sl_name, 0.0 },
		{ N_("Hits"),	c_sl_hit,  1.0 },
		{ N_("New"),	c_sl_new,  1.0 }
	};
	guint32 width[G_N_ELEMENTS(columns)];
	guint i;

	STATIC_ASSERT(SEARCH_LIST_VISIBLE_COLUMNS == G_N_ELEMENTS(columns));

    gui_prop_get_guint32(PROP_SEARCH_LIST_COL_WIDTHS, width, 0,
		G_N_ELEMENTS(width));

	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		GtkTreeViewColumn *column;
		
		column = add_column(tv, _(columns[i].title), columns[i].id,
					width[i], columns[i].align, NULL, c_sl_fg, c_sl_bg);
		gtk_tree_view_column_set_sort_column_id(column, columns[i].id);
		g_signal_connect_after(G_OBJECT(column), "clicked",
			G_CALLBACK(on_search_list_column_clicked),
			NULL);
	}
}

static void
add_results_column(
	GtkTreeView *tv,
	const gchar *name,
	gint id,
	gint width,
	gfloat xalign,
	GtkTreeIterCompareFunc sortfunc,
	gpointer udata)
{
    GtkTreeViewColumn *column;
	GtkTreeModel *model;

	model = gtk_tree_view_get_model(tv);
	column = add_column(tv, name, id, width, xalign, cell_renderer, -1, -1);
   	gtk_tree_view_column_set_sort_column_id(column, id);
	gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(model), id,
		sortfunc, NULL, NULL);
	g_signal_connect_after(G_OBJECT(column), "clicked",
		G_CALLBACK(on_tree_view_search_results_click_column),
		udata);
}

static void
search_details_treeview_init(void)
{
	static const struct {
		const gchar *title;
		gfloat xalign;
		gboolean editable;
	} tab[] = {
		{ "Item",	1.0, FALSE },
		{ "Value",	0.0, TRUE },
	};
	GtkTreeView *tv;
	GtkTreeModel *model;
	guint i;

	tv = GTK_TREE_VIEW(gui_main_window_lookup("treeview_search_details"));
	g_return_if_fail(tv);

	model = GTK_TREE_MODEL(
				gtk_list_store_new(G_N_ELEMENTS(tab),
				G_TYPE_STRING, G_TYPE_STRING));

	gtk_tree_view_set_model(tv, model);
	g_object_unref(model);

	for (i = 0; i < G_N_ELEMENTS(tab); i++) {
    	GtkTreeViewColumn *column;
		GtkCellRenderer *renderer;
		
		renderer = create_cell_renderer(tab[i].xalign);
		g_object_set(G_OBJECT(renderer),
			"editable", tab[i].editable,
			(void *) 0);
		column = gtk_tree_view_column_new_with_attributes(tab[i].title,
					renderer, "text", i, (void *) 0);
		g_object_set(column,
			"min-width", 1,
			"resizable", TRUE,
			(void *) 0);
		g_object_set(column,
			"sizing", GTK_TREE_VIEW_COLUMN_AUTOSIZE,
			(void *) 0);
    	gtk_tree_view_append_column(tv, column);
	}
}

static GtkTreeModel *
create_searches_model(void)
{
	static GType columns[c_sl_num];
	GtkListStore *store;
	guint i;

	STATIC_ASSERT(c_sl_num == G_N_ELEMENTS(columns));
#define SET(c, x) case (c): columns[i] = (x); break
	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		switch (i) {
		SET(c_sl_name, G_TYPE_STRING);
		SET(c_sl_hit, G_TYPE_INT);
		SET(c_sl_new, G_TYPE_INT);
		SET(c_sl_fg, GDK_TYPE_COLOR);
		SET(c_sl_bg, GDK_TYPE_COLOR);
		SET(c_sl_sch, G_TYPE_POINTER);
		default:
			g_assert_not_reached();
		}
	}
#undef SET

	store = gtk_list_store_newv(G_N_ELEMENTS(columns), columns);
	return GTK_TREE_MODEL(store);
}

/***
 *** Public functions
 ***/

void
search_gui_init(void)
{
    GtkTreeView *tv;
	search_t *current_search;

    tree_view_search =
		GTK_TREE_VIEW(gui_main_window_lookup("tree_view_search"));
    button_search_clear =
		GTK_BUTTON(gui_main_window_lookup("button_search_clear"));
    notebook_search_results =
		GTK_NOTEBOOK(gui_main_window_lookup("notebook_search_results"));
	gtk_notebook_popup_enable(notebook_search_results);

	search_gui_common_init();
	search_details_treeview_init();

	gtk_tree_view_set_reorderable(tree_view_search, TRUE);	
	gtk_tree_selection_set_mode(gtk_tree_view_get_selection(tree_view_search),
		GTK_SELECTION_MULTIPLE);
	g_signal_connect(GTK_OBJECT(tree_view_search), "button-press-event",
		G_CALLBACK(on_search_list_button_press_event), NULL);
	gtk_tree_view_set_model(tree_view_search, create_searches_model());
	add_list_columns(tree_view_search);
	g_signal_connect(G_OBJECT(tree_view_search), "cursor-changed",
		G_CALLBACK(on_tree_view_search_cursor_changed), NULL);
	g_signal_connect_after(G_OBJECT(gtk_tree_view_get_model(tree_view_search)),
		"row-deleted", G_CALLBACK(on_search_list_row_deleted), NULL);
	gui_search_create_tree_view(&default_scrolled_window,
		&default_search_tree_view, NULL);
	gui_color_init(GTK_WIDGET(default_search_tree_view));
    gtk_notebook_remove_page(notebook_search_results, 0);
	gtk_notebook_set_scrollable(notebook_search_results, TRUE);
	gtk_notebook_append_page(notebook_search_results,
		default_scrolled_window, NULL);
  	gtk_notebook_set_tab_label_text(notebook_search_results,
		default_scrolled_window, _("(no search)"));

	g_signal_connect(GTK_OBJECT(notebook_search_results), "switch-page",
		G_CALLBACK(on_search_notebook_switch), NULL);
	g_signal_connect(GTK_OBJECT(notebook_search_results), "focus-tab",
		G_CALLBACK(on_search_notebook_focus_tab), NULL);

   	current_search = search_gui_get_current_search();
    tv = current_search != NULL ?
			GTK_TREE_VIEW(current_search->tree) :
			GTK_TREE_VIEW(default_search_tree_view);
	tree_view_restore_visibility(tv, PROP_SEARCH_RESULTS_COL_VISIBLE);
	search_gui_retrieve_searches();
    search_add_got_results_listener(search_gui_got_results);

	{
		GtkWidget *widget;

		widget = gui_main_window_lookup("treeview_search_details");
		gtk_signal_connect(GTK_OBJECT(widget), "key-press-event",
			GTK_SIGNAL_FUNC(on_treeview_search_details_key_press_event), NULL);
		
		drag_attach(widget, search_details_get_text);
	}
}

void
search_gui_shutdown(void)
{
	GtkTreeView *tv;
	search_t *current_search = search_gui_get_current_search();
	guint32 pos;

	search_gui_shutting_down = TRUE;
	search_gui_callbacks_shutdown();
 	search_remove_got_results_listener(search_gui_got_results);
	search_gui_store_searches();

	pos = gtk_paned_get_position(
			GTK_PANED(gui_main_window_lookup("vpaned_results")));

	gui_prop_set_guint32(PROP_RESULTS_DIVIDER_POS, &pos, 0, 1);

	tv = current_search != NULL
		? GTK_TREE_VIEW(current_search->tree)
		: GTK_TREE_VIEW(default_search_tree_view);

	tree_view_save_widths(tv, PROP_SEARCH_RESULTS_COL_WIDTHS);
	tree_view_save_visibility(tv, PROP_SEARCH_RESULTS_COL_VISIBLE);

    while (searches != NULL)
        search_gui_close_search(searches->data);

	tree_view_save_widths(tree_view_search, PROP_SEARCH_LIST_COL_WIDTHS);
	search_gui_common_shutdown();
}

const GList *
search_gui_get_searches(void)
{
	return (const GList *) searches;
}

#if 0
/*
 * This is deactivated because this can be *extremely* slow with a few
 * thousand rows in sorted treeviews.
 */
static void
selection_counter_helper(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	gint *counter = data;

	(void) model;
	(void) path;
	(void) iter;
	*counter += 1;
}

static gint
selection_counter(GtkTreeView *tv)
{
	gint rows = 0;

	if (tv) {
		gtk_tree_selection_selected_foreach(gtk_tree_view_get_selection(tv),
			selection_counter_helper, &rows);
	}

	return rows;
}
#else
static gint
selection_counter(GtkTreeView *tv)
{
	g_return_val_if_fail(tv, 0);
	return 1;	/* Pretend there's at least one selected row */
}
#endif	/* 0 */

/**
 * Remove the search from the gui and update all widget accordingly.
 */
void
search_gui_remove_search(search_t *sch)
{
	GtkTreeModel *model;
    gboolean sensitive;

    g_assert(sch != NULL);

	model = gtk_tree_view_get_model(tree_view_search);

	{
		GtkTreeIter iter;

		if (tree_find_iter_by_data(model, c_sl_sch, sch, &iter))
    		gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
	}

    gtk_timeout_remove(sch->tab_updating);

    if (searches) {				/* Some other searches remain. */
		gtk_notebook_remove_page(notebook_search_results,
			gtk_notebook_page_num(notebook_search_results,
				sch->scrolled_window));
	} else {
		/*
		 * Keep the GtkTreeView of this search, clear it and make it the
		 * default GtkTreeView
		 */

		default_search_tree_view = sch->tree;
		default_scrolled_window = sch->scrolled_window;
		sch->tree = NULL;
		sch->scrolled_window = NULL;

		search_gui_forget_current_search();
		search_gui_update_items(NULL);
		search_gui_update_expiry(NULL);

        gtk_notebook_set_tab_label_text(notebook_search_results,
			default_scrolled_window, _("(no search)"));

		gtk_widget_set_sensitive(GTK_WIDGET(button_search_clear), FALSE);
	}

    sensitive = searches != NULL;
	gtk_widget_set_sensitive(
        gui_main_window_lookup("button_search_close"), sensitive);

	{
		search_t *current_search = search_gui_get_current_search();

		if (current_search != NULL)
			sensitive = sensitive &&
				selection_counter(GTK_TREE_VIEW(current_search->tree)) > 0;
	}

    gtk_widget_set_sensitive(
		gui_popup_search_lookup("popup_search_download"), sensitive);
}

void
search_gui_set_current_search(search_t *sch)
{
	search_t *old_sch = search_gui_get_current_search();
    GtkWidget *spinbutton_reissue_timeout;
    static gboolean locked = FALSE;
    gboolean active;
    gboolean frozen;
    guint32 reissue_timeout;
	search_t *current_search = old_sch;

	g_assert(sch != NULL);

    if (locked)
		return;
    locked = TRUE;

	if (old_sch)
		gui_search_force_update_tab_label(old_sch, tm_time());

    active = guc_search_is_active(sch->search_handle);
    frozen = guc_search_is_frozen(sch->search_handle);
    reissue_timeout = guc_search_get_reissue_timeout(sch->search_handle);

    /*
     * We now propagate the column visibility from the current_search
     * to the new current_search.
     */

    if (current_search != NULL) {
        GtkTreeView *tv_new = GTK_TREE_VIEW(sch->tree);
        GtkTreeView *tv_old = GTK_TREE_VIEW(current_search->tree);

		gtk_widget_hide(GTK_WIDGET(tv_old));
		g_object_freeze_notify(G_OBJECT(tv_old));
		gtk_widget_show(GTK_WIDGET(tv_new));
		g_object_thaw_notify(G_OBJECT(tv_new));
		if (tvm_search) {
			tree_view_motion_clear_callback(tv_old, tvm_search);
			tvm_search = NULL;
		}
		tree_view_save_widths(tv_old, PROP_SEARCH_RESULTS_COL_WIDTHS);
		tree_view_save_visibility(tv_old, PROP_SEARCH_RESULTS_COL_VISIBLE);
		tree_view_restore_visibility(tv_new, PROP_SEARCH_RESULTS_COL_VISIBLE);
		search_gui_clear_sorting(current_search);
    } else if (default_search_tree_view) {
		tree_view_save_widths(GTK_TREE_VIEW(default_search_tree_view),
			PROP_SEARCH_RESULTS_COL_WIDTHS);
	}


	/* This prevents side-effects otherwise caused by  changing the value of
	 * spinbutton_reissue_timeout.
	 */
	search_gui_forget_current_search();
	sch->unseen_items = 0;

    spinbutton_reissue_timeout = gui_main_window_lookup(
									"spinbutton_search_reissue_timeout");

    if (sch != NULL) {
		GtkTreeModel *model;
		GtkTreeIter iter;
		
        gui_search_force_update_tab_label(sch, tm_time());
        search_gui_update_items(sch);

        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spinbutton_reissue_timeout),
			reissue_timeout);
        gtk_widget_set_sensitive(spinbutton_reissue_timeout, active);
        gtk_widget_set_sensitive(
            gui_popup_search_lookup("popup_search_download"),
			selection_counter(GTK_TREE_VIEW(sch->tree)) > 0);
        gtk_widget_set_sensitive(GTK_WIDGET(button_search_clear),
            sch->items != 0);
        gtk_widget_set_sensitive(
            gui_popup_search_lookup("popup_search_restart"),
		   	active);
        gtk_widget_set_sensitive(
            gui_popup_search_lookup("popup_search_stop"), !frozen);
        gtk_widget_set_sensitive(
            gui_popup_search_lookup("popup_search_resume"), frozen);

		model = gtk_tree_view_get_model(tree_view_search);
		if (tree_find_iter_by_data(model, c_sl_sch, sch, &iter)) {
			GtkTreePath *path;
	
			path = gtk_tree_model_get_path(model, &iter);
			gtk_tree_view_set_cursor(tree_view_search, path, NULL, FALSE);
			gtk_tree_path_free(path);
		}
		search_gui_restore_sorting(sch);
    } else {
		static const gchar * const popup_items[] = {
			"popup_search_restart",
			"popup_search_stop",
			"popup_search_resume",
		};
		guint i;

        gtk_tree_selection_unselect_all(
			gtk_tree_view_get_selection(tree_view_search));
        gtk_widget_set_sensitive(spinbutton_reissue_timeout, FALSE);
       	gtk_widget_set_sensitive(
            gui_popup_search_lookup("popup_search_download"), FALSE);

		for (i = 0; i < G_N_ELEMENTS(popup_items); i++) {
       		gtk_widget_set_sensitive(
            	gui_popup_search_lookup(popup_items[i]), FALSE);
		}
    }
	search_gui_current_search(sch);

	tree_view_restore_widths(GTK_TREE_VIEW(sch->tree),
		PROP_SEARCH_RESULTS_COL_WIDTHS);

	tvm_search = tree_view_motion_set_callback(GTK_TREE_VIEW(sch->tree),
					search_update_tooltip, 400);

    /*
     * Search results notebook
     */
	gtk_notebook_set_current_page(notebook_search_results,
		gtk_notebook_page_num(notebook_search_results, sch->scrolled_window));

	search_gui_menu_select(nb_main_page_search);
	
	if (search_gui_update_expiry(sch))
		gui_search_set_enabled(sch, FALSE);

    locked = FALSE;
}

static GtkTreeModel *
create_results_model(void)
{
	GtkTreeStore *store;

	store = gtk_tree_store_new(1, G_TYPE_POINTER);
	return GTK_TREE_MODEL(store);
}

static void
add_results_columns(GtkTreeView *treeview, gpointer udata)
{
	static const struct {
		const gchar * const title;
		const gint id;
		const gfloat align;
		const GtkTreeIterCompareFunc func;
	} columns[] = {
		{ N_("File"),	   c_sr_filename, 0.0, search_gui_cmp_filename },
		{ N_("Extension"), c_sr_ext,	  0.0, search_gui_cmp_ext },
		{ N_("Encoding"),  c_sr_charset,  0.0, search_gui_cmp_charset },
		{ N_("Size"),	   c_sr_size,	  1.0, search_gui_cmp_size },
		{ N_("#"),		   c_sr_count,	  1.0, search_gui_cmp_count },
		{ N_("Loc"),	   c_sr_loc,	  0.0, search_gui_cmp_country },
		{ N_("Metadata"),  c_sr_meta,	  0.0, search_gui_cmp_meta },
		{ N_("Vendor"),	   c_sr_vendor,	  0.0, search_gui_cmp_vendor },
		{ N_("Info"),	   c_sr_info,	  0.0, search_gui_cmp_info },
		{ N_("Route"),	   c_sr_route,	  0.0, search_gui_cmp_route },
		{ N_("Protocol"),  c_sr_protocol, 0.0, search_gui_cmp_protocol },
		{ N_("Hops"),  	   c_sr_hops,	  0.0, search_gui_cmp_hops },
		{ N_("TTL"),  	   c_sr_ttl,	  0.0, search_gui_cmp_ttl },
		{ N_("Owned"),     c_sr_owned,	  0.0, search_gui_cmp_owned },
		{ N_("Spam"),      c_sr_spam,	  0.0, search_gui_cmp_spam },
		{ N_("Hostile"),   c_sr_hostile,  0.0, search_gui_cmp_hostile },
		{ N_("SHA-1"),     c_sr_sha1,     0.0, search_gui_cmp_sha1 },
		{ N_("Created"),   c_sr_ctime,    0.0, search_gui_cmp_ctime },
	};
	guint32 width[G_N_ELEMENTS(columns)];
	guint i;

	STATIC_ASSERT(SEARCH_RESULTS_VISIBLE_COLUMNS == G_N_ELEMENTS(columns));

    gui_prop_get_guint32(PROP_SEARCH_RESULTS_COL_WIDTHS, width, 0,
		G_N_ELEMENTS(width));
	for (i = 0; i < G_N_ELEMENTS(columns); i++) {
		add_results_column(treeview, _(columns[i].title), columns[i].id,
			width[columns[i].id], columns[i].align, columns[i].func, udata);
	}
}

static gboolean
search_by_regex(GtkTreeModel *model, gint column, const gchar *key,
	GtkTreeIter *iter, gpointer unused_data)
{
	static const gboolean found = FALSE;
	static gchar *last_key;	/* This will be "leaked" on exit */
	static regex_t re;		/* The last regex will be "leaked" on exit */
	gint ret;

	g_return_val_if_fail(model, !found);
	g_return_val_if_fail(column >= 0, !found);
	g_return_val_if_fail((guint) column < SEARCH_RESULTS_VISIBLE_COLUMNS,
		!found);
	g_return_val_if_fail(key, !found);
	g_return_val_if_fail(iter, !found);
	(void) unused_data;

	if (!last_key || 0 != strcmp(last_key, key)) {
		if (last_key) {
			regfree(&re);
			G_FREE_NULL(last_key);
		}

		ret = regcomp(&re, key, REG_EXTENDED | REG_NOSUB | REG_ICASE);
		g_return_val_if_fail(0 == ret, !found);

		last_key = g_strdup(key);
	}

	{
		const struct result_data *rd;
		
		rd = get_result_data(model, iter);
		g_return_val_if_fail(NULL != rd, !found);
		g_return_val_if_fail(NULL != rd->record->utf8_name, !found);

		ret = regexec(&re, rd->record->utf8_name, 0, NULL, 0);
	}

	return 0 == ret ? found : !found;
}

static gboolean
tree_view_search_update(
	GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
	search_t *sch;

	(void) path;
    gtk_tree_model_get(model, iter, c_sl_sch, &sch, (-1));
 	if (sch == data) {
   		GtkWidget *widget;
		GdkColor *fg;
		GdkColor *bg;

		widget = GTK_WIDGET(tree_view_search);
		if (sch->unseen_items > 0) {
#if 0
    		fg = &(gtk_widget_get_style(widget)->fg[GTK_STATE_PRELIGHT]);
    		bg = &(gtk_widget_get_style(widget)->bg[GTK_STATE_PRELIGHT]);
#endif
			fg = NULL;
			bg = NULL;
		} else if (!search_gui_is_enabled(sch)) {
    		fg = &(gtk_widget_get_style(widget)->fg[GTK_STATE_INSENSITIVE]);
    		bg = &(gtk_widget_get_style(widget)->bg[GTK_STATE_INSENSITIVE]);
		} else {
			fg = NULL;
			bg = NULL;
		}

		gtk_list_store_set(GTK_LIST_STORE(model), iter,
			c_sl_hit, sch->items,
			c_sl_new, sch->unseen_items,
			c_sl_fg, fg,
			c_sl_bg, bg,
			(-1));
		return TRUE;
	}

	return FALSE;
}

/**
 * Like search_update_tab_label but always update the label.
 */
void
gui_search_force_update_tab_label(search_t *sch, time_t now)
{
    search_t *search;
	GtkTreeModel *model;
	gchar buf[4096];

    search = search_gui_get_current_search();

	if (sch == search || sch->unseen_items == 0) {
		gm_snprintf(buf, sizeof buf, "%s\n(%d)",
			search_gui_query(sch), sch->items);
	} else {
		gm_snprintf(buf, sizeof buf, "%s\n(%d, %d)",
			search_gui_query(sch), sch->items, sch->unseen_items);
	}
	sch->last_update_items = sch->items;
	gtk_notebook_set_tab_label_text(notebook_search_results,
		sch->scrolled_window, buf);
	model = gtk_tree_view_get_model(tree_view_search);
	gtk_tree_model_foreach(model, tree_view_search_update, sch);
	sch->last_update_time = now;

}

/**
 * Doesn't update the label if nothing's changed or if the last update was
 * recent.
 */
gboolean
gui_search_update_tab_label(struct search *sch)
{
	static time_t now;

	if (sch->items != sch->last_update_items) {
		now = tm_time();

		if (delta_time(now, sch->last_update_time) >= TAB_UPDATE_TIME)
			gui_search_force_update_tab_label(sch, now);
	}

	return TRUE;
}

void
gui_search_clear_results(void)
{
	search_t *search;

	search = search_gui_get_current_search();
	search_gui_reset_search(search);
	gui_search_force_update_tab_label(search, tm_time());
	search_gui_update_items(search);
}

/**
 * Flag whether search is enabled.
 */
void
gui_search_set_enabled(struct search *sch, gboolean enabled)
{
	if (search_gui_is_enabled(sch) != enabled) {
		if (enabled)
			guc_search_start(sch->search_handle);
		else
			guc_search_stop(sch->search_handle);

		/* Marks this entry as active/inactive in the searches list. */
		gui_search_force_update_tab_label(sch, tm_time());
	}
}

/**
 * Expand all nodes in tree for current search.
 */
void
search_gui_expand_all(void)
{
	search_t *current_search = search_gui_get_current_search();
	gtk_tree_view_expand_all(GTK_TREE_VIEW(current_search->tree));
}


/**
 * Collapse all nodes in tree for current search.
 */
void
search_gui_collapse_all(void)
{
	search_t *current_search = search_gui_get_current_search();
	gtk_tree_view_collapse_all(GTK_TREE_VIEW(current_search->tree));
}

void
search_gui_start_massive_update(search_t *sch)
{
	g_assert(sch);

	g_object_freeze_notify(G_OBJECT(sch->tree));
}

void
search_gui_end_massive_update(search_t *sch)
{
	g_assert(sch);

    gui_search_force_update_tab_label(sch, tm_time());
	g_object_thaw_notify(G_OBJECT(sch->tree));
}

static void
collect_parents_with_sha1(GtkTreeModel *model, GtkTreePath *unused_path,
	GtkTreeIter *iter, gpointer data)
{
	GtkTreeIter parent_iter;
	struct result_data *rd;

	g_assert(data);
	(void) unused_path;

	if (gtk_tree_model_iter_parent(model, &parent_iter, iter)) {
		iter = &parent_iter;
	}
	rd = get_result_data(model, iter);
	if (rd->record->sha1) {
		g_hash_table_insert(data, rd, rd);
	}
}

static void
search_gui_request_bitzi_data_helper(gpointer key,
	gpointer unused_value, gpointer unused_udata)
{
	struct result_data *rd;

	(void) unused_value;
	(void) unused_udata;
	
	rd = key;
	record_check(rd->record);
	g_return_if_fail(rd->record->sha1);

	atom_str_change(&rd->meta, _("Query queued..."));
	guc_query_bitzi_by_sha1(rd->record->sha1, rd->record->size);
}

static void
search_gui_make_meta_column_visible(search_t *search)
{
	static const int min_width = 80;
	GtkTreeViewColumn *column;
	gint width;

	g_return_if_fail(search);
	g_return_if_fail(search->tree);

	column = gtk_tree_view_get_column(GTK_TREE_VIEW(search->tree), c_sr_meta);
	g_return_if_fail(column);

	gtk_tree_view_column_set_visible(column, TRUE);
	width = gtk_tree_view_column_get_width(column);
	if (width < min_width) {
		gtk_tree_view_column_set_fixed_width(column, min_width);
	}
}

void
search_gui_request_bitzi_data(void)
{
	GtkTreeSelection *selection;
	GHashTable *results;
	search_t *search;

	/* collect the list of files selected */

	search = search_gui_get_current_search();
	g_return_if_fail(search);

	g_object_freeze_notify(G_OBJECT(search->tree));

	results = g_hash_table_new(NULL, NULL);
	selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(search->tree));
	gtk_tree_selection_selected_foreach(selection,
		collect_parents_with_sha1, results);

	{
		guint32 bitzi_debug;

		gnet_prop_get_guint32_val(PROP_BITZI_DEBUG, &bitzi_debug);
		if (bitzi_debug) {
			g_message("on_search_meta_data: %u items",
					g_hash_table_size(results));
		}
	}

	g_hash_table_foreach(results, search_gui_request_bitzi_data_helper, NULL);
	g_hash_table_destroy(results);
	results = NULL;

	/* Make sure the column is actually visible. */
	search_gui_make_meta_column_visible(search);
	
	g_object_thaw_notify(G_OBJECT(search->tree));
}

/**
 * Update the search displays with the correct meta-data.
 */
void
search_gui_metadata_update(const bitzi_data_t *data)
{
	GList *iter;
	gchar *text;

	text = bitzi_gui_get_metadata(data);

	/*
	 * Fill in the columns in each search that contains a reference
	 */

	for (iter = searches; iter != NULL; iter = g_list_next(iter)) {
		struct result_data *rd;
		search_t *search;
	
		search = iter->data;
	   	rd = find_parent2(search, data->sha1, data->size);
		if (rd) {
			atom_str_change(&rd->meta, text ? text : _("Not in database"));
			
			/* Re-store the parent to refresh the display/sorting */
			search_gui_set_data(gtk_tree_view_get_model(search->tree), rd);
		}
	}

	G_FREE_NULL(text);
}

/**
 * Create a new GtkTreeView for search results.
 */
static void
gui_search_create_tree_view(GtkWidget ** sw, GtkTreeView ** tv, gpointer udata)
{
	GtkTreeModel *model = create_results_model();
	GtkTreeSelection *selection;
	GtkTreeView	*treeview;

	*sw = gtk_scrolled_window_new(NULL, NULL);

	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(*sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

	treeview = GTK_TREE_VIEW(gtk_tree_view_new_with_model(model));
	*tv = treeview;
	g_object_unref(model);

	selection = gtk_tree_view_get_selection(treeview);
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
	gtk_tree_view_set_headers_visible(treeview, TRUE);
	gtk_tree_view_set_headers_clickable(treeview, TRUE);
	gtk_tree_view_set_enable_search(treeview, TRUE);
	gtk_tree_view_set_search_column(treeview, 0);
	gtk_tree_view_set_rules_hint(treeview, TRUE);
	gtk_tree_view_set_search_equal_func(treeview, search_by_regex, NULL, NULL);

      /* add columns to the tree view */
	add_results_columns(treeview, udata);

	gtk_container_add(GTK_CONTAINER(*sw), GTK_WIDGET(*tv));

	if (!GTK_WIDGET_VISIBLE (*sw))
		gtk_widget_show_all(*sw);

	g_signal_connect(GTK_OBJECT(treeview), "cursor-changed",
		G_CALLBACK(on_tree_view_search_results_select_row), treeview);
	g_signal_connect(GTK_OBJECT(treeview), "button-press-event",
		G_CALLBACK(on_tree_view_search_results_button_press_event), NULL);
    g_signal_connect(GTK_OBJECT(treeview), "key-press-event",
		G_CALLBACK(on_tree_view_search_results_key_press_event), NULL);
    g_signal_connect(GTK_OBJECT(treeview), "leave-notify-event",
		G_CALLBACK(on_leave_notify), NULL);
	g_object_freeze_notify(G_OBJECT(treeview));
}

static void
search_gui_get_selected_searches_helper(GtkTreeModel *model,
	GtkTreePath *unused_path, GtkTreeIter *iter, gpointer data)
{
	gpointer p = NULL;

	(void) unused_path;
	g_assert(data);

	gtk_tree_model_get(model, iter, c_sl_sch, &p, (-1));
	if (p) {
		GSList **sl_ptr = data;
		*sl_ptr = g_slist_prepend(*sl_ptr, p);
	}
}

GSList *
search_gui_get_selected_searches(void)
{
	GSList *sl = NULL;
	GtkTreeView *tv;

    tv = GTK_TREE_VIEW(gui_main_window_lookup("tree_view_search"));
	gtk_tree_selection_selected_foreach(gtk_tree_view_get_selection(tv),
		search_gui_get_selected_searches_helper, &sl);
	return sl;
}

gboolean
search_gui_has_selected_item(search_t *search)
{
	GtkTreePath *path = NULL;
	gboolean ret = FALSE;

	g_return_val_if_fail(search, FALSE);

	gtk_tree_view_get_cursor(GTK_TREE_VIEW(search->tree), &path, NULL);
	if (path) {
		ret = TRUE;
		gtk_tree_path_free(path);
	}
	return ret;
}

void
search_gui_search_list_clicked(GtkWidget *widget, GdkEventButton *event)
{
	GtkTreeView *tv = GTK_TREE_VIEW(widget);
	GtkTreePath *path = NULL;
	GtkTreeViewColumn *column = NULL;

	if (
		gtk_tree_view_get_path_at_pos(tv, event->x, event->y,
			&path, &column, NULL, NULL)
	) {
		GtkTreeModel *model;
		GtkTreeIter iter;

		model = gtk_tree_view_get_model(tv);
		if (gtk_tree_model_get_iter(model, &iter, path)) {
			gpointer p = NULL; 
			gtk_tree_model_get(model, &iter, c_sl_sch, &p, (-1));
			if (p) {
				search_t *search = p;
				search_gui_set_current_search(search);
#if 0
				gtk_tree_view_set_cursor(tv, path, column, FALSE);
				gtk_widget_grab_focus(GTK_WIDGET(tv));
#endif
			}
		}
		gtk_tree_path_free(path);
	}
}

static void
search_gui_flush_queue_data(search_t *search, GtkTreeModel *model,
	struct result_data *rd)
{
	GtkTreeIter *parent_iter;
	record_t *rc;

	rc = rd->record;
	record_check(rc);

	if (rc->sha1) {
		struct result_data *parent;

		parent = find_parent(search, rd);
		parent_iter = parent ? &parent->iter : NULL;
		if (parent) {
			record_check(parent->record);
			parent->children++;
			/* Re-store the parent to refresh the display/sorting */
			search_gui_set_data(model, parent);
		} else {
			gm_hash_table_insert_const(search->parents, rd, rd);
		}
	} else {
		parent_iter = NULL;
	}

	gtk_tree_store_append(GTK_TREE_STORE(model), &rd->iter, parent_iter);
	search_gui_set_data(model, rd);

	/*
	 * There might be some metadata about this record already in the
	 * cache. If so lets update the GUI to reflect this.
	 */
	if (NULL != rc->sha1 && guc_bitzi_has_cached_ticket(rc->sha1)) {
		guc_query_bitzi_by_sha1(rc->sha1, rc->size);
	}
}

static void
search_gui_flush_queue(search_t *search)
{
	g_return_if_fail(search);
	g_return_if_fail(search->tree);
	
	if (slist_length(search->queue) > 0) {
		GtkTreeModel *model;
		slist_iter_t *iter;
		guint n = 0;

		search_gui_start_massive_update(search);

		model = gtk_tree_view_get_model(search->tree);

		iter = slist_iter_on_head(search->queue);
		while (slist_iter_has_item(iter) && n++ < 100) {
			struct result_data *data;

			data = slist_iter_current(iter);
			slist_iter_remove(iter);
			search_gui_flush_queue_data(search, model, data);
		}
		slist_iter_free(&iter);

		search_gui_end_massive_update(search);
	}
}

void
search_gui_flush_queues(void)
{
	const GList *iter;

	iter = search_gui_get_searches();
	for (/* NOTHING*/; NULL != iter; iter = g_list_next(iter)) {
		search_gui_flush_queue(iter->data);
	}
}

/* -*- mode: cc-mode; tab-width:4; -*- */
/* vi: set ts=4 sw=4 cindent: */
