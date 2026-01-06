/*
 * Remote display factory implementation for Deepin specific sessions.
 */

#include <config.h>

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "remote-display-factory.h"

#include "seat-config.h"
#include "seat.h"
#include "greeter-session.h"

#define REMOTE_DISPLAY_FACTORY_BUS_NAME "org.deepin.DisplayManager"
#define REMOTE_DISPLAY_FACTORY_OBJECT_PATH "/org/deepin/DisplayManager/RemoteDisplayFactory"
#define REMOTE_DISPLAY_SESSION_OBJECT_PREFIX "/org/deepin/DisplayManager/RemoteDisplayFactory/Sessions"
#define REMOTE_DISPLAY_X_COMMAND "/usr/bin/Xorg"
#define REMOTE_DISPLAY_DEFAULT_DISPLAY_NUMBER 99
#define REMOTE_DISPLAY_CONFIG_DIR RUN_DIR "/remote-displays"
#define REMOTE_DISPLAY_FACTORY_INTERFACE_NAME "org.deepin.DisplayManager.RemoteDisplayFactory"
#define REMOTE_DISPLAY_SESSION_INTERFACE_NAME "org.deepin.DisplayManager.RemoteDisplayFactory.Session"

typedef enum
{
    REMOTE_DISPLAY_SESSION_MODE_GREETER = 0,
    REMOTE_DISPLAY_SESSION_MODE_SINGLE_LOGON
} RemoteDisplaySessionMode;

typedef struct
{
    guint8 remote_id;
    guint32 width;
    guint32 height;
    gchar *user_name;
    gchar *address;
    gchar *object_path;
    guint registration_id;
    RemoteDisplaySessionMode mode;

    Seat *seat;
    gchar *config_path;
    guint display_number;
    gulong seat_stopped_handler;
    gulong seat_session_added_handler;
    gulong seat_session_removed_handler;
    gchar *session_id;
} RemoteDisplaySession;

static DisplayManager *remote_display_manager = NULL;
static guint remote_display_factory_bus_id = 0;
static GDBusConnection *remote_display_connection = NULL;
static guint remote_display_factory_reg_id = 0;
static GHashTable *remote_display_sessions = NULL;
static GHashTable *remote_display_numbers_in_use = NULL;
static guint remote_display_session_index = 0;
static guint remote_display_next_display_number = REMOTE_DISPLAY_DEFAULT_DISPLAY_NUMBER;
static GDBusNodeInfo *remote_display_dbus_info = NULL;

static const gchar remote_display_dbus_xml[] =
"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
"<node>"
"  <interface name=\"org.deepin.DisplayManager.RemoteDisplayFactory\">"
"    <method name=\"CreateRemoteGreeterDisplay\">"
"      <arg name=\"remote_id\" direction=\"in\" type=\"u\"/>"
"      <arg name=\"width\" direction=\"in\" type=\"u\"/>"
"      <arg name=\"height\" direction=\"in\" type=\"u\"/>"
"      <arg name=\"address\" direction=\"in\" type=\"s\"/>"
"      <arg name=\"session\" direction=\"out\" type=\"o\"/>"
"    </method>"
"    <method name=\"CreateSingleLogonSession\">"
"      <arg name=\"remote_id\" direction=\"in\" type=\"y\"/>"
"      <arg name=\"width\" direction=\"in\" type=\"u\"/>"
"      <arg name=\"height\" direction=\"in\" type=\"u\"/>"
"      <arg name=\"user_name\" direction=\"in\" type=\"s\"/>"
"      <arg name=\"address\" direction=\"in\" type=\"s\"/>"
"      <arg name=\"session\" direction=\"out\" type=\"o\"/>"
"    </method>"
"  </interface>"
"  <interface name=\"org.deepin.DisplayManager.RemoteDisplayFactory.Session\">"
"    <property name=\"UserName\" type=\"s\" access=\"read\"/>"
"    <property name=\"Address\" type=\"s\" access=\"read\"/>"
"    <property name=\"SessionId\" type=\"s\" access=\"read\"/>"
"  </interface>"
"</node>";

static const GDBusInterfaceVTable remote_display_factory_vtable;
static const GDBusInterfaceVTable remote_display_session_vtable;

