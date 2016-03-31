/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * nemo-application: main Nemo application class.
 *
 * Copyright (C) 1999, 2000 Red Hat, Inc.
 * Copyright (C) 2000, 2001 Eazel, Inc.
 * Copyright (C) 2010, Cosimo Cecchi <cosimoc@gnome.org>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, MA 02110-1335, USA.
 *
 * Authors: Elliot Lee <sopwith@redhat.com>,
 *          Darin Adler <darin@bentspoon.com>
 *          Cosimo Cecchi <cosimoc@gnome.org>
 *
 */

#include <config.h>

#include "nemo-application.h"

#if ENABLE_EMPTY_VIEW
#include "nemo-empty-view.h"
#endif /* ENABLE_EMPTY_VIEW */

#include "nemo-desktop-icon-view.h"
#include "nemo-desktop-window.h"
#include "nemo-desktop-manager.h"
#include "nemo-freedesktop-dbus.h"
#include "nemo-icon-view.h"
#include "nemo-image-properties-page.h"
#include "nemo-list-view.h"
#include "nemo-previewer.h"
#include "nemo-progress-ui-handler.h"
#include "nemo-self-check-functions.h"
#include "nemo-window.h"
#include "nemo-window-bookmarks.h"
#include "nemo-window-manage-views.h"
#include "nemo-window-private.h"
#include "nemo-window-slot.h"
#include "nemo-statusbar.h"

#include <libnemo-private/nemo-dbus-manager.h>
#include <libnemo-private/nemo-desktop-link-monitor.h>
#include <libnemo-private/nemo-directory-private.h>
#include <libnemo-private/nemo-file-utilities.h>
#include <libnemo-private/nemo-file-operations.h>
#include <libnemo-private/nemo-global-preferences.h>
#include <libnemo-private/nemo-lib-self-check-functions.h>
#include <libnemo-private/nemo-module.h>
#include <libnemo-private/nemo-signaller.h>
#include <libnemo-private/nemo-ui-utilities.h>
#include <libnemo-private/nemo-undo-manager.h>
#include <libnemo-private/nemo-thumbnails.h>
#include <libnemo-extension/nemo-menu-provider.h>

#define DEBUG_FLAG NEMO_DEBUG_APPLICATION
#include <libnemo-private/nemo-debug.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <fcntl.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-stock-dialogs.h>
#include <libnotify/notify.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>

#ifdef HAVE_UNITY
#include "src/unity-bookmarks-handler.h"
#endif

#define GNOME_DESKTOP_USE_UNSTABLE_API

#ifdef GNOME_BUILD
#include <libgnome-desktop/gnome-desktop-thumbnail.h>
#else
#include <libcinnamon-desktop/gnome-desktop-thumbnail.h>
#endif

/* Keep window from shrinking down ridiculously small; numbers are somewhat arbitrary */
#define APPLICATION_WINDOW_MIN_WIDTH	300
#define APPLICATION_WINDOW_MIN_HEIGHT	100

#define START_STATE_CONFIG "start-state"

#define NEMO_ACCEL_MAP_SAVE_DELAY 30

static NemoApplication *singleton = NULL;

/* The saving of the accelerator map was requested  */
static gboolean save_of_accel_map_requested = FALSE;

static void     mount_removed_callback            (GVolumeMonitor            *monitor,
						   GMount                    *mount,
						   NemoApplication       *application);
static void     mount_added_callback              (GVolumeMonitor            *monitor,
						   GMount                    *mount,
						   NemoApplication       *application);

G_DEFINE_TYPE (NemoApplication, nemo_application, GTK_TYPE_APPLICATION);

struct _NemoApplicationPriv {
	GVolumeMonitor *volume_monitor;
	NemoProgressUIHandler *progress_handler;
	NemoDBusManager *dbus_manager;
	NemoFreedesktopDBus *fdb_manager;
    NemoDesktopManager *desktop_manager;

	gboolean no_desktop;
	gboolean force_desktop;
	gchar *geometry;

    gboolean cache_problem;
    gboolean ignore_cache_problem;

    NotifyNotification *unmount_notify;
};

void
nemo_application_notify_unmount_done (NemoApplication *application,
                                                  const gchar *message)
{
    if (application->priv->unmount_notify) {
        notify_notification_close (application->priv->unmount_notify, NULL);
        g_clear_object (&application->priv->unmount_notify);
    }

    if (message != NULL) {
        NotifyNotification *unplug;
        gchar **strings;

        strings = g_strsplit (message, "\n", 0);
        unplug = notify_notification_new (strings[0], strings[1],
                                          "media-removable");

        notify_notification_show (unplug, NULL);
        g_object_unref (unplug);
        g_strfreev (strings);
    }
}

