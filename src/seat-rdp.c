/*
 * Seat implementation specialized for remote Xorg sessions serving RDP clients.
 */

#include "seat-rdp.h"

#include "process.h"
#include "session.h"
#include "x-authority.h"
#include "x-server-local.h"
#include "accounts.h"
#include "configuration.h"
#include "remote-display-factory.h"

typedef struct _SeatRDP
{
    Seat parent_instance;
} SeatRDP;

typedef struct _SeatRDPClass
{
    SeatClass parent_class;
} SeatRDPClass;

G_DEFINE_TYPE (SeatRDP, seat_rdp, SEAT_TYPE)

static void
seat_rdp_apply_remote_env (Seat *seat, Session *session)
{
    const gchar *remote_id = seat_get_string_property (seat, "remote-id");
    const gchar *remote_address = seat_get_string_property (seat, "remote-address");
    const gchar *remote_mode = seat_get_string_property (seat, "remote-mode");

    session_set_env (session, "XDG_SEAT", "");

    if (remote_id && remote_id[0])
        session_set_env (session, "REMOTE_ID", remote_id);
    if (remote_mode && remote_mode[0])
        session_set_env (session, "REMOTE_MODE", remote_mode);
    if (remote_address && remote_address[0])
    {
        session_set_env (session, "REMOTE_HOST", remote_address);
        session_set_env (session, "PAM_RHOST", remote_address);
        session_set_remote_host_name (session, remote_address);
    }
    else
        session_set_remote_host_name (session, NULL);
}

static void
seat_rdp_setup (Seat *seat)
{
    seat_set_property (seat, "skip-greeter-on-logout", "true");
    seat_set_supports_multi_session (seat, FALSE);
    seat_set_share_display_server (seat, seat_get_boolean_property (seat, "xserver-share"));
    SEAT_CLASS (seat_rdp_parent_class)->setup (seat);
}

static DisplayServer *
seat_rdp_create_display_server (Seat *seat, Session *session)
{
    if (g_strcmp0 (session_get_session_type (session), "x") != 0)
    {
        l_warning (seat, "RDP seat only supports X sessions");
        return NULL;
    }

    g_autoptr(XServerLocal) x_server = x_server_local_new ();

    const gchar *command = seat_get_string_property (seat, "xserver-command");
    if (command && command[0])
        x_server_local_set_command (x_server, command);

    const gchar *config = seat_get_string_property (seat, "xserver-config");
    if (config && config[0])
        x_server_local_set_config (x_server, config);

    const gchar *display_number_value = seat_get_string_property (seat, "xserver-display-number");
    if (display_number_value && display_number_value[0])
    {
        gchar *endptr = NULL;
        long number = g_ascii_strtoll (display_number_value, &endptr, 10);
        if (endptr && *endptr == '\0' && number >= 0)
        {
            if (!x_server_local_set_display_number (x_server, (guint) number))
                l_warning (seat, "Display number %ld already in use, falling back to automatic allocation", number);
        }
        else
            l_warning (seat, "Invalid xserver-display-number '%s'", display_number_value);
    }

    gboolean allow_tcp = seat_get_boolean_property (seat, "xserver-allow-tcp");
    x_server_local_set_allow_tcp (x_server, allow_tcp);
    x_server_local_set_xdg_seat (x_server, seat_get_name (seat));

    g_autofree gchar *number = g_strdup_printf ("%d", x_server_get_display_number (X_SERVER (x_server)));
    g_autoptr(XAuthority) cookie = x_authority_new_local_cookie (number);
    x_server_set_authority (X_SERVER (x_server), cookie);

    return DISPLAY_SERVER (g_steal_pointer (&x_server));
}

static GreeterSession *
seat_rdp_create_greeter_session (Seat *seat)
{
    GreeterSession *session = SEAT_CLASS (seat_rdp_parent_class)->create_greeter_session (seat);
    seat_rdp_apply_remote_env (seat, SESSION (session));
    return session;
}

static Session *
seat_rdp_create_session (Seat *seat)
{
    Session *session = SEAT_CLASS (seat_rdp_parent_class)->create_session (seat);
    seat_rdp_apply_remote_env (seat, session);
    return session;
}

static void
seat_rdp_run_script (Seat *seat, DisplayServer *display_server, Process *script)
{
    if (IS_X_SERVER_LOCAL (display_server))
    {
        const gchar *path = x_server_local_get_authority_file_path (X_SERVER_LOCAL (display_server));
        process_set_env (script, "DISPLAY", x_server_get_address (X_SERVER (display_server)));
        process_set_env (script, "XAUTHORITY", path);
    }

    const gchar *remote_address = seat_get_string_property (seat, "remote-address");
    const gchar *remote_id = seat_get_string_property (seat, "remote-id");
    process_set_env (script, "XDG_SEAT", "");
    if (remote_address && remote_address[0])
        process_set_env (script, "REMOTE_HOST", remote_address);
    if (remote_id && remote_id[0])
        process_set_env (script, "REMOTE_ID", remote_id);

    SEAT_CLASS (seat_rdp_parent_class)->run_script (seat, display_server, script);
}

static gboolean
delay_stop (gpointer data)
{
    seat_stop (SEAT (data));
    return G_SOURCE_REMOVE;
}