static RemoteDisplaySession *remote_display_session_create (guint32 remote_id,
                                                            guint32 width,
                                                            guint32 height,
                                                            const gchar *user_name,
                                                            const gchar *address,
                                                            RemoteDisplaySessionMode mode,
                                                            GError **error);
static void remote_display_session_free (RemoteDisplaySession *session);

static gpointer
remote_display_session_key (guint32 remote_id)
{
    return GUINT_TO_POINTER ((guint) remote_id);
}

static GDBusInterfaceInfo *
remote_display_lookup_interface (const gchar *interface_name)
{
    if (!remote_display_dbus_info)
    {
        g_autoptr(GError) parse_error = NULL;
        remote_display_dbus_info = g_dbus_node_info_new_for_xml (remote_display_dbus_xml, &parse_error);
        if (!remote_display_dbus_info)
        {
            const gchar *message = parse_error ? parse_error->message : "unknown error";
            g_error ("Failed to parse remote display D-Bus XML: %s", message);
        }
    }

    GDBusInterfaceInfo *info = g_dbus_node_info_lookup_interface (remote_display_dbus_info, interface_name);
    if (!info)
        g_error ("Missing interface %s in remote display D-Bus XML", interface_name);

    return info;
}

static GDBusInterfaceInfo *
remote_display_session_interface_info (void)
{
    return remote_display_lookup_interface (REMOTE_DISPLAY_SESSION_INTERFACE_NAME);
}

static GDBusInterfaceInfo *
remote_display_factory_interface_info (void)
{
    return remote_display_lookup_interface (REMOTE_DISPLAY_FACTORY_INTERFACE_NAME);
}

static guint
remote_display_allocate_display_number (void)
{
    if (!remote_display_numbers_in_use)
        remote_display_numbers_in_use = g_hash_table_new (g_direct_hash, g_direct_equal);

    guint number = remote_display_next_display_number;
    while (g_hash_table_lookup (remote_display_numbers_in_use, GUINT_TO_POINTER (number)))
        number++;

    remote_display_next_display_number = number + 1;
    g_hash_table_insert (remote_display_numbers_in_use, GUINT_TO_POINTER (number), GUINT_TO_POINTER (1));
    return number;
}

static void
remote_display_release_display_number (guint number)
{
    if (!remote_display_numbers_in_use || number == 0)
        return;

    g_hash_table_remove (remote_display_numbers_in_use, GUINT_TO_POINTER (number));
    if (number < remote_display_next_display_number)
        remote_display_next_display_number = number;

    if (g_hash_table_size (remote_display_numbers_in_use) == 0)
    {
        g_hash_table_unref (remote_display_numbers_in_use);
        remote_display_numbers_in_use = NULL;
        remote_display_next_display_number = REMOTE_DISPLAY_DEFAULT_DISPLAY_NUMBER;
    }
}

static gboolean
remote_display_session_register_object (RemoteDisplaySession *session, GError **error)
{
    g_return_val_if_fail (session != NULL, FALSE);
    g_return_val_if_fail (remote_display_connection != NULL, FALSE);

    g_autofree gchar *path = g_strdup_printf ("%s/%u", REMOTE_DISPLAY_SESSION_OBJECT_PREFIX, ++remote_display_session_index);
    session->object_path = g_steal_pointer (&path);
    session->registration_id = g_dbus_connection_register_object (remote_display_connection,
                                                                  session->object_path,
                                                                  remote_display_session_interface_info (),
                                                                  &remote_display_session_vtable,
                                                                  session,
                                                                  NULL,
                                                                  error);
    return session->registration_id != 0;
}

