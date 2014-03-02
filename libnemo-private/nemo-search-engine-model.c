/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Copyright (C) 2005 Red Hat, Inc
 *
 * Nemo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * Nemo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; see the file COPYING.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 *
 */

#include <config.h>
#include "nemo-search-hit.h"
#include "nemo-search-provider.h"
#include "nemo-search-engine-model.h"
#include "nemo-directory.h"
#include "nemo-directory-private.h"
#include "nemo-file.h"

#include <string.h>
#include <glib.h>
#include <gio/gio.h>

struct NemoSearchEngineModelDetails {
	NemoQuery *query;

	GList *hits;
	NemoDirectory *directory;

	gboolean query_pending;
};

static void nemo_search_provider_init (NemoSearchProviderIface  *iface);

G_DEFINE_TYPE_WITH_CODE (NemoSearchEngineModel,
			 nemo_search_engine_model,
			 G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (NEMO_TYPE_SEARCH_PROVIDER,
						nemo_search_provider_init))

static void
finalize (GObject *object)
{
	NemoSearchEngineModel *model;

	model = NEMO_SEARCH_ENGINE_MODEL (object);

	if (model->details->hits != NULL) {
		g_list_free_full (model->details->hits, g_object_unref);
		model->details->hits = NULL;
	}

	g_clear_object (&model->details->directory);
	g_clear_object (&model->details->query);

	G_OBJECT_CLASS (nemo_search_engine_model_parent_class)->finalize (object);
}

static gboolean
emit_finished_idle_cb (gpointer user_data)
{
	NemoSearchEngineModel *model = user_data;

	if (model->details->hits != NULL) {
		nemo_search_provider_hits_added (NEMO_SEARCH_PROVIDER (model),
						     model->details->hits);
		g_list_free_full (model->details->hits, g_object_unref);
		model->details->hits = NULL;
	}

	nemo_search_provider_finished (NEMO_SEARCH_PROVIDER (model));
	model->details->query_pending = FALSE;
	g_object_unref (model);

	return FALSE;
}

static gchar *
prepare_string_for_compare (const gchar *string)
{
	gchar *normalized, *res;

	normalized = g_utf8_normalize (string, -1, G_NORMALIZE_NFD);
	res = g_utf8_strdown (normalized, -1);
	g_free (normalized);

	return res;
}

static gchar *
prepare_pattern_for_comparison (NemoSearchEngineModel *model)
{
	gchar *text, *prepared, *pattern;

	text = nemo_query_get_text (model->details->query);
	prepared = prepare_string_for_compare (text);
	pattern = g_strdup_printf ("*%s*", prepared);

	g_free (prepared);
	g_free (text);

	return pattern;
}

static void
model_directory_ready_cb (NemoDirectory	*directory,
			  GList			*list,
			  gpointer		 user_data)
{
	NemoSearchEngineModel *model = user_data;
	gchar *uri, *pattern;
	gchar *display_name, *prepared;
	GList *files, *l, *hits;
	NemoFile *file;

	pattern = prepare_pattern_for_comparison (model);
	files = nemo_directory_get_file_list (directory);
	hits = NULL;

	for (l = files; l != NULL; l = l->next) {
		file = l->data;
		display_name = nemo_file_get_display_name (file);
		prepared = prepare_string_for_compare (display_name);

		if (g_pattern_match_simple (pattern, prepared)) {
			uri = nemo_file_get_uri (file);
			hits = g_list_prepend (hits, nemo_search_hit_new (uri));
			g_free (uri);
		}

		g_free (prepared);
		g_free (display_name);
	}

	nemo_file_list_free (files);
	model->details->hits = hits;

	emit_finished_idle_cb (model);
}

static void
nemo_search_engine_model_start (NemoSearchProvider *provider)
{
	NemoSearchEngineModel *model;

	model = NEMO_SEARCH_ENGINE_MODEL (provider);
	g_object_ref (model);

	if (model->details->query_pending) {
		return;
	}

	if (model->details->query == NULL ||
	    model->details->directory == NULL) {
		g_idle_add (emit_finished_idle_cb, model);
		return;
	}

	model->details->query_pending = TRUE;
	nemo_directory_call_when_ready (model->details->directory,
					    NEMO_FILE_ATTRIBUTE_INFO,
					    TRUE, model_directory_ready_cb, model);
}

static void
nemo_search_engine_model_stop (NemoSearchProvider *provider)
{
	NemoSearchEngineModel *model;

	model = NEMO_SEARCH_ENGINE_MODEL (provider);

	if (model->details->query_pending) {
		nemo_directory_cancel_callback (model->details->directory,
						    model_directory_ready_cb, model);
		model->details->query_pending = FALSE;
	}

	g_clear_object (&model->details->directory);
}

static void
nemo_search_engine_model_set_query (NemoSearchProvider *provider,
					 NemoQuery          *query)
{
	NemoSearchEngineModel *model;

	model = NEMO_SEARCH_ENGINE_MODEL (provider);

	if (query) {
		g_object_ref (query);
	}

	if (model->details->query) {
		g_object_unref (model->details->query);
	}

	model->details->query = query;
}

static void
nemo_search_provider_init (NemoSearchProviderIface *iface)
{
	iface->set_query = nemo_search_engine_model_set_query;
	iface->start = nemo_search_engine_model_start;
	iface->stop = nemo_search_engine_model_stop;
}

static void
nemo_search_engine_model_class_init (NemoSearchEngineModelClass *class)
{
	GObjectClass *gobject_class;

	gobject_class = G_OBJECT_CLASS (class);
	gobject_class->finalize = finalize;

	g_type_class_add_private (class, sizeof (NemoSearchEngineModelDetails));
}

static void
nemo_search_engine_model_init (NemoSearchEngineModel *engine)
{
	engine->details = G_TYPE_INSTANCE_GET_PRIVATE (engine, NEMO_TYPE_SEARCH_ENGINE_MODEL,
						       NemoSearchEngineModelDetails);
}

NemoSearchEngineModel *
nemo_search_engine_model_new (void)
{
	NemoSearchEngineModel *engine;

	engine = g_object_new (NEMO_TYPE_SEARCH_ENGINE_MODEL, NULL);

	return engine;
}

void
nemo_search_engine_model_set_model (NemoSearchEngineModel *model,
					NemoDirectory         *directory)
{
	g_clear_object (&model->details->directory);
	model->details->directory = nemo_directory_ref (directory);
}

NemoDirectory *
nemo_search_engine_model_get_model (NemoSearchEngineModel *model)
{
	return model->details->directory;
}
