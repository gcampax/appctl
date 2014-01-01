/*
   Copyright (c) 2013 Giovanni Campagna <scampa.giovanni@gmail.com>

   Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
     * Redistributions of source code must retain the above copyright
       notice, this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
     * Neither the name of the GNOME Foundation nor the
       names of its contributors may be used to endorse or promote products
       derived from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
   DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>

static void
usage (const char *argv0)
{
  g_printerr (_("Usage: %s [activate-action|quit|start] APPLICATION\n"), argv0);
}

static char *
make_object_path (const char *app_id)
{
  char *tmp;
  char *app_object_path;

  tmp = g_strdup (app_id);
  g_strdelimit (tmp, ".", '/');
  app_object_path = g_strconcat ("/", tmp, NULL);
  g_free (tmp);

  return app_object_path;
}

static void
do_dbus_call (const char *app_id,
              const char *method,
              GVariant   *parameters)
{
  GDBusConnection *session_bus;
  GError *error;
  char *app_object_path;
  GVariant *result;

  error = NULL;
  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL) {
    g_printerr (_("Failed to acquire the session bus: %s.\n"), error->message);
    exit (1);
  }

  app_object_path = make_object_path (app_id);

  result = g_dbus_connection_call_sync (session_bus,
                                        app_id,
                                        app_object_path,
                                        "org.freedesktop.Application",
                                        method,
                                        parameters,
                                        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        &error);

  g_free (app_object_path);
  g_object_unref (session_bus);

  if (result == NULL) {
    g_printerr (_("Remote application action failed: %s.\n"), error->message);
    exit (1);
  }

  g_variant_unref (result);
}

static void
send_env_var (GVariantBuilder *builder,
              const char      *key,
              const char      *var)
{
  const char *value;

  value = g_getenv (var);
  if (value)
    g_variant_builder_add (builder, "{sv}",
                           key, g_variant_new_string (value));
}

static GVariant *
build_platform_data (void)
{
  const char *xdg_platform_data;
  char *cwd;
  GVariantBuilder builder;

  xdg_platform_data = g_getenv ("XDG_PLATFORM_DATA");
  if (xdg_platform_data) {
    GError *error;
    GVariant *result;

    error = NULL;
    result = g_variant_parse (G_VARIANT_TYPE ("a{sv}"),
                              xdg_platform_data,
                              NULL, NULL,
                              &error);
    if (result == NULL)
      g_printerr (_("Warning: ignoring invalid XDG_PLATFORM_DATA.\n"));
    else
      return result;
  }

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  send_env_var (&builder, "desktop-startup-id", "DESKTOP_STARTUP_ID");
  send_env_var (&builder, "display", "DISPLAY");
  send_env_var (&builder, "wayland-display", "WAYLAND_DISPLAY");

  cwd = g_get_current_dir ();
  g_variant_builder_add (&builder, "{sv}",
                         "cwd", g_variant_new_bytestring (cwd));
  g_free (cwd);

  return g_variant_builder_end (&builder);
}

static void
do_activate_action (const char *app_id,
                    const char *action_name)
{
  do_dbus_call (app_id, "ActivateAction",
                g_variant_new ("(sav@a{sv})", action_name, NULL,
                               build_platform_data ()));
}

static void
do_quit (const char *app_id)
{
  do_activate_action (app_id, "quit");
}

static void
do_start (const char *app_id)
{
  do_dbus_call (app_id, "Activate",
                g_variant_new ("(@a{sv})", build_platform_data ()));
}

static void
do_list_apps (void)
{
  GList *apps, *iter;

  apps = g_app_info_get_all ();

  for (iter = apps; iter; iter = iter->next)
    {
      GDesktopAppInfo *app = iter->data;

      if (g_desktop_app_info_get_boolean (app, "DBusActivatable"))
        {
          char *id;

          id = g_strdup (g_app_info_get_id (G_APP_INFO (app)));
          /* remove final .desktop */
          *(strrchr (id, '.')) = 0;

          g_print ("%s\n", id);

          g_free (id);
        }
    }

  g_list_free_full (apps, g_object_unref);
}