static gchar *
remote_display_session_build_config (RemoteDisplaySession *session)
{
    (void) g_mkdir_with_parents (REMOTE_DISPLAY_CONFIG_DIR, 0755);
    gdouble h_total = session->width + 656.0;
    gdouble v_total = session->height + 40.0;
    gdouble clock = (h_total * v_total * 60.0) / 1000000.0;

    guint h1 = session->width + 128;
    guint h2 = session->width + 328;
    guint h3 = session->width + 656;
    guint v1 = session->height + 3;
    guint v2 = session->height + 8;
    guint v3 = session->height + 40;

    g_autofree gchar *contents = g_strdup_printf (
        "Section \"Device\"\n"
        "  Identifier \"dummy_device\"\n"
        "  Driver     \"dummy\"\n"
        "  VideoRam   256000\n"
        "EndSection\n\n"
        "Section \"Monitor\"\n"
        "  Identifier \"dummy_monitor\"\n"
        "  HorizSync 30-80\n"
        "  VertRefresh 60\n"
        "  Modeline \"%ux%u\"  %.2f  %u %u %u %u %u %u %u %u -hsync +vsync\n"
        "EndSection\n\n"
        "Section \"Screen\"\n"
        "  Identifier \"dummy_screen\"\n"
        "  Device     \"dummy_device\"\n"
        "  Monitor    \"dummy_monitor\"\n"
        "  DefaultDepth 24\n"
        "  SubSection \"Display\"\n"
        "    Depth 24\n"
        "    Modes \"%ux%u\"\n"
        "  EndSubSection\n"
        "EndSection\n",
        session->width,
        session->height,
        clock,
        session->width,
        h1,
        h2,
        h3,
        session->height,
        v1,
        v2,
        v3,
        session->width,
        session->height);

    g_autofree gchar *path = g_strdup_printf ("%s/session-%u.conf", REMOTE_DISPLAY_CONFIG_DIR, session->remote_id);
    if (!g_file_set_contents (path, contents, -1, NULL))
    {
        g_warning ("Failed to write remote display configuration to %s", path);
        return NULL;
    }

    return g_steal_pointer (&path);
}


static void
remote_display_session_register (RemoteDisplaySession *session)
{
    if (!remote_display_sessions)
        remote_display_sessions = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) remote_display_session_free);

    g_hash_table_insert (remote_display_sessions, remote_display_session_key (session->remote_id), session);
}

static void
remote_display_session_cleanup_config (RemoteDisplaySession *session)
{
    if (session->config_path)
    {
        // g_remove (session->config_path);
        g_clear_pointer (&session->config_path, g_free);
    }
}

static void
remote_display_session_emit_session_id_changed (RemoteDisplaySession *session)
{
    g_return_if_fail (session != NULL);

    if (!remote_display_connection || !session->object_path)
        return;

    GVariantBuilder changed_builder;
    g_variant_builder_init (&changed_builder, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&changed_builder,
                           "{sv}",
                           "SessionId",
                           g_variant_new_string (session->session_id ? session->session_id : ""));
    g_autoptr(GVariant) changed = g_variant_builder_end (&changed_builder);

    GVariantBuilder invalidated_builder;
    g_variant_builder_init (&invalidated_builder, G_VARIANT_TYPE ("as"));
    g_autoptr(GVariant) invalidated = g_variant_builder_end (&invalidated_builder);

    g_dbus_connection_emit_signal (remote_display_connection,
                                   NULL,
                                   session->object_path,
                                   "org.freedesktop.DBus.Properties",
                                   "PropertiesChanged",
                                   g_variant_new ("(sa{sv}as)",
                                                  REMOTE_DISPLAY_SESSION_INTERFACE_NAME,
                                                  g_steal_pointer (&changed),
                                                  g_steal_pointer (&invalidated)),
                                   NULL);
}

static void
remote_display_session_set_session_id (RemoteDisplaySession *session, const gchar *new_id)
{
    g_return_if_fail (session != NULL);

    const gchar *value = (new_id && new_id[0]) ? new_id : "";
    if (g_strcmp0 (session->session_id, value) == 0)
        return;

    g_free (session->session_id);
    session->session_id = g_strdup (value);
    remote_display_session_emit_session_id_changed (session);
}

