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

G_END_DECLS

#endif /* __REMOTE_DISPLAY_FACTORY_H__ */