static gboolean
seat_rdp_update_session_identity (Seat *seat, Session *session)
{
    const gchar *client_id = seat_get_string_property (seat, "remote-id");
    const gchar *user_name = session_get_username (session);
    const gchar *login1_session_id = session_get_login1_session_id (session);

    if (client_id && client_id[0] != '\0')
        return remote_display_factory_update_session_identity_for_client_id (client_id, user_name, login1_session_id);

    return remote_display_factory_update_session_identity (seat, user_name, login1_session_id);
}

static gboolean
seat_rdp_attach_existing_session (Seat        *seat,
                                  Session     *session,
                                  const gchar *user_name)
{
    const gchar *client_id = seat_get_string_property (seat, "remote-id");
    const gchar *address = seat_get_string_property (seat, "remote-address");

    if (!remote_display_factory_attach_existing_session (seat, user_name, client_id, address))
        return FALSE;

    l_debug (seat, "Remote greeter authenticated for existing session, stopping incoming seat");
    g_timeout_add_seconds_full (G_PRIORITY_DEFAULT, 3, delay_stop, g_object_ref (seat), g_object_unref);
    session_stop (session);

    return TRUE;
}

static gboolean
seat_rdp_session_authenticated (Seat *seat, Session *session)
{
    const gchar *remote_mode = seat_get_string_property (seat, "remote-mode");
    const gchar *user_name = session_get_username (session);

    l_debug (seat, "authentication complete username: %s", user_name);

    if (g_strcmp0 (remote_mode, "greeter") == 0 && IS_GREETER_SESSION (session))
        return FALSE;

    if (g_strcmp0 (remote_mode, "greeter") == 0 &&
        seat_rdp_attach_existing_session (seat, session, user_name))
        return TRUE;

    remote_display_factory_update_session_identity (seat,
                                                    user_name,
                                                    session_get_login1_session_id (session));
    l_debug (seat, "Session authentication session id: %s", session_get_login1_session_id (session));

    return FALSE;
}

static void
remote_greeter_authentication_complete_cb (Session *session, Seat *seat)
{
    l_debug (seat, "remote_greeter_authentication_complete_cb");
    if (!session_get_is_authenticated (session))
        return;

    const gchar *remote_mode = seat_get_string_property (seat, "remote-mode");
    if (g_strcmp0 (remote_mode, "greeter") != 0)
        return;

    const gchar *user_name = session_get_username (session);
    g_autofree gchar *greeter_user = config_get_string (config_get_instance (), "LightDM", "greeter-user");
    if (!user_name || user_name[0] == '\0' || (greeter_user && g_strcmp0 (user_name, greeter_user) == 0))
    {
        const gchar *cached = g_object_get_data (G_OBJECT (session), "rdp-requested-user");
        if (cached && cached[0] != '\0')
            user_name = cached;
    }

    if (!user_name || user_name[0] == '\0' || (greeter_user && g_strcmp0 (user_name, greeter_user) == 0))
        return;

    g_autoptr(User) user = accounts_get_user_by_name (user_name);
    if (!user)
        return;

    const gchar *client_id = seat_get_string_property (seat, "remote-id");
    if (client_id && client_id[0] != '\0' &&
        seat_rdp_attach_existing_session (seat, session, user_name))
        return;

    if (client_id && client_id[0] != '\0')
        remote_display_factory_update_session_identity_for_client_id (client_id, user_name, NULL);
}

static void
seat_rdp_prepare_session_for_greeter (Seat *seat, Session *session, Greeter *greeter)
{
    const gchar *requested_user = greeter_get_active_username (greeter);
    if (requested_user && requested_user[0] != '\0')
        g_object_set_data_full (G_OBJECT (session), "rdp-requested-user", g_strdup (requested_user), g_free);

    const gchar *remote_mode = seat_get_string_property (seat, "remote-mode");
    if (g_strcmp0 (remote_mode, "greeter") == 0)
        g_signal_connect_after (session,
                                SESSION_SIGNAL_AUTHENTICATION_COMPLETE,
                                G_CALLBACK (remote_greeter_authentication_complete_cb),
                                seat);
}

static void
seat_rdp_session_running (Seat *seat, Session *session)
{
    if (IS_GREETER_SESSION (session))
        return;

    seat_rdp_update_session_identity (seat, session);
}

static gboolean
seat_rdp_can_stop_unused_display_server (Seat *seat, DisplayServer *display_server)
{
    (void) seat;
    (void) display_server;

    return FALSE;
}

static void
seat_rdp_class_init (SeatRDPClass *klass)
{
    SeatClass *seat_class = SEAT_CLASS (klass);

    seat_class->setup = seat_rdp_setup;
    seat_class->create_display_server = seat_rdp_create_display_server;
    seat_class->can_stop_unused_display_server = seat_rdp_can_stop_unused_display_server;
    seat_class->create_greeter_session = seat_rdp_create_greeter_session;
    seat_class->create_session = seat_rdp_create_session;
    seat_class->session_authenticated = seat_rdp_session_authenticated;
    seat_class->prepare_session_for_greeter = seat_rdp_prepare_session_for_greeter;
    seat_class->session_running = seat_rdp_session_running;
    seat_class->run_script = seat_rdp_run_script;
}

static void
seat_rdp_init (SeatRDP *seat)
{
    (void) seat;
}
