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

#include "configuration.h"
#include "seat-config.h"
#include "seat.h"
#include "greeter-session.h"
#include "drd-dbus-lightdm.h"

#define REMOTE_DISPLAY_FACTORY_BUS_NAME "org.deepin.DisplayManager"
#define REMOTE_DISPLAY_FACTORY_OBJECT_PATH "/org/deepin/DisplayManager/RemoteDisplayFactory"
#define REMOTE_DISPLAY_SESSION_OBJECT_PREFIX "/org/deepin/DisplayManager/RemoteDisplayFactory/Sessions"
#define REMOTE_DISPLAY_X_COMMAND "/usr/bin/Xorg"
#define REMOTE_DISPLAY_CONFIG_DIR "/var/lib/lightdm/remote-displays"

typedef enum
{
    REMOTE_DISPLAY_SESSION_MODE_GREETER = 0,
    REMOTE_DISPLAY_SESSION_MODE_SINGLE_LOGON
} RemoteDisplaySessionMode;

typedef struct
{
    guint32 remote_id;
    guint32 width;
    guint32 height;
    gchar *user_name;
    gchar *address;
    gchar *object_path;
    DrdDBusLightdmRemoteDisplayFactorySession *dbus_session;
    RemoteDisplaySessionMode mode;

    Seat *seat;
    gchar *config_path;
    guint display_number;
    gulong seat_stopped_handler;
    gulong seat_session_added_handler;
    gulong seat_session_removed_handler;
    guint session_id;
} RemoteDisplaySession;

static DisplayManager *remote_display_manager = NULL;
static guint remote_display_factory_bus_id = 0;
static GDBusConnection *remote_display_connection = NULL;
static DrdDBusLightdmRemoteDisplayFactory *remote_display_factory_skeleton = NULL;
static GHashTable *remote_display_sessions = NULL;
static GHashTable *remote_display_numbers_in_use = NULL;
static guint remote_display_session_index = 0;
static guint remote_display_next_display_number = 0;

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

static guint
remote_display_default_display_number (void)
{
    return (guint) config_get_integer (config_get_instance (), "LightDM", "minimum-remote-display-number");
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
        remote_display_next_display_number = remote_display_default_display_number ();
    }
}

static gboolean
remote_display_session_register_object (RemoteDisplaySession *session, GError **error)
{
    g_return_val_if_fail (session != NULL, FALSE);
    g_return_val_if_fail (remote_display_connection != NULL, FALSE);

    g_autofree gchar *path = g_strdup_printf ("%s/%u", REMOTE_DISPLAY_SESSION_OBJECT_PREFIX, ++remote_display_session_index);
    session->object_path = g_steal_pointer (&path);
    g_autoptr(DrdDBusLightdmRemoteDisplayFactorySession) session_skeleton =
        drd_dbus_lightdm_remote_display_factory_session_skeleton_new ();

    drd_dbus_lightdm_remote_display_factory_session_set_user_name (session_skeleton,
                                                                   session->user_name ? session->user_name : "");
    drd_dbus_lightdm_remote_display_factory_session_set_address (session_skeleton,
                                                                 session->address ? session->address : "");
    drd_dbus_lightdm_remote_display_factory_session_set_session_id (session_skeleton,
                                                                    session->session_id);
    drd_dbus_lightdm_remote_display_factory_session_set_client_id (session_skeleton,
                                                                   session->remote_id);

    if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (session_skeleton),
                                           remote_display_connection,
                                           session->object_path,
                                           error))
        return FALSE;

    session->dbus_session = g_steal_pointer (&session_skeleton);
    return TRUE;
}