static const gchar *
remote_display_session_select_login1_id (RemoteDisplaySession *session)
{
    if (!session->seat)
        return NULL;

    GList *sessions = seat_get_sessions (session->seat);

    for (GList *link = sessions; link; link = link->next)
    {
        Session *seat_session = link->data;
        if (session_get_is_stopping (seat_session) || IS_GREETER_SESSION (seat_session))
            continue;
        const gchar *login1_id = session_get_login1_session_id (seat_session);
        if (login1_id && login1_id[0])
            return login1_id;
    }

    for (GList *link = sessions; link; link = link->next)
    {
        Session *seat_session = link->data;
        if (session_get_is_stopping (seat_session) || !IS_GREETER_SESSION (seat_session))
            continue;
        const gchar *login1_id = session_get_login1_session_id (seat_session);
        if (login1_id && login1_id[0])
            return login1_id;
    }

    return NULL;
}

static void
remote_display_session_refresh_session_id (RemoteDisplaySession *session)
{
    g_return_if_fail (session != NULL);
    const gchar *login1_id = remote_display_session_select_login1_id (session);
    remote_display_session_set_session_id (session, login1_id);
}

static void
remote_display_session_session_login1_notify_cb (Session *seat_session,
                                                 G_GNUC_UNUSED GParamSpec *pspec,
                                                 RemoteDisplaySession *session)
{
    (void) seat_session;
    remote_display_session_refresh_session_id (session);
}

static void
remote_display_session_session_stopped_cb (Session *seat_session, RemoteDisplaySession *session)
{
    g_signal_handlers_disconnect_matched (seat_session, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, session);
    remote_display_session_refresh_session_id (session);
}

static void
remote_display_session_watch_session (RemoteDisplaySession *session, Session *seat_session)
{
    g_signal_connect (seat_session,
                      "notify::login1-session-id",
                      G_CALLBACK (remote_display_session_session_login1_notify_cb),
                      session);
    g_signal_connect (seat_session,
                      SESSION_SIGNAL_STOPPED,
                      G_CALLBACK (remote_display_session_session_stopped_cb),
                      session);
}

static void
remote_display_session_watch_existing_sessions (RemoteDisplaySession *session)
{
    if (!session->seat)
        return;

    for (GList *link = seat_get_sessions (session->seat); link; link = link->next)
        remote_display_session_watch_session (session, link->data);
}

static void
remote_display_session_unwatch_sessions (RemoteDisplaySession *session)
{
    if (!session->seat)
        return;

    for (GList *link = seat_get_sessions (session->seat); link; link = link->next)
        g_signal_handlers_disconnect_matched (link->data, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, session);
}

static void
remote_display_session_seat_session_added_cb (Seat *seat,
                                              Session *seat_session,
                                              RemoteDisplaySession *session)
{
    (void) seat;
    remote_display_session_watch_session (session, seat_session);
    remote_display_session_refresh_session_id (session);
}

static void
remote_display_session_seat_session_removed_cb (Seat *seat,
                                                G_GNUC_UNUSED Session *seat_session,
                                                RemoteDisplaySession *session)
{
    (void) seat;
    remote_display_session_refresh_session_id (session);
}

static void
remote_display_session_free (RemoteDisplaySession *session)
{
    if (!session)
        return;

    if (session->registration_id && remote_display_connection)
        g_dbus_connection_unregister_object (remote_display_connection, session->registration_id);

    if (session->seat)
    {
        if (session->seat_stopped_handler)
            g_signal_handler_disconnect (session->seat, session->seat_stopped_handler);
        if (session->seat_session_added_handler)
            g_signal_handler_disconnect (session->seat, session->seat_session_added_handler);
        if (session->seat_session_removed_handler)
            g_signal_handler_disconnect (session->seat, session->seat_session_removed_handler);
        remote_display_session_unwatch_sessions (session);
        if (!seat_get_is_stopping (session->seat))
            seat_stop (session->seat);
        g_clear_object (&session->seat);
    }

    remote_display_session_cleanup_config (session);
    remote_display_release_display_number (session->display_number);

    g_clear_pointer (&session->user_name, g_free);
    g_clear_pointer (&session->address, g_free);
    g_clear_pointer (&session->session_id, g_free);
    g_clear_pointer (&session->object_path, g_free);
    g_free (session);
}

static void
remote_display_session_seat_stopped_cb (Seat *seat, RemoteDisplaySession *session)
{
    (void) seat;
    if (remote_display_sessions)
        g_hash_table_remove (remote_display_sessions, remote_display_session_key (session->remote_id));
}

