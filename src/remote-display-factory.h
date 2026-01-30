/*
 * Copyright (C) 2024
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#ifndef __REMOTE_DISPLAY_FACTORY_H__
#define __REMOTE_DISPLAY_FACTORY_H__

#include <glib-object.h>

#include "display-manager.h"

G_BEGIN_DECLS

void remote_display_factory_init (DisplayManager *manager);
void remote_display_factory_stop (void);

/*
 * When an RDP greeter authenticates a user that already has an existing remote
 * session, we don't need to start a new session on the incoming remote seat.
 * Instead, update the existing remote session metadata (client-id/address/etc)
 * so the caller can re-attach to it, then the incoming seat can be stopped.
 *
 * Returns TRUE if an existing remote session is found and updated.
 */
gboolean remote_display_factory_attach_existing_session (Seat        *incoming_seat,
                                                        const gchar *user_name,
                                                        const gchar *client_id,
                                                        const gchar *address);

gboolean remote_display_factory_update_session_identity_for_client_id (const gchar *client_id,
                                                                       const gchar *user_name,
                                                                       const gchar *login1_session_id);

/*
 * For remote displays, the RemoteDisplaySession can be created before we know
 * which user will authenticate. Persist identity on successful authentication
 * so future connections can find the existing remote session by user_name, and
 * update the remote session_id when available.
 */
gboolean remote_display_factory_update_session_identity (Seat        *seat,
                                                         const gchar *user_name,
                                                         const gchar *login1_session_id);

G_END_DECLS

#endif /* __REMOTE_DISPLAY_FACTORY_H__ */