void
nemo_application_notify_unmount_show (NemoApplication *application,
                                          const gchar *message)
{
    gchar **strings;

    strings = g_strsplit (message, "\n", 0);

    if (!application->priv->unmount_notify) {
        application->priv->unmount_notify =
                        notify_notification_new (strings[0], strings[1],
                                                 "media-removable");

        notify_notification_set_hint (application->priv->unmount_notify,
                                      "transient", g_variant_new_boolean (TRUE));
        notify_notification_set_urgency (application->priv->unmount_notify,
                                         NOTIFY_URGENCY_CRITICAL);
    } else {
        notify_notification_update (application->priv->unmount_notify,
                                    strings[0], strings[1],
                                    "media-removable");
    }

    notify_notification_show (application->priv->unmount_notify, NULL);
    g_strfreev (strings);
}

static gboolean
check_required_directories (NemoApplication *application)
{
	char *user_directory;
	char *desktop_directory;
	GSList *directories;
	gboolean ret;

	g_assert (NEMO_IS_APPLICATION (application));

	ret = TRUE;

	user_directory = nemo_get_user_directory ();
	desktop_directory = nemo_get_desktop_directory ();

	directories = NULL;

	if (!g_file_test (user_directory, G_FILE_TEST_IS_DIR)) {
		directories = g_slist_prepend (directories, user_directory);
	}

	if (!g_file_test (desktop_directory, G_FILE_TEST_IS_DIR)) {
		directories = g_slist_prepend (directories, desktop_directory);
	}

	if (directories != NULL) {
		int failed_count;
		GString *directories_as_string;
		GSList *l;
		char *error_string;
		const char *detail_string;
		GtkDialog *dialog;

		ret = FALSE;

		failed_count = g_slist_length (directories);

		directories_as_string = g_string_new ((const char *)directories->data);
		for (l = directories->next; l != NULL; l = l->next) {
			g_string_append_printf (directories_as_string, ", %s", (const char *)l->data);
		}

		if (failed_count == 1) {
			error_string = g_strdup_printf (_("Nemo could not create the required folder \"%s\"."),
							directories_as_string->str);
			detail_string = _("Before running Nemo, please create the following folder, or "
					  "set permissions such that Nemo can create it.");
		} else {
			error_string = g_strdup_printf (_("Nemo could not create the following required folders: "
							  "%s."), directories_as_string->str);
			detail_string = _("Before running Nemo, please create these folders, or "
					  "set permissions such that Nemo can create them.");
		}

		dialog = eel_show_error_dialog (error_string, detail_string, NULL);
		/* We need the main event loop so the user has a chance to see the dialog. */
		gtk_application_add_window (GTK_APPLICATION (application),
					    GTK_WINDOW (dialog));

		g_string_free (directories_as_string, TRUE);
		g_free (error_string);
	}

	g_slist_free (directories);
	g_free (user_directory);
	g_free (desktop_directory);

	return ret;
}

static void
menu_provider_items_updated_handler (NemoMenuProvider *provider, GtkWidget* parent_window, gpointer data)
{

	g_signal_emit_by_name (nemo_signaller_get_current (),
			       "popup_menu_changed");
}

static void
menu_provider_init_callback (void)
{
        GList *providers;
        GList *l;

        providers = nemo_module_get_extensions_for_type (NEMO_TYPE_MENU_PROVIDER);

        for (l = providers; l != NULL; l = l->next) {
                NemoMenuProvider *provider = NEMO_MENU_PROVIDER (l->data);

		g_signal_connect_after (G_OBJECT (provider), "items_updated",
                           (GCallback)menu_provider_items_updated_handler,
                           NULL);
        }

        nemo_module_extension_list_free (providers);
}

void
nemo_application_close_all_windows (NemoApplication *self)
{
	GList *list_copy;
	GList *l;
	
	list_copy = g_list_copy (gtk_application_get_windows (GTK_APPLICATION (self)));
	for (l = list_copy; l != NULL; l = l->next) {
		if (NEMO_IS_WINDOW (l->data)) {
			NemoWindow *window;

			window = NEMO_WINDOW (l->data);
			nemo_window_close (window);
		}
	}
	g_list_free (list_copy);
}