static void
remote_display_append_mode (GString *modelines, GString *modes, guint width, guint height)
{
    gdouble h_total = width + 656.0;
    gdouble v_total = height + 40.0;
    gdouble clock = (h_total * v_total * 60.0) / 1000000.0;

    guint h1 = width + 128;
    guint h2 = width + 328;
    guint h3 = width + 656;
    guint v1 = height + 3;
    guint v2 = height + 8;
    guint v3 = height + 40;

    g_string_append_printf (modelines,
                            "  Modeline \"%ux%u\"  %.2f  %u %u %u %u %u %u %u %u -hsync +vsync\n",
                            width,
                            height,
                            clock,
                            width,
                            h1,
                            h2,
                            h3,
                            height,
                            v1,
                            v2,
                            v3);

    if (modes->len > 0)
        g_string_append_c (modes, ' ');
    g_string_append_printf (modes, "\"%ux%u\"", width, height);
}

static gchar *
remote_display_session_build_config (RemoteDisplaySession *session)
{
    (void) g_mkdir_with_parents (REMOTE_DISPLAY_CONFIG_DIR, 0755);
    static const struct
    {
        guint width;
        guint height;
    } default_resolutions[] =
    {
        { 3840, 2160 },
        { 2560, 1440 },
        { 1920, 1080 },
        { 1600, 900 },
        { 1280, 720 }
    };
    GString *modelines = g_string_new ("");
    GString *modes = g_string_new ("");
    gsize i = 0;

    remote_display_append_mode (modelines, modes, session->width, session->height);
    for (i = 0; i < G_N_ELEMENTS (default_resolutions); i++)
    {
        guint width = default_resolutions[i].width;
        guint height = default_resolutions[i].height;

        if (width > session->width || height > session->height)
            continue;
        if (width == session->width && height == session->height)
            continue;

        remote_display_append_mode (modelines, modes, width, height);
    }

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
        "%s"
        "EndSection\n\n"
        "Section \"Screen\"\n"
        "  Identifier \"dummy_screen\"\n"
        "  Device     \"dummy_device\"\n"
        "  Monitor    \"dummy_monitor\"\n"
        "  DefaultDepth 24\n"
        "  SubSection \"Display\"\n"
        "    Depth 24\n"
        "    Modes %s\n"
        "  EndSubSection\n"
        "EndSection\n",
        modelines->str,
        modes->str);

    g_string_free (modelines, TRUE);
    g_string_free (modes, TRUE);

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
        g_remove (session->config_path);
        g_clear_pointer (&session->config_path, g_free);
    }
}

static guint
remote_display_session_parse_login1_id (const gchar *login1_id)
{
    if (!login1_id || login1_id[0] == '\0')
        return 0;

    gchar *end = NULL;
    guint64 value = g_ascii_strtoull (login1_id, &end, 10);
    if (!end || *end != '\0' || value > G_MAXUINT32)
        return 0;

    return (guint) value;
}

static void
remote_display_session_set_session_id (RemoteDisplaySession *session, guint new_id)
{
    g_return_if_fail (session != NULL);

    if (session->session_id == new_id)
        return;

    session->session_id = new_id;
    if (session->dbus_session)
        drd_dbus_lightdm_remote_display_factory_session_set_session_id (session->dbus_session, new_id);
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
    remote_display_session_set_session_id (session, remote_display_session_parse_login1_id (login1_id));
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

    if (session->dbus_session)
    {
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (session->dbus_session));
        g_clear_object (&session->dbus_session);
    }

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
    session->session_id = 0;

    g_autoptr(GError) register_error = NULL;
    if (!remote_display_session_register_object (session, &register_error))
    {
        const gchar *message = register_error ? register_error->message : "unknown error";
        g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Failed to export remote session object: %s", message);
        remote_display_session_free (session);
        return NULL;
    }

    session->config_path = remote_display_session_build_config (session); // TODO
    // session->config_path = g_strdup("/etc/X11/lcz/10-dummy.conf");
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