static gboolean
remote_display_session_configure_seat (RemoteDisplaySession *session, Seat *seat, GError **error)
{
    g_autofree gchar *seat_name = g_strdup_printf ("remote-seat-%u", session->display_number);
    seat_set_name (seat, seat_name);
    seat_config_apply (seat, NULL);
    seat_set_property (seat, "allow-user-switching", "false");
    seat_set_property (seat, "xserver-share", "true");

    g_autofree gchar *display_str = g_strdup_printf ("%u", session->display_number);
    g_autofree gchar *width_str = g_strdup_printf ("%u", session->width);
    g_autofree gchar *height_str = g_strdup_printf ("%u", session->height);
    g_autofree gchar *remote_id_str = g_strdup_printf ("%u", session->remote_id);

    seat_set_property (seat, "xserver-command", REMOTE_DISPLAY_X_COMMAND);
    seat_set_property (seat, "xserver-config", session->config_path);
    seat_set_property (seat, "xserver-display-number", display_str);
    seat_set_property (seat, "remote-id", remote_id_str);
    seat_set_property (seat, "remote-address", session->address ? session->address : "");
    seat_set_property (seat, "remote-mode", session->mode == REMOTE_DISPLAY_SESSION_MODE_GREETER ? "greeter" : "single-logon");
    seat_set_property (seat, "remote-width", width_str);
    seat_set_property (seat, "remote-height", height_str);
    if (session->user_name)
        seat_set_property (seat, "remote-user", session->user_name);

    if (session->mode == REMOTE_DISPLAY_SESSION_MODE_SINGLE_LOGON)
    {
        seat_set_property (seat, "autologin-user", session->user_name);
        seat_set_property (seat, "autologin-user-timeout", "0");
        seat_set_property (seat, "autologin-in-background", "false");
        seat_set_property (seat, "autologin-guest", "false");
    }

    if (!display_manager_add_seat (remote_display_manager, seat))
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Failed to start remote seat");
        return FALSE;
    }

    session->seat = g_object_ref (seat);
    session->seat_stopped_handler = g_signal_connect (seat, SEAT_SIGNAL_STOPPED, G_CALLBACK (remote_display_session_seat_stopped_cb), session);
    session->seat_session_added_handler = g_signal_connect (seat, SEAT_SIGNAL_SESSION_ADDED, G_CALLBACK (remote_display_session_seat_session_added_cb), session);
    session->seat_session_removed_handler = g_signal_connect (seat, SEAT_SIGNAL_SESSION_REMOVED, G_CALLBACK (remote_display_session_seat_session_removed_cb), session);
    remote_display_session_watch_existing_sessions (session);
    remote_display_session_refresh_session_id (session);
    return TRUE;
}

static RemoteDisplaySession *
remote_display_session_create (guint32 remote_id,
                               guint32 width,
                               guint32 height,
                               const gchar *user_name,
                               const gchar *address,
                               RemoteDisplaySessionMode mode,
                               GError **error)
{
    if (!remote_display_connection)
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Remote display bus connection not ready");
        return NULL;
    }
    if (!remote_display_manager)
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Display manager not initialized");
        return NULL;
    }
    if (remote_display_sessions && g_hash_table_lookup (remote_display_sessions, remote_display_session_key (remote_id)))
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Remote ID %u already in use", remote_id);
        return NULL;
    }
    if (width == 0 || height == 0)
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Width and height must be greater than zero");
        return NULL;
    }
    if (mode == REMOTE_DISPLAY_SESSION_MODE_SINGLE_LOGON && (!user_name || user_name[0] == '\0'))
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "User name required for single logon session");
        return NULL;
    }

    RemoteDisplaySession *session = g_new0 (RemoteDisplaySession, 1);
    session->remote_id = remote_id;
    session->width = width;
    session->height = height;
    session->user_name = g_strdup (user_name);
    session->address = g_strdup (address);
    session->mode = mode;
    session->display_number = remote_display_allocate_display_number ();
    session->session_id = g_strdup ("");

    g_autoptr(GError) register_error = NULL;
    if (!remote_display_session_register_object (session, &register_error))
    {
        const gchar *message = register_error ? register_error->message : "unknown error";
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Failed to export remote session object: %s", message);
        remote_display_session_free (session);
        return NULL;
    }

    // session->config_path = remote_display_session_build_config (session); // TODO
    session->config_path = g_strdup("/etc/X11/lcz/10-dummy.conf");
    if (!session->config_path)
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Failed to prepare Xorg configuration");
        remote_display_session_free (session);
        return NULL;
    }

    g_autoptr(Seat) seat = seat_new ("rdp");
    if (!seat)
    {
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Failed to create seat object");
        remote_display_session_free (session);
        return NULL;
    }

    if (!remote_display_session_configure_seat (session, seat, error))
    {
        remote_display_session_free (session);
        return NULL;
    }

    remote_display_session_register (session);
    return session;
}