NemoWindow *
nemo_application_create_window (NemoApplication *application,
				    GdkScreen           *screen)
{
	NemoWindow *window;
	char *geometry_string;
	gboolean maximized;

	g_return_val_if_fail (NEMO_IS_APPLICATION (application), NULL);

	window = nemo_window_new (screen);
	gtk_application_add_window (GTK_APPLICATION (application),
				    GTK_WINDOW (window));

	maximized = g_settings_get_boolean
		(nemo_window_state, NEMO_WINDOW_STATE_MAXIMIZED);
	if (maximized) {
		gtk_window_maximize (GTK_WINDOW (window));
	} else {
		gtk_window_unmaximize (GTK_WINDOW (window));
	}

	geometry_string = g_settings_get_string
		(nemo_window_state, NEMO_WINDOW_STATE_GEOMETRY);
	if (geometry_string != NULL &&
	    geometry_string[0] != 0) {
		/* Ignore saved window position if a window with the same
		 * location is already showing. That way the two windows
		 * wont appear at the exact same location on the screen.
		 */
		eel_gtk_window_set_initial_geometry_from_string 
			(GTK_WINDOW (window), 
			 geometry_string,
			 NEMO_WINDOW_MIN_WIDTH,
			 NEMO_WINDOW_MIN_HEIGHT,
			 TRUE);
	}
	g_free (geometry_string);

	DEBUG ("Creating a new navigation window");
	
	return window;
}

static gboolean
window_can_be_closed (NemoWindow *window)
{
	if (!NEMO_IS_DESKTOP_WINDOW (window)) {
		return TRUE;
	}
	
	return FALSE;
}

static void
mount_added_callback (GVolumeMonitor *monitor,
		      GMount *mount,
		      NemoApplication *application)
{
	NemoDirectory *directory;
	GFile *root;
	gchar *uri;
		
	root = g_mount_get_root (mount);
	uri = g_file_get_uri (root);

	DEBUG ("Added mount at uri %s", uri);
	g_free (uri);
	
	directory = nemo_directory_get_existing (root);
	g_object_unref (root);
	if (directory != NULL) {
		nemo_directory_force_reload (directory);
		nemo_directory_unref (directory);
	}
}

/* Called whenever a mount is unmounted. Check and see if there are
 * any windows open displaying contents on the mount. If there are,
 * close them.  It would also be cool to save open window and position
 * info.
 */
static void
mount_removed_callback (GVolumeMonitor *monitor,
			GMount *mount,
			NemoApplication *application)
{
	GList *window_list, *node, *close_list;
	NemoWindow *window;
	NemoWindowSlot *slot;
	NemoWindowSlot *force_no_close_slot;
	GFile *root, *computer;
	gchar *uri;
	gint n_slots;

	close_list = NULL;
	force_no_close_slot = NULL;
	n_slots = 0;

	/* Check and see if any of the open windows are displaying contents from the unmounted mount */
	window_list = gtk_application_get_windows (GTK_APPLICATION (application));

	root = g_mount_get_root (mount);
	uri = g_file_get_uri (root);

	DEBUG ("Removed mount at uri %s", uri);
	g_free (uri);

	/* Construct a list of windows to be closed. Do not add the non-closable windows to the list. */
	for (node = window_list; node != NULL; node = node->next) {
        /* Skip blank desktop windows */
        if (!NEMO_IS_WINDOW (node->data))
            continue;

		window = NEMO_WINDOW (node->data);
		if (window != NULL && window_can_be_closed (window)) {
			GList *l;
			GList *lp;

			for (lp = window->details->panes; lp != NULL; lp = lp->next) {
				NemoWindowPane *pane;
				pane = (NemoWindowPane*) lp->data;
				for (l = pane->slots; l != NULL; l = l->next) {
					slot = l->data;
					n_slots++;
					if (nemo_window_slot_should_close_with_mount (slot, mount)) {
						close_list = g_list_prepend (close_list, slot);
					}
				} /* for all slots */
			} /* for all panes */
		}
	}

	if ((!nemo_desktop_manager_has_desktop_windows (application->priv->desktop_manager)) &&
	    (close_list != NULL) &&
	    (g_list_length (close_list) == n_slots)) {
		/* We are trying to close all open slots. Keep one navigation slot open. */
		force_no_close_slot = close_list->data;
	}

	/* Handle the windows in the close list. */
	for (node = close_list; node != NULL; node = node->next) {
		slot = node->data;

		if (slot != force_no_close_slot) {
            if (g_settings_get_boolean (nemo_preferences, NEMO_PREFERENCES_CLOSE_DEVICE_VIEW_ON_EJECT))
                nemo_window_pane_close_slot (slot->pane, slot);
            else
                nemo_window_slot_go_home (slot, FALSE);
		} else {
			computer = g_file_new_for_path (g_get_home_dir ());
			nemo_window_slot_open_location (slot, computer, 0);
			g_object_unref(computer);
		}
	}

	g_list_free (close_list);
}

