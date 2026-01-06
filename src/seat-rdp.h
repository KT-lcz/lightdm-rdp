/*
 * Seat implementation for remote RDP-driven sessions.
 */

#ifndef SEAT_RDP_H_
#define SEAT_RDP_H_

#include "seat.h"

G_BEGIN_DECLS

#define SEAT_RDP_TYPE (seat_rdp_get_type ())
#define SEAT_RDP(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), SEAT_RDP_TYPE, SeatRDP))

typedef struct _SeatRDP SeatRDP;
typedef struct _SeatRDPClass SeatRDPClass;

GType seat_rdp_get_type (void);

G_END_DECLS

#endif /* SEAT_RDP_H_ */