static GVariant *
remote_display_session_get_property (G_GNUC_UNUSED GDBusConnection *connection,
                                     G_GNUC_UNUSED const gchar *sender,
                                     G_GNUC_UNUSED const gchar *object_path,
                                     G_GNUC_UNUSED const gchar *interface_name,
                                     const gchar *property_name,
                                     GError **error,
                                     gpointer user_data)
{
    RemoteDisplaySession *session = user_data;

    if (g_strcmp0 (property_name, "UserName") == 0)
        return g_variant_new_string (session->user_name ? session->user_name : "");
    if (g_strcmp0 (property_name, "Address") == 0)
        return g_variant_new_string (session->address ? session->address : "");
    if (g_strcmp0 (property_name, "SessionId") == 0)
        return g_variant_new_string (session->session_id ? session->session_id : "");

    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                 "Unknown property %s", property_name);
    return NULL;
}

static const GDBusInterfaceVTable remote_display_session_vtable =
{
    NULL,
    remote_display_session_get_property,
    NULL
};

static void
remote_display_factory_call (G_GNUC_UNUSED GDBusConnection *connection,
                             G_GNUC_UNUSED const gchar *sender,
                             G_GNUC_UNUSED const gchar *object_path,
                             G_GNUC_UNUSED const gchar *interface_name,
                             const gchar *method_name,
                             GVariant *parameters,
                             GDBusMethodInvocation *invocation,
                             G_GNUC_UNUSED gpointer user_data)
{
    if (g_strcmp0 (method_name, "CreateRemoteGreeterDisplay") == 0)
    {
        guint8 remote_id = 0;
        guint32 width = 0;
        guint32 height = 0;
        const gchar *address = NULL;
        g_variant_get (parameters, "(uuu&s)", &remote_id, &width, &height, &address);

        g_autoptr(GError) error = NULL;
        RemoteDisplaySession *session = remote_display_session_create (remote_id,
                                                                       width,
                                                                       height,
                                                                       NULL,
                                                                       address,
                                                                       REMOTE_DISPLAY_SESSION_MODE_GREETER,
                                                                       &error);
        if (!session)
        {
            if (error && error->domain == G_DBUS_ERROR && error->code == G_DBUS_ERROR_INVALID_ARGS)
                g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error->message);
            else
                g_dbus_method_invocation_return_gerror (invocation, error);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(o)", session->object_path));
        return;
    }

    if (g_strcmp0 (method_name, "CreateSingleLogonSession") == 0)
    {
        guint8 remote_id = 0;
        guint32 width = 0;
        guint32 height = 0;
        const gchar *user_name = NULL;
        const gchar *address = NULL;
        g_variant_get (parameters, "(yuu&s&s)", &remote_id, &width, &height, &user_name, &address);

        g_autoptr(GError) error = NULL;
        RemoteDisplaySession *session = remote_display_session_create (remote_id,
                                                                       width,
                                                                       height,
                                                                       user_name,
                                                                       address,
                                                                       REMOTE_DISPLAY_SESSION_MODE_SINGLE_LOGON,
                                                                       &error);
        if (!session)
        {
            if (error && error->domain == G_DBUS_ERROR && error->code == G_DBUS_ERROR_INVALID_ARGS)
                g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error->message);
            else
                g_dbus_method_invocation_return_gerror (invocation, error);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(o)", session->object_path));
        return;
    }

    g_dbus_method_invocation_return_error (invocation,
                                           G_DBUS_ERROR,
                                           G_DBUS_ERROR_UNKNOWN_METHOD,
                                           "Unknown method %s",
                                           method_name);
}