static void
open_window (NemoApplication *application,
	     GFile *location, GdkScreen *screen, const char *geometry)
{
	NemoWindow *window;
	gchar *uri;

	uri = g_file_get_uri (location);
	DEBUG ("Opening new window at uri %s", uri);

	window = nemo_application_create_window (application,
						     screen);
	nemo_window_go_to (window, location);

	if (geometry != NULL && !gtk_widget_get_visible (GTK_WIDGET (window))) {
		/* never maximize windows opened from shell if a
		 * custom geometry has been requested.
		 */
		gtk_window_unmaximize (GTK_WINDOW (window));
		eel_gtk_window_set_initial_geometry_from_string (GTK_WINDOW (window),
								 geometry,
								 APPLICATION_WINDOW_MIN_WIDTH,
								 APPLICATION_WINDOW_MIN_HEIGHT,
								 FALSE);
	}

	g_free (uri);
}

static void
open_windows (NemoApplication *application,
	      GFile **files,
	      gint n_files,
	      GdkScreen *screen,
	      const char *geometry)
{
	guint i;

	if (files == NULL || files[0] == NULL) {
		/* Open a window pointing at the default location. */
		open_window (application, NULL, screen, geometry);
	} else {
		/* Open windows at each requested location. */
		for (i = 0; i < n_files; i++) {
			open_window (application, files[i], screen, geometry);
		}
	}
}

void
nemo_application_open_location (NemoApplication *application,
				    GFile *location,
				    GFile *selection,
				    const char *startup_id)
{
	NemoWindow *window;
	GList *sel_list = NULL;

	window = nemo_application_create_window (application, gdk_screen_get_default ());
	gtk_window_set_startup_id (GTK_WINDOW (window), startup_id);

	if (selection != NULL) {
		sel_list = g_list_prepend (sel_list, nemo_file_get (selection));
	}

	nemo_window_slot_open_location_full (nemo_window_get_active_slot (window), location,
						 0, sel_list, NULL, NULL);

	if (sel_list != NULL) {
		nemo_file_list_free (sel_list);
	}
}

static void
nemo_application_open (GApplication *app,
			   GFile **files,
			   gint n_files,
			   const gchar *hint)
{
	NemoApplication *self = NEMO_APPLICATION (app);

	DEBUG ("Open called on the GApplication instance; %d files", n_files);

	open_windows (self, files, n_files,
		      gdk_screen_get_default (),
		      self->priv->geometry);
}

static GObject *
nemo_application_constructor (GType type,
				  guint n_construct_params,
				  GObjectConstructParam *construct_params)
{
        GObject *retval;

        if (singleton != NULL) {
                return G_OBJECT (singleton);
        }

        retval = G_OBJECT_CLASS (nemo_application_parent_class)->constructor
                (type, n_construct_params, construct_params);

        singleton = NEMO_APPLICATION (retval);
        g_object_add_weak_pointer (retval, (gpointer) &singleton);

        return retval;
}

static void
nemo_application_init (NemoApplication *application)
{
	GSimpleAction *action;

	application->priv =
		G_TYPE_INSTANCE_GET_PRIVATE (application, NEMO_TYPE_APPLICATION,
					     NemoApplicationPriv);

    if (g_getenv("NEMO_TIME_STARTUP"))
        nemo_startup_time = g_get_monotonic_time ();

	action = g_simple_action_new ("quit", NULL);

        g_action_map_add_action (G_ACTION_MAP (application), G_ACTION (action));

	g_signal_connect_swapped (action, "activate",
				  G_CALLBACK (nemo_application_quit), application);

	g_object_unref (action);
}

static void
nemo_application_finalize (GObject *object)
{
	NemoApplication *application;

	application = NEMO_APPLICATION (object);

	nemo_bookmarks_exiting ();

	g_clear_object (&application->undo_manager);
	g_clear_object (&application->priv->volume_monitor);
	g_clear_object (&application->priv->progress_handler);

	g_free (application->priv->geometry);

	g_clear_object (&application->priv->dbus_manager);
	g_clear_object (&application->priv->fdb_manager);

    g_clear_object (&application->priv->desktop_manager);

	notify_uninit ();

        G_OBJECT_CLASS (nemo_application_parent_class)->finalize (object);
}