static void
do_list_actions_off (const char *app_id)
{
  GDesktopAppInfo *app;
  char *full_app_id;

  full_app_id = g_strconcat (app_id, ".desktop", NULL);
  app = g_desktop_app_info_new (full_app_id);

  if (app) {
    const char *const *actions;
    int i;

    actions = g_desktop_app_info_list_actions (app);
    for (i = 0; actions[i]; i++)
      g_print ("%s\n", actions[i]);

    g_object_unref (app);
  }

  g_free (full_app_id);
}

static void
do_list_actions_on (GDBusConnection *session_bus,
                    const char      *app_id)
{
  GError *error;
  char *app_object_path;
  GVariant *result;

  app_object_path = make_object_path (app_id);

  error = NULL;
  result = g_dbus_connection_call_sync (session_bus,
                                        app_id,
                                        app_object_path,
                                        "org.gtk.Actions",
                                        "List",
                                        NULL,
                                        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        &error);

  g_free (app_object_path);

  if (result)
    {
      GVariantIter *iter;
      const char *action_name;

      g_variant_get (result, "(as)", &iter);

      while (g_variant_iter_loop (iter, "&s", &action_name))
        g_print ("%s\n", action_name);

      g_variant_iter_free (iter);
      g_variant_unref (result);
    }
  else
    {
      g_printerr (_("Warning: listing remote actions failed: %s.\n"), error->message);
    }
}

static void
do_list_actions (const char *app_id)
{
  GDBusConnection *session_bus;
  GError *error;
  GVariant *result;

  error = NULL;
  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  if (session_bus == NULL) {
    g_printerr (_("Failed to acquire the session bus: %s.\n"), error->message);
    exit (1);
  }

  result = g_dbus_connection_call_sync (session_bus,
                                        "org.freedesktop.DBus",
                                        "/org/freedesktop/DBus",
                                        "org.freedesktop.DBus",
                                        "GetNameOwner",
                                        g_variant_new ("(s)", app_id),
                                        NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                                        NULL);

  if (result)
    {
      do_list_actions_on (session_bus, app_id);
      g_variant_unref (result);
    }
  else
    do_list_actions_off (app_id);
}

int
main (int argc, char **argv)
{
  const char *verb;
  const char *app_id;

  setlocale (LC_ALL, "");

  if (argc < 2)
    {
      usage (argv[0]);
      exit (1);
    }

  verb = argv[1];

  if (strcmp (verb, "list-applications") != 0)
    {
      if (argc < 3)
        {
          usage (argv[0]);
          exit (1);
        }

      app_id = argv[2];

      if (!g_dbus_is_name (app_id))
        {
          g_printerr (_("Invalid application ID %s.\n"), app_id);
          exit (1);
        }
    }

  if (strcmp (verb, "activate-action") == 0)
    {
      const char *action;

      if (argc < 4)
        {
          /* TRANSLATORS: activate-action is the command, and should not be translated */
          g_printerr (_("activate-action requires an action name.\n"));
          exit (1);
        }

      action = argv[3];
      do_activate_action (app_id, action);
    }
  else if (strcmp (verb, "quit") == 0)
    do_quit (app_id);
  else if (strcmp (verb, "start") == 0 || strcmp (verb, "activate") == 0)
    do_start (app_id);
  else if (strcmp (verb, "help") == 0)
    usage (argv[0]);
  else if (strcmp (verb, "list-applications") == 0)
    do_list_apps ();
  else if (strcmp (verb, "list-actions") == 0)
    do_list_actions (app_id);
  else
    {
      g_printerr (_("Invalid command %s.\n"), verb);
      usage (argv[0]);
      exit (1);
    }

  return 0;
}