static GVariant *
remote_display_factory_get_property (G_GNUC_UNUSED GDBusConnection *connection,
                                     G_GNUC_UNUSED const gchar *sender,
                                     G_GNUC_UNUSED const gchar *object_path,
                                     G_GNUC_UNUSED const gchar *interface_name,
                                     const gchar *property_name,
                                     GError **error,
                                     G_GNUC_UNUSED gpointer user_data)
{
    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                 "Unknown property %s", property_name);
    return NULL;
}

static const GDBusInterfaceVTable remote_display_factory_vtable =
{
    remote_display_factory_call,
    remote_display_factory_get_property,
    NULL
};

static void
remote_display_factory_name_acquired_cb (GDBusConnection *connection,
                                         G_GNUC_UNUSED const gchar *name,
                                         G_GNUC_UNUSED gpointer user_data)
{
    remote_display_connection = g_object_ref (connection);

    g_autoptr(GError) error = NULL;
    remote_display_factory_reg_id = g_dbus_connection_register_object (connection,
                                                                       REMOTE_DISPLAY_FACTORY_OBJECT_PATH,
                                                                       remote_display_factory_interface_info (),
                                                                       &remote_display_factory_vtable,
                                                                       NULL,
                                                                       NULL,
                                                                       &error);
    if (remote_display_factory_reg_id == 0)
    {
        const gchar *message = error ? error->message : "unknown error";
        g_critical ("Failed to export remote display factory: %s", message);
    }
}

static void
remote_display_factory_name_lost_cb (GDBusConnection *connection,
                                     G_GNUC_UNUSED const gchar *name,
                                     G_GNUC_UNUSED gpointer user_data)
{
    if (remote_display_factory_reg_id != 0 && connection)
        g_dbus_connection_unregister_object (connection, remote_display_factory_reg_id);

    remote_display_factory_reg_id = 0;

    if (remote_display_connection)
        g_clear_object (&remote_display_connection);
}

void
remote_display_factory_init (DisplayManager *manager)
{
    g_return_if_fail (manager != NULL);

    g_set_object (&remote_display_manager, manager);
    if (remote_display_factory_bus_id != 0)
        return;

    GBusType bus_type = getuid () == 0 ? G_BUS_TYPE_SYSTEM : G_BUS_TYPE_SESSION;
    remote_display_factory_bus_id = g_bus_own_name (bus_type,
                                                    REMOTE_DISPLAY_FACTORY_BUS_NAME,
                                                    G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
                                                    remote_display_factory_name_acquired_cb,
                                                    NULL,
                                                    remote_display_factory_name_lost_cb,
                                                    NULL,
                                                    NULL);
}

void
remote_display_factory_stop (void)
{
    if (remote_display_factory_bus_id != 0)
    {
        g_bus_unown_name (remote_display_factory_bus_id);
        remote_display_factory_bus_id = 0;
    }

    if (remote_display_factory_reg_id != 0 && remote_display_connection)
        g_dbus_connection_unregister_object (remote_display_connection, remote_display_factory_reg_id);
    remote_display_factory_reg_id = 0;

    if (remote_display_sessions)
    {
        g_hash_table_unref (remote_display_sessions);
        remote_display_sessions = NULL;
    }

    if (remote_display_numbers_in_use)
    {
        g_hash_table_unref (remote_display_numbers_in_use);
        remote_display_numbers_in_use = NULL;
    }

    /* Best-effort cleanup of generated configs */
    g_rmdir (REMOTE_DISPLAY_CONFIG_DIR);

    g_clear_object (&remote_display_connection);
    g_clear_object (&remote_display_manager);

    if (remote_display_dbus_info)
    {
        g_dbus_node_info_unref (remote_display_dbus_info);
        remote_display_dbus_info = NULL;
    }

    remote_display_session_index = 0;
    remote_display_next_display_number = REMOTE_DISPLAY_DEFAULT_DISPLAY_NUMBER;
}