static gboolean
do_cmdline_sanity_checks (NemoApplication *self,
			  gboolean perform_self_check,
			  gboolean version,
			  gboolean kill_shell,
			  gchar **remaining)
{
	gboolean retval = FALSE;

	if (perform_self_check && (remaining != NULL || kill_shell)) {
		g_printerr ("%s\n",
			    _("--check cannot be used with other options."));
		goto out;
	}

	if (kill_shell && remaining != NULL) {
		g_printerr ("%s\n",
			    _("--quit cannot be used with URIs."));
		goto out;
	}

	if (self->priv->geometry != NULL &&
	    remaining != NULL && remaining[0] != NULL && remaining[1] != NULL) {
		g_printerr ("%s\n",
			    _("--geometry cannot be used with more than one URI."));
		goto out;
	}

	retval = TRUE;

 out:
	return retval;
}

static void
do_perform_self_checks (gint *exit_status)
{
#ifndef NEMO_OMIT_SELF_CHECK
	/* Run the checks (each twice) for nemo and libnemo-private. */

	nemo_run_self_checks ();
	nemo_run_lib_self_checks ();
	eel_exit_if_self_checks_failed ();

	nemo_run_self_checks ();
	nemo_run_lib_self_checks ();
	eel_exit_if_self_checks_failed ();
#endif

	*exit_status = EXIT_SUCCESS;
}

void
nemo_application_quit (NemoApplication *self)
{
	GApplication *app = G_APPLICATION (self);
	GList *windows;

	windows = gtk_application_get_windows (GTK_APPLICATION (app));
	g_list_foreach (windows, (GFunc) gtk_widget_destroy, NULL);

    /* we have been asked to force quit */
    g_application_quit (G_APPLICATION (self));
}

static gboolean
nemo_application_local_command_line (GApplication *application,
					 gchar ***arguments,
					 gint *exit_status)
{
	gboolean perform_self_check = FALSE;
	gboolean version = FALSE;
	gboolean browser = FALSE;
	gboolean kill_shell = FALSE;
	gboolean no_default_window = FALSE;
    gboolean fix_cache = FALSE;
	gchar **remaining = NULL;
	NemoApplication *self = NEMO_APPLICATION (application);

	const GOptionEntry options[] = {
#ifndef NEMO_OMIT_SELF_CHECK
		{ "check", 'c', 0, G_OPTION_ARG_NONE, &perform_self_check, 
		  N_("Perform a quick set of self-check tests."), NULL },
#endif
		/* dummy, only for compatibility reasons */
		{ "browser", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &browser,
		  NULL, NULL },
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &version,
		  N_("Show the version of the program."), NULL },
		{ "geometry", 'g', 0, G_OPTION_ARG_STRING, &self->priv->geometry,
		  N_("Create the initial window with the given geometry."), N_("GEOMETRY") },
		{ "no-default-window", 'n', 0, G_OPTION_ARG_NONE, &no_default_window,
		  N_("Only create windows for explicitly specified URIs."), NULL },
		{ "no-desktop", '\0', 0, G_OPTION_ARG_NONE, &self->priv->no_desktop,
		  N_("Never manage the desktop (ignore the GSettings preference)."), NULL },
		{ "force-desktop", '\0', 0, G_OPTION_ARG_NONE, &self->priv->force_desktop,
		  N_("Always manage the desktop (ignore the GSettings preference)."), NULL },
		{ "fix-cache", '\0', 0, G_OPTION_ARG_NONE, &fix_cache,
		  N_("Repair the user thumbnail cache - this can be useful if you're having trouble with file thumbnails.  Must be run as root"), NULL },
		{ "quit", 'q', 0, G_OPTION_ARG_NONE, &kill_shell, 
		  N_("Quit Nemo."), NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &remaining, NULL,  N_("[URI...]") },

		{ NULL }
	};
	GOptionContext *context;
	GError *error = NULL;
	gint argc = 0;
	gchar **argv = NULL;

	*exit_status = EXIT_SUCCESS;

	context = g_option_context_new (_("\n\nBrowse the file system with the file manager"));
	g_option_context_add_main_entries (context, options, NULL);
	g_option_context_add_group (context, gtk_get_option_group (TRUE));

	argv = *arguments;
	argc = g_strv_length (argv);

	if (!g_option_context_parse (context, &argc, &argv, &error)) {
		g_printerr ("Could not parse arguments: %s\n", error->message);
		g_error_free (error);

		*exit_status = EXIT_FAILURE;
		goto out;
	}

	if (version) {
		g_print ("nemo " PACKAGE_VERSION "\n");
		goto out;
	}

	if (!do_cmdline_sanity_checks (self, perform_self_check,
				       version, kill_shell, remaining)) {
		*exit_status = EXIT_FAILURE;
		goto out;
	}

	if (perform_self_check) {
		do_perform_self_checks (exit_status);
		goto out;
	}