static gboolean
remote_display_factory_handle_create_remote_greeter_display (DrdDBusLightdmRemoteDisplayFactory *object,
                                                             GDBusMethodInvocation *invocation,
                                                             guint arg_remote_id,
                                                             guint arg_width,
                                                             guint arg_height,
                                                             const gchar *arg_address,
                                                             gpointer user_data)
{
    (void) user_data;

    g_autoptr(GError) error = NULL;
    RemoteDisplaySession *session = remote_display_session_create (arg_remote_id,
                                                                   arg_width,
                                                                   arg_height,
                                                                   NULL,
                                                                   arg_address,
                                                                   REMOTE_DISPLAY_SESSION_MODE_GREETER,
                                                                   &error);
    if (!session)
    {
        if (error && error->domain == G_DBUS_ERROR && error->code == G_DBUS_ERROR_INVALID_ARGS)
            g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error->message);
        else
            g_dbus_method_invocation_return_gerror (invocation, error);
        return TRUE;
    }

    drd_dbus_lightdm_remote_display_factory_complete_create_remote_greeter_display (object,
                                                                                    invocation,
                                                                                    session->object_path);
    return TRUE;
}

static gboolean
remote_display_factory_handle_create_single_logon_session (DrdDBusLightdmRemoteDisplayFactory *object,
                                                           GDBusMethodInvocation *invocation,
                                                           guint arg_remote_id,
                                                           guint arg_width,
                                                           guint arg_height,
                                                           const gchar *arg_user_name,
                                                           const gchar *arg_address,
                                                           gpointer user_data)
{
    (void) user_data;

    g_autoptr(GError) error = NULL;
    RemoteDisplaySession *session = remote_display_session_create (arg_remote_id,
                                                                   arg_width,
                                                                   arg_height,
                                                                   arg_user_name,
                                                                   arg_address,
                                                                   REMOTE_DISPLAY_SESSION_MODE_SINGLE_LOGON,
                                                                   &error);
    if (!session)
    {
        if (error && error->domain == G_DBUS_ERROR && error->code == G_DBUS_ERROR_INVALID_ARGS)
            g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "%s", error->message);
        else
            g_dbus_method_invocation_return_gerror (invocation, error);
        return TRUE;
    }

    drd_dbus_lightdm_remote_display_factory_complete_create_single_logon_session (object,
                                                                                  invocation,
                                                                                  session->object_path);
    return TRUE;
}

static void
remote_display_factory_name_acquired_cb (GDBusConnection *connection,
                                         G_GNUC_UNUSED const gchar *name,
                                         G_GNUC_UNUSED gpointer user_data)
{
    remote_display_connection = g_object_ref (connection);

    g_autoptr(DrdDBusLightdmRemoteDisplayFactory) factory_skeleton =
        drd_dbus_lightdm_remote_display_factory_skeleton_new ();
    g_signal_connect (factory_skeleton,
                      "handle-create-remote-greeter-display",
                      G_CALLBACK (remote_display_factory_handle_create_remote_greeter_display),
                      NULL);
    g_signal_connect (factory_skeleton,
                      "handle-create-single-logon-session",
                      G_CALLBACK (remote_display_factory_handle_create_single_logon_session),
                      NULL);

    g_autoptr(GError) error = NULL;
    if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (factory_skeleton),
                                           connection,
                                           REMOTE_DISPLAY_FACTORY_OBJECT_PATH,
                                           &error))
    {
        const gchar *message = error ? error->message : "unknown error";
        g_critical ("Failed to export remote display factory: %s", message);
        return;
    }

    remote_display_factory_skeleton = g_steal_pointer (&factory_skeleton);
}

static void
remote_display_factory_name_lost_cb (GDBusConnection *connection,
                                     G_GNUC_UNUSED const gchar *name,
                                     G_GNUC_UNUSED gpointer user_data)
{
    (void) connection;
    if (remote_display_factory_skeleton)
    {
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (remote_display_factory_skeleton));
        g_clear_object (&remote_display_factory_skeleton);
    }

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

    remote_display_next_display_number = remote_display_default_display_number ();

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

    if (remote_display_factory_skeleton)
    {
        g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (remote_display_factory_skeleton));
        g_clear_object (&remote_display_factory_skeleton);
    }

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

    remote_display_session_index = 0;
    remote_display_next_display_number = remote_display_default_display_number ();
}
