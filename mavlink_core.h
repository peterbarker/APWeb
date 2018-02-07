/*
  core mavlink functionality
 */
#pragma once

#define MAVLINK_SEPARATE_HELPERS
#define MAVLINK_NO_CONVERSION_HELPERS

// arm processor does not handle misaligned accesses
#define MAVLINK_ALIGNED_FIELDS 0

#define MAVLINK_SEND_UART_BYTES(chan, buf, len) comm_send_buffer(chan, buf, len)

// allow two mavlink ports
#define MAVLINK_COMM_NUM_BUFFERS 2

#include <generated/mavlink/ardupilotmega/version.h>
#include <generated/mavlink/mavlink_types.h>

extern mavlink_system_t mavlink_system;

/*
  Send a byte to the nominated MAVLink channel
*/
void comm_send_ch(mavlink_channel_t chan, uint8_t ch);

#define MAVLINK_USE_CONVENIENCE_FUNCTIONS
#include <generated/mavlink/ardupilotmega/mavlink.h>
#ifndef MAV_COMP_ID_AUTOPILOT1
#define MAV_COMP_ID_AUTOPILOT1 1
#endif

// alias for link to flight controller
#define MAVLINK_COMM_FC MAVLINK_COMM_0

// link to real FC when using SITL, for RC_CHANNELS
#define MAVLINK_COMM_RC MAVLINK_COMM_1

// mavlink target information; if a packet needs to be sent to a
// target, this system id should be used.  These must be implemented...
uint8_t mavlink_target_sysid(void);
uint8_t mavlink_target_compid(void);