#ifndef GNOME_BUILD
    if (fix_cache) {
        if (geteuid () != 0) {
            g_printerr ("The --fix-cache option must be run with sudo or as the root user.\n");
        } else {
            gnome_desktop_thumbnail_cache_fix_permissions ();
            g_print ("User thumbnail cache successfully repaired.\n");
        }

        goto out;
    }
#endif

	DEBUG ("Parsing local command line, no_default_window %d, quit %d, "
	       "self checks %d, no_desktop %d",
	       no_default_window, kill_shell, perform_self_check, self->priv->no_desktop);

	g_application_register (application, NULL, &error);

	if (error != NULL) {
		g_printerr ("Could not register the application: %s\n", error->message);
		g_error_free (error);

		*exit_status = EXIT_FAILURE;
		goto out;
	}

	if (kill_shell) {
		DEBUG ("Killing application, as requested");
		g_action_group_activate_action (G_ACTION_GROUP (application),
						"quit", NULL);
		goto out;
	}

	GFile **files;
	gint idx, len;

	len = 0;
	files = NULL;

	/* Convert args to GFiles */
	if (remaining != NULL) {
		GFile *file;
		GPtrArray *file_array;

		file_array = g_ptr_array_new ();

		for (idx = 0; remaining[idx] != NULL; idx++) {
			file = g_file_new_for_commandline_arg (remaining[idx]);
			if (file != NULL) {
				g_ptr_array_add (file_array, file);
			}
		}

		len = file_array->len;
		files = (GFile **) g_ptr_array_free (file_array, FALSE);
		g_strfreev (remaining);
	}

	if (files == NULL && !no_default_window) {
		files = g_malloc0 (2 * sizeof (GFile *));
		len = 1;

		files[0] = g_file_new_for_path (g_get_home_dir ());
		files[1] = NULL;
	}

	/* Invoke "Open" to create new windows */
	if (len > 0) {
		g_application_open (application, files, len, "");
	}

	for (idx = 0; idx < len; idx++) {
		g_object_unref (files[idx]);
	}
	g_free (files);

 out:
	g_option_context_free (context);

	return TRUE;	
}

gboolean 
nemo_application_get_show_desktop (NemoApplication *self) {
	if (self->priv->force_desktop) {
		return TRUE;
	} 
	if (self->priv->no_desktop) {
		return FALSE;
	}
	return g_settings_get_boolean (nemo_desktop_preferences, NEMO_PREFERENCES_SHOW_DESKTOP);
}

static gboolean
css_provider_load_from_resource (GtkCssProvider *provider,
                     const char     *resource_path,
                     GError        **error)
{
   GBytes  *data;
   gboolean retval;

   data = g_resources_lookup_data (resource_path, 0, error);
   if (!data)
       return FALSE;

   retval = gtk_css_provider_load_from_data (provider,
                         g_bytes_get_data (data, NULL),
                         g_bytes_get_size (data),
                         error);
   g_bytes_unref (data);

   return retval;
}

