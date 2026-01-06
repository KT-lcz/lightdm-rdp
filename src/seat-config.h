/*
 * Helper utilities for applying seat configuration blocks.
 */

#ifndef SEAT_CONFIG_H_
#define SEAT_CONFIG_H_

#include <glib.h>

#include "seat.h"


G_BEGIN_DECLS

void seat_config_apply (Seat *seat, const gchar *seat_name);
GList *get_config_sections (const gchar *seat_name);
G_END_DECLS

#endif /* SEAT_CONFIG_H_ */