static void
nemo_application_add_app_css_provider (void)
{
  GtkCssProvider *provider;
  GError *error = NULL;
  GdkScreen *screen;

  provider = gtk_css_provider_new ();

  if (!css_provider_load_from_resource (provider, "/org/nemo/nemo-style-fallback.css", &error))
    {
      g_warning ("Failed to load fallback css file: %s", error->message);
      if (error->message != NULL)
        g_error_free (error);
      goto out_a;
    }

  screen = gdk_screen_get_default ();

  gtk_style_context_add_provider_for_screen (screen,
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_FALLBACK);

out_a:
  g_object_unref (provider);

  provider = gtk_css_provider_new ();

  if (!css_provider_load_from_resource (provider, "/org/nemo/nemo-style-application.css", &error))
    {
      g_warning ("Failed to load application css file: %s", error->message);
      if (error->message != NULL)
        g_error_free (error);
      goto out_b;
    }

  screen = gdk_screen_get_default ();

  gtk_style_context_add_provider_for_screen (screen,
                                             GTK_STYLE_PROVIDER (provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

out_b:
  g_object_unref (provider);
}

static void
init_icons_and_styles (void)
{
	/* initialize search path for custom icons */
	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   NEMO_DATADIR G_DIR_SEPARATOR_S "icons");

    gtk_icon_size_register (NEMO_STATUSBAR_ICON_SIZE_NAME,
                            NEMO_STATUSBAR_ICON_SIZE,
                            NEMO_STATUSBAR_ICON_SIZE);

    nemo_application_add_app_css_provider ();
}

static void
init_desktop (NemoApplication *self)
{
	/* Initialize the desktop link monitor singleton */
	nemo_desktop_link_monitor_get ();

    self->priv->desktop_manager = nemo_desktop_manager_get ();
}

static gboolean 
nemo_application_save_accel_map (gpointer data)
{
	if (save_of_accel_map_requested) {
		char *accel_map_filename;
	 	accel_map_filename = nemo_get_accel_map_file ();
	 	if (accel_map_filename) {
	 		gtk_accel_map_save (accel_map_filename);
	 		g_free (accel_map_filename);
	 	}
		save_of_accel_map_requested = FALSE;
	}

	return FALSE;
}

static void 
queue_accel_map_save_callback (GtkAccelMap *object, gchar *accel_path,
		guint accel_key, GdkModifierType accel_mods,
		gpointer user_data)
{
	if (!save_of_accel_map_requested) {
		save_of_accel_map_requested = TRUE;
		g_timeout_add_seconds (NEMO_ACCEL_MAP_SAVE_DELAY, 
				nemo_application_save_accel_map, NULL);
	}
}

static void
init_gtk_accels (void)
{
	char *accel_map_filename;

	/* load accelerator map, and register save callback */
	accel_map_filename = nemo_get_accel_map_file ();
	if (accel_map_filename) {
		gtk_accel_map_load (accel_map_filename);
		g_free (accel_map_filename);
	}

	g_signal_connect (gtk_accel_map_get (), "changed",
			  G_CALLBACK (queue_accel_map_save_callback), NULL);
}


static void
menu_state_changed_callback (NemoApplication *self)
{
    if (!g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_MENU_BAR) &&
        !g_settings_get_boolean (nemo_preferences, NEMO_PREFERENCES_DISABLE_MENU_WARNING)) {

        GtkWidget *dialog;
        GtkWidget *msg_area;
        GtkWidget *checkbox;

        dialog = gtk_message_dialog_new (NULL,
                                         GTK_DIALOG_MODAL,
                                         GTK_MESSAGE_INFO,
                                         GTK_BUTTONS_OK,
                                         _("Nemo's main menu is now hidden"));

        gchar *secondary;
        secondary = g_strdup_printf (_("You have chosen to hide the main menu.  You can get it back temporarily by:\n\n"
                                     "- Tapping the <Alt> key\n"
                                     "- Right-clicking an empty region of the main toolbar\n"
                                     "- Right-clicking an empty region of the status bar.\n\n"
                                     "You can restore it permanently by selecting this option again from the View menu."));
        g_object_set (dialog,
                      "secondary-text", secondary,
                      NULL);
        g_free (secondary);

        msg_area = gtk_message_dialog_get_message_area (GTK_MESSAGE_DIALOG (dialog));
        checkbox = gtk_check_button_new_with_label (_("Don't show this message again."));
        gtk_box_pack_start (GTK_BOX (msg_area), checkbox, TRUE, TRUE, 2);

        g_settings_bind (nemo_preferences,
                         NEMO_PREFERENCES_DISABLE_MENU_WARNING,
                         checkbox,
                         "active",
                         G_SETTINGS_BIND_DEFAULT);

        gtk_widget_show_all (dialog);

        g_signal_connect (dialog, "response",
                          G_CALLBACK (gtk_widget_destroy), NULL);
    }

}

static void
nemo_application_startup (GApplication *app)
{
	NemoApplication *self = NEMO_APPLICATION (app);
	/* chain up to the GTK+ implementation early, so gtk_init()
	 * is called for us.
	 */
	G_APPLICATION_CLASS (nemo_application_parent_class)->startup (app);

	/* initialize the previewer singleton */
	//nemo_previewer_get_singleton ();

	/* create an undo manager */
	self->undo_manager = nemo_undo_manager_new ();

	/* create DBus manager */
	self->priv->dbus_manager = nemo_dbus_manager_new ();
	self->priv->fdb_manager = nemo_freedesktop_dbus_new ();

	/* initialize preferences and create the global GSettings objects */
	nemo_global_preferences_init ();

	/* register views */
	nemo_icon_view_register ();
	nemo_desktop_icon_view_register ();
	nemo_list_view_register ();
	nemo_icon_view_compact_register ();
#if ENABLE_EMPTY_VIEW
	nemo_empty_view_register ();
#endif

	/* register property pages */
	nemo_image_properties_page_register ();

	/* initialize theming */
	init_icons_and_styles ();
	init_gtk_accels ();
	
	/* initialize nemo modules */
	nemo_module_setup ();

	/* attach menu-provider module callback */
	menu_provider_init_callback ();
	
	/* Initialize the UI handler singleton for file operations */
	notify_init (GETTEXT_PACKAGE);
	self->priv->progress_handler = nemo_progress_ui_handler_new ();

	/* Watch for unmounts so we can close open windows */
	/* TODO-gio: This should be using the UNMOUNTED feature of GFileMonitor instead */
	self->priv->volume_monitor = g_volume_monitor_get ();
	g_signal_connect_object (self->priv->volume_monitor, "mount_removed",
				 G_CALLBACK (mount_removed_callback), self, 0);
	g_signal_connect_object (self->priv->volume_monitor, "mount_added",
				 G_CALLBACK (mount_added_callback), self, 0);

    g_signal_connect_swapped (nemo_window_state, "changed::" NEMO_WINDOW_STATE_START_WITH_MENU_BAR,
                              G_CALLBACK (menu_state_changed_callback), self);

	/* Check the user's ~/.nemo directories and post warnings
	 * if there are problems.
	 */
	check_required_directories (self);

    self->priv->cache_problem = FALSE;
    self->priv->ignore_cache_problem = FALSE;

#ifndef GNOME_BUILD
    /* silently do a full check of the cache and fix if running as root.
     * If running as a normal user, do a quick check, and we'll notify the
     * user later if there's a problem via an infobar */
    if (geteuid () == 0) {
        if (!gnome_desktop_thumbnail_cache_check_permissions (NULL, FALSE))
            gnome_desktop_thumbnail_cache_fix_permissions ();
    } else {
        if (!gnome_desktop_thumbnail_cache_check_permissions (NULL, TRUE))
            self->priv->cache_problem = TRUE;
    }
#endif

    if (geteuid() != 0)
        init_desktop (self);

#ifdef HAVE_UNITY
	unity_bookmarks_handler_initialize ();
#endif
}

static void
nemo_application_quit_mainloop (GApplication *app)
{
	DEBUG ("Quitting mainloop");

	nemo_icon_info_clear_caches ();
 	nemo_application_save_accel_map (NULL);

    nemo_application_notify_unmount_done (NEMO_APPLICATION (app), NULL);

	G_APPLICATION_CLASS (nemo_application_parent_class)->quit_mainloop (app);
}

static void
nemo_application_window_removed (GtkApplication *app,
				     GtkWindow *window)
{
	NemoPreviewer *previewer;

	/* chain to parent */
	GTK_APPLICATION_CLASS (nemo_application_parent_class)->window_removed (app, window);

	/* if this was the last window, close the previewer */
	if (g_list_length (gtk_application_get_windows (app)) == 0) {
		previewer = nemo_previewer_get_singleton ();
		nemo_previewer_call_close (previewer);
	}
}

static void
nemo_application_class_init (NemoApplicationClass *class)
{
        GObjectClass *object_class;
	GApplicationClass *application_class;
	GtkApplicationClass *gtkapp_class;

        object_class = G_OBJECT_CLASS (class);
	object_class->constructor = nemo_application_constructor;
        object_class->finalize = nemo_application_finalize;

	application_class = G_APPLICATION_CLASS (class);
	application_class->startup = nemo_application_startup;
	application_class->quit_mainloop = nemo_application_quit_mainloop;
	application_class->open = nemo_application_open;
	application_class->local_command_line = nemo_application_local_command_line;

	gtkapp_class = GTK_APPLICATION_CLASS (class);
	gtkapp_class->window_removed = nemo_application_window_removed;

	g_type_class_add_private (class, sizeof (NemoApplicationPriv));
}

NemoApplication *
nemo_application_get_singleton (void)
{
	return g_object_new (NEMO_TYPE_APPLICATION,
                         "application-id", "org.Nemo",
                         "flags", G_APPLICATION_HANDLES_OPEN,
                         "inactivity-timeout", 12000,
                         "register-session", TRUE,
                         NULL);
}

void
nemo_application_check_thumbnail_cache (NemoApplication *application)
{
    application->priv->cache_problem = !nemo_thumbnail_factory_check_status ();
}

gboolean
nemo_application_get_cache_bad (NemoApplication *application)
{
    return application->priv->cache_problem;
}

void
nemo_application_clear_cache_flag (NemoApplication *application)
{
    application->priv->cache_problem = FALSE;
}

void
nemo_application_set_cache_flag (NemoApplication *application)
{
    application->priv->cache_problem = TRUE;
}

void
nemo_application_ignore_cache_problem (NemoApplication *application)
{
    application->priv->ignore_cache_problem = TRUE;
}

gboolean
nemo_application_get_cache_problem_ignored (NemoApplication *application)
{
    return application->priv->ignore_cache_problem;
}
