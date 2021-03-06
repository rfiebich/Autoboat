/**
 * This file contains all of the MAVLink interfacing necessary by Sealion.
 * It relies heavily on the MavlinkMessageScheduler for scheduling transmission
 * of MAVLink messages such as to not overload the interface.
 *
 * The main functions are at the bottom: MavLinkTransmit() and MavLinkReceive()
 * handle the dispatching of messages (and sending of non-FSM reliant ones) and
 * the reception of messages and forwarding of reception events to the relevent
 * FSMs. The two state machine functions (MavLinkEvaluateMissionState and
 * MavLinkEvaluateParameterState) both contain all of the state logic for the
 * MAVLink mission and parameter protocols. As the specifications for those two
 * protocols are not fully defined they have been tested with QGroundControl to
 * work correctly.
 *
 * This code was written to be as generic as possible. If you remove all of the
 * custom messages and switch the transmission from uart1EnqueueData() it should
 * be almost exclusively relient on modules like MissionManager and the scheduler.
 */

#include "Uart1.h"
#include "MessageScheduler.h"
#include "EcanSensors.h"
#include "Rudder.h"
#include "Revo.h"
#include "MavlinkGlue.h"

#include <stdio.h>

// Set up some state machine variables for the parameter protocol
enum {
	PARAM_STATE_INACTIVE = 0,

	PARAM_STATE_SINGLETON_SEND_VALUE,

	PARAM_STATE_STREAM_SEND_VALUE,
	PARAM_STATE_STREAM_DELAY
};

enum {
	PARAM_EVENT_NONE,
	PARAM_EVENT_ENTER_STATE,
	PARAM_EVENT_EXIT_STATE,

	PARAM_EVENT_REQUEST_LIST_RECEIVED,
	PARAM_EVENT_REQUEST_READ_RECEIVED,
	PARAM_EVENT_SET_RECEIVED,
	PARAM_EVENT_VALUE_DISPATCHED
};

// Set up state machine variables for the mission protocol
enum {
	MISSION_STATE_INACTIVE = 0,

	// States handling reading of our mission list
	MISSION_STATE_SEND_MISSION_COUNT,
	MISSION_STATE_COUNTDOWN,
	MISSION_STATE_SEND_MISSION_ITEM,

	// States handling current mission setting
	MISSION_STATE_SEND_CURRENT,

	// States handling writing to our mission list
	MISSION_STATE_SEND_REQUEST,
	MISSION_STATE_ACK_ERROR,
	MISSION_STATE_ACK_NO_SPACE,
	MISSION_STATE_ACK_INVALID_SEQUENCE,
	MISSION_STATE_ACK_ACCEPTED
};
// Set up the events necessary for the mission protocol state machine
enum {
	MISSION_EVENT_NONE = 0,
	MISSION_EVENT_ENTER_STATE,
	MISSION_EVENT_EXIT_STATE,

	// Message reception events
	MISSION_EVENT_COUNT_RECEIVED,
	MISSION_EVENT_ACK_RECEIVED,
	MISSION_EVENT_REQUEST_RECEIVED,
	MISSION_EVENT_REQUEST_LIST_RECEIVED,
	MISSION_EVENT_CLEAR_ALL_RECEIVED,
	MISSION_EVENT_SET_CURRENT_RECEIVED,
	MISSION_EVENT_ITEM_RECEIVED,

	// Message transmission events
	MISSION_EVENT_COUNT_DISPATCHED,
	MISSION_EVENT_ACK_DISPATCHED,
	MISSION_EVENT_REQUEST_DISPATCHED,
	MISSION_EVENT_CURRENT_DISPATCHED,
	MISSION_EVENT_ITEM_DISPATCHED
};

// These flags are for use with the SYS_STATUS MAVLink message as a mapping from the Autoboat's
// sensors to the sensors/controllers available in SYS_STATUS.
enum {
	ONBOARD_SENSORS_REVO_GS = (1 << 0) | (1 << 1) | (1 << 2),
	ONBOARD_SENSORS_WSO100 = 1 << 3,
	ONBOARD_SENSORS_GPS = 1 << 5,
	ONBOARD_CONTROL_YAW_POS = 1 << 12,
	ONBOARD_CONTROL_XY_POS = 1 << 14,
	ONBOARD_CONTROL_MOTOR = 1 << 15
};

// Store a module-wide variable for common MAVLink system variables.
static mavlink_system_t mavlink_system = {
	20, // Arbitrarily chosen MAV number
	MAV_COMP_ID_ALL,
	MAV_TYPE_SURFACE_BOAT,
	MAV_STATE_UNINIT,
	MAV_MODE_PREFLIGHT | MAV_MODE_FLAG_MANUAL_INPUT_ENABLED, // The vehicle is booting up and have manual control enabled.
	0 // Unused and unsure of expected usage
};

// Latch onto the first groundstation unit and only receive and transmit to it.
static uint8_t groundStationSystemId = 0;
static uint8_t groundStationComponentId = 0;

// Globally declare here how many parameters we have.
// TODO: Move into its own code section
static uint16_t parameterCount = 4;

// Declare a character buffer here to prevent continual allocation/deallocation of MAVLink buffers.
static uint8_t buf[MAVLINK_MAX_PACKET_LEN];
static uint16_t len;

// Track how well MAVLink decoding is going:
// WARN: Possible overflow over long-enough duration
uint16_t mavLinkMessagesReceived = 0;
uint16_t mavLinkMessagesFailedParsing = 0;

// Track manual control data transmit via MAVLink
struct {
	union {
		struct {
			int16_t X;
			int16_t Z;
			uint16_t Buttons; 
			bool NewData;
		} unpackedData;
		uint8_t packedData[7];
	};
} mavlinkManualControlData;

// Set up the message scheduler for MAVLink transmission
#define MAVLINK_MSGS_SIZE 21
uint8_t ids[MAVLINK_MSGS_SIZE] = {
	MAVLINK_MSG_ID_HEARTBEAT,
	MAVLINK_MSG_ID_SYS_STATUS,
	MAVLINK_MSG_ID_SYSTEM_TIME,
	MAVLINK_MSG_ID_LOCAL_POSITION_NED,
	MAVLINK_MSG_ID_ATTITUDE,
	MAVLINK_MSG_ID_GPS_RAW_INT,
	MAVLINK_MSG_ID_RC_CHANNELS_SCALED,
	MAVLINK_MSG_ID_STATUS_AND_ERRORS,
	MAVLINK_MSG_ID_WSO100,
	MAVLINK_MSG_ID_BASIC_STATE,
	MAVLINK_MSG_ID_RUDDER_RAW,
	MAVLINK_MSG_ID_DST800,
	MAVLINK_MSG_ID_REVO_GS,
	MAVLINK_MSG_ID_MAIN_POWER,
	
	// Only used for transient messages
	MAVLINK_MSG_ID_MISSION_CURRENT,
	MAVLINK_MSG_ID_MISSION_COUNT,
	MAVLINK_MSG_ID_MISSION_ACK,
	MAVLINK_MSG_ID_MISSION_REQUEST,
	MAVLINK_MSG_ID_MISSION_ITEM,
	MAVLINK_MSG_ID_GPS_GLOBAL_ORIGIN,
	MAVLINK_MSG_ID_PARAM_VALUE,
};
uint16_t tsteps[MAVLINK_MSGS_SIZE][2][8] = {};
uint8_t  mSizes[MAVLINK_MSGS_SIZE];
MessageSchedule mavlinkSchedule = {
	MAVLINK_MSGS_SIZE,
	ids,
	mSizes,
	0,
	tsteps
};

/**
 * Initialize MAVLink transmission. This just sets up the MAVLink scheduler with the basic
 * repeatedly-transmit messages.
 */
void MavLinkInit(void)
{
	// First initialize the MessageSchedule struct with the proper sizes.
	const uint8_t const mavMessageSizes[] = MAVLINK_MESSAGE_LENGTHS;
	int i;
	for (i = 0; i < MAVLINK_MSGS_SIZE; ++i) {
		mavlinkSchedule.MessageSizes[i] = mavMessageSizes[ids[i]];
	}
	
	const uint8_t const periodicities[] = {2, 2, 1, 10, 10, 5, 4, 4, 2, 10, 4, 2, 2, 5};
	for (i = 0; i < sizeof(periodicities); ++i) {
		if (!AddMessageRepeating(&mavlinkSchedule, ids[i], periodicities[i])) {
			while (1);
		}
	}
}

/**
 * Simulink helper function that schedules a one-off MISSION_CURRENT message.
 */
void MavLinkScheduleCurrentMission(void)
{
	if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_CURRENT)) {
		while (1);
	}
}

/**
 * Simulink helper function that scheduls a one-off GPS_GLOBAL_ORIGIN message.
 */
void MavLinkScheduleGpsOrigin(void)
{
	// Commented out on 7/22/2012 because QGC doesn't handle these messages well,
	// spawning a modal dialog whenever it's received.
	//if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_GPS_GLOBAL_ORIGIN)) {
	//	while (1);
	//}
}

/**
 * This function creates a MAVLink heartbeat message with some basic parameters and
 * caches that message (along with its size) in the module-level variables declared
 * above. This buffer should be transmit at 1Hz back to the groundstation.
 */
void MavLinkSendHeartbeat(void)
{
	mavlink_message_t msg;

	// Pack the message
	mavlink_msg_heartbeat_pack(mavlink_system.sysid, mavlink_system.compid, &msg, mavlink_system.type, MAV_AUTOPILOT_GENERIC_WAYPOINTS_ONLY, mavlink_system.mode, 0, mavlink_system.state);

	// Copy the message to the send buffer
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * This function transmits the system time. Looks like it's necessary for QGC to
 * record timestamps on data reliably. For some reason it doesn't just use the local
 * time of message reception. Hopefully this fixes that.
 * This message is only transmitted if there actually is a global clock to sync with.
 */
void MavLinkSendSystemTime(void)
{
	// Grab the global time if the GPS is active
	if (sensorAvailability.gps.active) {
		mavlink_message_t msg;

		// Pack the message
		mavlink_msg_system_time_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		                             dateTimeDataStore.usecSinceEpoch, systemStatus.time*10);

		// Copy the message to the send buffer
		len = mavlink_msg_to_send_buffer(buf, &msg);
		uart1EnqueueData(buf, (uint8_t)len);
	}
}

/**
 * This function transmits a MAVLink SYS_STATUS message. It relies on various external information such as sensor/actuator status
 * from ecanSensors.h, the internalVariables struct exported by Simulink, and the drop rate calculated within ecanSensors.c.
 */
void MavLinkSendStatus(void)
{
	mavlink_message_t msg;

	// Declare that we have onboard sensors: 3D gyro, 3D accelerometer, 3D magnetometer, absolute pressure, GPS
	// And that we have the following controllers: yaw position, x/y position control, motor outputs/control.
	uint32_t systemsPresent = ONBOARD_SENSORS_REVO_GS |
	                          ONBOARD_SENSORS_WSO100  |
	                          ONBOARD_SENSORS_GPS     |
	                          ONBOARD_CONTROL_YAW_POS |
	                          ONBOARD_CONTROL_XY_POS  |
	                          ONBOARD_CONTROL_MOTOR;

	uint32_t systemsEnabled = ONBOARD_CONTROL_YAW_POS;
	systemsEnabled |= sensorAvailability.gps.enabled?ONBOARD_SENSORS_GPS:0;
	systemsEnabled |= sensorAvailability.revo_gs.enabled?ONBOARD_SENSORS_REVO_GS:0;
	systemsEnabled |= sensorAvailability.wso100.enabled?ONBOARD_SENSORS_WSO100:0;
	// The DST800 doesn't map into this bitfield.
	// The power node doesn't map into this bitfield.
	systemsEnabled |= sensorAvailability.prop.enabled?(ONBOARD_CONTROL_XY_POS|ONBOARD_CONTROL_MOTOR):0;

	uint32_t systemsActive = ONBOARD_CONTROL_YAW_POS;
	systemsActive |= sensorAvailability.gps.active?ONBOARD_SENSORS_GPS:0;
	systemsActive |= sensorAvailability.revo_gs.active?ONBOARD_SENSORS_REVO_GS:0;
	systemsActive |= sensorAvailability.wso100.active?ONBOARD_SENSORS_WSO100:0;
	// The DST800 doesn't map into this bitfield.
	// The power node doesn't map into this bitfield.
	systemsActive |= sensorAvailability.prop.active?(ONBOARD_CONTROL_XY_POS|ONBOARD_CONTROL_MOTOR):0;

	// Grab the globally-declared battery sensor data and map into the values necessary for transmission.
	uint16_t voltage = (uint16_t)(internalVariables.BatteryVoltage * 1000);
	int16_t amperage = (int16_t)(internalVariables.BatteryAmperage * 100);

	// Calculate the drop rate
	uint16_t dropRate = 0;
	if (mavLinkMessagesFailedParsing) {
		dropRate = (uint16_t)(((float)mavLinkMessagesFailedParsing) * 10000.0f / ((float)(mavLinkMessagesReceived + mavLinkMessagesFailedParsing)));
	}

	mavlink_msg_sys_status_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		systemsPresent, systemsEnabled, systemsActive,
		(uint16_t)(systemStatus.cpu_load)*10,
		voltage, amperage, -1,
		dropRate, 0, 0, 0, 0, 0);
	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * Pull the raw GPS sensor data from the gpsDataStore struct within the GPS module and
 * transmit it via MAVLink over UART1.
 * TODO: Convert this message to a GLOBAL_POSITION_INT
 */
void MavLinkSendRawGps(void)
{
	mavlink_message_t msg;

	mavlink_msg_gps_raw_int_pack(mavlink_system.sysid, mavlink_system.compid, &msg, ((uint64_t)systemStatus.time)*10000,
		sensorAvailability.gps.active?3:0, gpsDataStore.lat.lData, gpsDataStore.lon.lData, gpsDataStore.alt.lData,
		0xFFFF, 0xFFFF,
		gpsDataStore.sog.usData, (uint16_t)(((float)gpsDataStore.cog.usData) * 180 / M_PI / 100),
		0xFF);

	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}

/**
  * Transmit the main battery state as obtained from the power node via the CAN bus.
  */
void MavLinkSendMainPower(void)
{
	mavlink_message_t msg;

	mavlink_msg_main_power_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		(uint16_t)(powerDataStore.voltage.flData * 100.0f),(uint16_t)(powerDataStore.current.flData * 10.0f));

	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}


/**
 * Transmits the custom BASIC_STATE message. This just transmits a bunch of random variables
 * that are good to know but arbitrarily grouped.
 */
void MavLinkSendBasicState(void)
{
	mavlink_message_t msg;

	mavlink_msg_basic_state_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		internalVariables.RudderCommand, rudderSensorData.RudderAngle.flData,
		internalVariables.ThrottleCommand,
		internalVariables.PropellerRpm,
		internalVariables.L2Vector[0], internalVariables.L2Vector[1]
	);

	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * Transmits the vehicle attitude. Right now just the yaw value.
 * Expects systemStatus.time to be in centiseconds which are then converted
 * to ms for transmission.
 * Yaw should be in radians where positive is eastward from north.
 */
void MavLinkSendAttitude(void)
{
	mavlink_message_t msg;

	mavlink_msg_attitude_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
	                          systemStatus.time*10, 0.0, 0.0, internalVariables.Heading, 0.0, 0.0, 0.0);

	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * This function takes in the local position and local velocity (as 3-tuples) from
 * Matlab as real32s and ships them off over a MAVLink MAVLINK_MSG_ID_LOCAL_POSITION_NED
 * message.
 * It also expects a systemStatus struct with a uint32_t time element that holds the
 * current system time in centiseconds.
 */
void MavLinkSendLocalPosition(void)
{
	mavlink_message_t msg;

	mavlink_msg_local_position_ned_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
	                                    systemStatus.time*10,
	                                    internalVariables.LocalPosition[0], internalVariables.LocalPosition[1], internalVariables.LocalPosition[2],
	                                    internalVariables.Velocity[0], internalVariables.Velocity[1], internalVariables.Velocity[2]);

	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * Only transmit scaled manual control data messages if manual control is enabled OR if 
 * the RC transmitter is enabled as the RC transmitter overrides everything.
 */
void MavLinkSendRcScaledData(void)
{
	if (!(systemStatus.status & 1)) {
		mavlink_message_t msg;

		mavlink_msg_rc_channels_scaled_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		                                    systemStatus.time*10,
		                                    0,
		                                    (int16_t)(signalRudderScaled * 10000),
		                                    (int16_t)(signalThrottleScaled * 10000),
		                                    (int16_t)(signalModeScaled * 10000),
		                                    INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX, INT16_MAX,
		                                    UINT8_MAX);

		len = mavlink_msg_to_send_buffer(buf, &msg);

		uart1EnqueueData(buf, (uint8_t)len);
	}
}


/**
 * Transmits the current GPS position of the origin of the local coordinate frame that the North-East-Down
 * coordinates are all relative too. They should be in units of 1e-7 degrees.
 */
void MavLinkSendGpsGlobalOrigin(void)
{
	mavlink_message_t msg;

	int32_t latitude = gpsDataStore.lat.lData;
	int32_t longitude = gpsDataStore.lon.lData;
	int32_t altitude = gpsDataStore.alt.lData;

	mavlink_msg_gps_global_origin_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
	                                   latitude, longitude, altitude);

	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * Transmit the current mission index via UART1. GetCurrentMission returns a -1 if there're no missions,
 * so we check and only transmit valid current missions.
 */
void MavLinkSendCurrentMission(void)
{
	int8_t currentMission;

	GetCurrentMission(&currentMission);

	if (currentMission != -1) {
		mavlink_message_t msg;
		mavlink_msg_mission_current_pack(mavlink_system.sysid, mavlink_system.compid, &msg, (uint16_t)currentMission);
		len = mavlink_msg_to_send_buffer(buf, &msg);
		uart1EnqueueData(buf, (uint8_t)len);
	}
}

/**
 * Transmit a mission acknowledgement message. The type of message is the sole argument to this
 * function (see enum MAV_MISSIONRESULT).
 */
void MavLinkSendMissionAck(uint8_t type)
{
	mavlink_message_t msg;
	mavlink_msg_mission_ack_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
	                             groundStationSystemId, groundStationComponentId, type);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

void MavLinkSendMissionCount(void)
{
	uint8_t missionCount;
	mavlink_message_t msg;
	GetMissionCount(&missionCount);
	mavlink_msg_mission_count_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
	                               groundStationSystemId, groundStationComponentId, missionCount);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

void MavLinkSendMissionItem(uint8_t currentMissionIndex)
{
	Mission m;
	uint8_t result;
	GetMission(currentMissionIndex, &m, &result);
	if (result) {
		mavlink_message_t msg;
		int8_t missionManagerCurrentIndex;
		GetCurrentMission(&missionManagerCurrentIndex);
		mavlink_msg_mission_item_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		                              groundStationSystemId, groundStationComponentId, currentMissionIndex,
		                              m.refFrame, m.action, (currentMissionIndex == (uint8_t)missionManagerCurrentIndex),
		                              m.autocontinue, m.parameters[0], m.parameters[1], m.parameters[2], m.parameters[3],
		                              m.coordinates[0], m.coordinates[1], m.coordinates[2]);
		len = mavlink_msg_to_send_buffer(buf, &msg);
		uart1EnqueueData(buf, (uint8_t)len);
	}
}

void MavLinkSendMissionRequest(uint8_t currentMissionIndex)
{
	mavlink_message_t msg;
	mavlink_msg_mission_request_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
	                                 groundStationSystemId, groundStationComponentId, currentMissionIndex);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * The following functions are helper functions for reading the various parameters aboard the boat.
 */
void _transmitParameter(uint16_t id)
{
	mavlink_message_t msg;
	mavlink_param_union_t x;
	mavlink_param_value_t valueMsg = {
		0.0,
		parameterCount,
		0,
		"",
		MAVLINK_TYPE_UINT32_T
	};

	switch (id) {
		case 0:
			x.param_uint32 = (systemStatus.status & (1 << 0))?1:0;
			valueMsg.param_value = x.param_float;
			valueMsg.param_index = 0;
			strncpy(valueMsg.param_id, "MODE_AUTO", 16);
		break;
		case 1:
			x.param_uint32 = (systemStatus.status & (1 << 1))?1:0;
			valueMsg.param_value = x.param_float;
			valueMsg.param_index = 1;
			strncpy(valueMsg.param_id, "MODE_HIL", 16);
		break;
		case 2:
			x.param_uint32 = (systemStatus.status & (1 << 2))?1:0;
			valueMsg.param_value = x.param_float;
			valueMsg.param_index = 2;
			strncpy(valueMsg.param_id, "MODE_HILSENSE", 16);
		break;
		case 3:
			x.param_uint32 = (systemStatus.status & (1 << 3))?1:0;
			valueMsg.param_value = x.param_float;
			valueMsg.param_index = 3;
			strncpy(valueMsg.param_id, "MODE_RCDISCON", 16);
		break;
		default:
			return; // Do nothing if there's no matching parameter.
		break;
	}

	mavlink_msg_param_value_encode(mavlink_system.sysid, mavlink_system.compid, &msg, &valueMsg);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

/** Custom Sealion Messages **/

void MavLinkSendRudderRaw(void)
{
	mavlink_message_t msg;

	mavlink_msg_rudder_raw_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
                                rudderSensorData.RudderPotValue.usData, rudderSensorData.LimitHitPort, 0, rudderSensorData.LimitHitStarboard,
                                rudderSensorData.RudderPotLimitPort.usData, rudderSensorData.RudderPotLimitStarboard.usData);

	len = mavlink_msg_to_send_buffer(buf, &msg);

	uart1EnqueueData(buf, (uint8_t)len);
}

void MavLinkSendStatusAndErrors(void)
{
	mavlink_message_t msg;
	mavlink_msg_status_and_errors_pack(mavlink_system.sysid, mavlink_system.compid, &msg, systemStatus.status, systemStatus.reset);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);

	// And finally update the MAVLink state and run mode based on the system state.

	// If the startup reset line is triggered, indicate we're booting up. This is the only unarmed state
	// although that's not technically true with this controller.
	if (systemStatus.reset & (1 << 0)) {
		mavlink_system.state = MAV_STATE_BOOT;
		mavlink_system.mode &= ~MAV_MODE_FLAG_SAFETY_ARMED;
	// Otherwise if we're undergoing calibration indicate that
	} else if (systemStatus.reset & (1 << 5)) {
		mavlink_system.state = MAV_STATE_CALIBRATING;
		mavlink_system.mode |= MAV_MODE_FLAG_SAFETY_ARMED;
	// Otherwise if there're any other errors we're in standby
	} else if (systemStatus.reset > 0) {
		mavlink_system.state = MAV_STATE_STANDBY;
		mavlink_system.mode |= MAV_MODE_FLAG_SAFETY_ARMED;
	// Finally we're active if there're no errors. Also indicate within the mode that we're armed.
	} else {
		mavlink_system.state = MAV_STATE_ACTIVE;
		mavlink_system.mode |= MAV_MODE_FLAG_SAFETY_ARMED;
	}

	/// Then we update the system mode using MAV_MODE_FLAGs
	// Set manual/autonomous mode. Note that they're not mutually exclusive within the MAVLink protocol,
	// though I treat them as such for my autopilot.
	if (systemStatus.status & (1 << 0)) {
		mavlink_system.mode |= (MAV_MODE_FLAG_AUTO_ENABLED | MAV_MODE_FLAG_GUIDED_ENABLED);
		mavlink_system.mode &= ~MAV_MODE_FLAG_MANUAL_INPUT_ENABLED;
	} else {
		mavlink_system.mode &= ~(MAV_MODE_FLAG_AUTO_ENABLED | MAV_MODE_FLAG_GUIDED_ENABLED);
		mavlink_system.mode |= MAV_MODE_FLAG_MANUAL_INPUT_ENABLED;
	}
	// Set HIL status
	if (systemStatus.status & (1 << 1)) {
		mavlink_system.mode |= MAV_MODE_FLAG_HIL_ENABLED;
	} else {
		mavlink_system.mode &= ~MAV_MODE_FLAG_HIL_ENABLED;
	}
}

void MavLinkSendWindAirData(void)
{
	mavlink_message_t msg;
	mavlink_msg_wso100_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		windDataStore.speed.flData, windDataStore.direction.flData,
		airDataStore.temp.flData, airDataStore.pressure.flData, airDataStore.humidity.flData);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

void MavLinkSendDst800Data(void)
{
	mavlink_message_t msg;
	mavlink_msg_dst800_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
	                        waterDataStore.speed.flData, waterDataStore.temp.flData, waterDataStore.depth.flData);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

void MavLinkSendRevoGsData(void)
{
	mavlink_message_t msg;
	mavlink_msg_revo_gs_pack(mavlink_system.sysid, mavlink_system.compid, &msg,
		revoDataStore.heading.flData, revoDataStore.magStatus,
		revoDataStore.pitch.flData, revoDataStore.pitchStatus,
		revoDataStore.roll.flData, revoDataStore.rollStatus,
		revoDataStore.dip.flData, revoDataStore.magneticMagnitude.usData);
	len = mavlink_msg_to_send_buffer(buf, &msg);
	uart1EnqueueData(buf, (uint8_t)len);
}

/**
 * Receives a manual control message from QGC and stores the commands from it for use 
 * with the Simulink controller.
 */
void MavLinkReceiveManualControl(mavlink_manual_control_t *msg)
{
	if (msg->target == mavlink_system.sysid) {
		mavlinkManualControlData.unpackedData.X = msg->x;
		mavlinkManualControlData.unpackedData.Z = msg->z;
		mavlinkManualControlData.unpackedData.Buttons = msg->buttons;
		mavlinkManualControlData.unpackedData.NewData = true;
	}
}

void MatlabGetMavLinkManualControl(uint8_t *data)
{
	data[0] = mavlinkManualControlData.packedData[0];
	data[1] = mavlinkManualControlData.packedData[1];
	data[2] = mavlinkManualControlData.packedData[2];
	data[3] = mavlinkManualControlData.packedData[3];
	data[4] = mavlinkManualControlData.packedData[4];
	data[5] = mavlinkManualControlData.packedData[5];
	data[6] = mavlinkManualControlData.packedData[6];
	mavlinkManualControlData.unpackedData.NewData = 0;
}

/** Core MAVLink functions handling transmission and state machines **/

void MavLinkEvaluateParameterState(uint8_t event, void *data)
{
	// Track the parameter protocol state
	static uint8_t state = PARAM_STATE_INACTIVE;

	// Keep a record of the current parameter being used
	static uint8_t currentParameter;

	// Used for the delaying parameter transmission
	static uint8_t delayCountdown = 0;

	// Store the state to change into
	uint8_t nextState = state;

	// First check the parameter protocol state
	switch (state) {
		case PARAM_STATE_INACTIVE:
			if (event == PARAM_EVENT_REQUEST_LIST_RECEIVED) {
				currentParameter = 0;
				nextState = PARAM_STATE_STREAM_SEND_VALUE;
			} else if (event == PARAM_EVENT_SET_RECEIVED) {
				mavlink_param_set_t x = *(mavlink_param_set_t *)data;
				mavlink_param_union_t paramValue;
				paramValue.param_float = x.param_value;
				if (strcmp(x.param_id, "MODE_AUTO") == 0) {
					if (paramValue.param_uint32) {
						systemStatus.status |= (1 << 0);
					} else {
						systemStatus.status &= ~(1 << 0);
					}
					currentParameter = 0;
				} else if (strcmp(x.param_id, "MODE_HIL") == 0) {
					if (paramValue.param_uint32) {
						systemStatus.status |= (1 << 1);
					} else {
						systemStatus.status &= ~(1 << 1);
					}
					currentParameter = 1;
				} else if (strcmp(x.param_id, "MODE_HILSENSE") == 0) {
					if (paramValue.param_uint32) {
						systemStatus.status |= (1 << 2);
					} else {
						systemStatus.status &= ~(1 << 2);
					}
					currentParameter = 2;
				} else if (strcmp(x.param_id, "MODE_RCDISCON") == 0) {
					if (paramValue.param_uint32) {
						systemStatus.status |= (1 << 3);
					} else {
						systemStatus.status &= ~(1 << 3);
					}
					currentParameter = 3;
				}
				nextState = PARAM_STATE_SINGLETON_SEND_VALUE;
			} else if (event == PARAM_EVENT_REQUEST_READ_RECEIVED) {
				currentParameter = *(uint16_t *)data;
				nextState = PARAM_STATE_SINGLETON_SEND_VALUE;
			}
		break;

		case PARAM_STATE_SINGLETON_SEND_VALUE: {
			if (event == PARAM_EVENT_ENTER_STATE) {
				if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_PARAM_VALUE)) {
					while (1);
				}
			} else if (event == PARAM_EVENT_VALUE_DISPATCHED) {
				_transmitParameter(currentParameter);
				nextState = PARAM_STATE_INACTIVE;
			}
		} break;

		case PARAM_STATE_STREAM_SEND_VALUE: {
			if (event == PARAM_EVENT_ENTER_STATE) {
				if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_PARAM_VALUE)) {
					while (1);
				}
			} else if (event == PARAM_EVENT_VALUE_DISPATCHED) {
				_transmitParameter(currentParameter);

				// And increment the current parameter index for the next iteration and
				// we finish if we've hit the limit of parameters.
				if (++currentParameter == parameterCount) {
					nextState = PARAM_STATE_INACTIVE;
				} else {
					nextState = PARAM_STATE_STREAM_DELAY;
				}
			}
		} break;

		// Add a delay of 10 timesteps before attempting to schedule another one
		case PARAM_STATE_STREAM_DELAY: {
			if (event == PARAM_EVENT_ENTER_STATE) {
					delayCountdown = 0;
			} else if (event == PARAM_EVENT_NONE) {
				if (++delayCountdown == 10) {
					nextState = PARAM_STATE_STREAM_SEND_VALUE;
				}
			}
		} break;

		default: break;
	}

	// Here is when we actually transition between states, calling init/exit code as necessary
	if (nextState != state) {
		MavLinkEvaluateParameterState(PARAM_EVENT_EXIT_STATE, NULL);
		state = nextState;
		MavLinkEvaluateParameterState(PARAM_EVENT_ENTER_STATE, NULL);
	}
}

/**
 * This function implements the mission protocol state machine for the MAVLink protocol.
 * events can be passed as the first argument, or NO_EVENT if desired. data is a pointer
 * to data if there is any to be passed to the state logic. data is not guaranteed to persist
 * beyond the single call to this function.
 */
void MavLinkEvaluateMissionState(uint8_t event, void *data)
{
	// Internal counter variable for use with the COUNTDOWN state
	static uint16_t counter = 0;

	// Keep track of the expected length of the incoming mission list
	static uint16_t mavlinkNewMissionListSize;

	// Track a mission index for some multi-state loops.
	static uint8_t currentMissionIndex;

	// Track the state
	static uint8_t state = MISSION_STATE_INACTIVE;

	// Keep track of the next state to transition into
	uint8_t nextState = state;

	// Then check the mission protocol state
	switch (state) {
		case MISSION_STATE_INACTIVE:
			if (event == MISSION_EVENT_REQUEST_LIST_RECEIVED) {
				if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_COUNT)) {
					while (1);
				}
				currentMissionIndex = 0;
				nextState = MISSION_STATE_SEND_MISSION_COUNT;
			} else if (event == MISSION_EVENT_COUNT_RECEIVED) {
				// Don't allow for writing of new missions if we're in autonomous mode.
				if ((systemStatus.status & 0x0001) > 0) {
					if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ACK)) {
						while (1);
					}
					nextState = MISSION_STATE_ACK_ERROR;
				} else {

					uint8_t newListSize = *(uint8_t *)data;

					// Only respond with a request if there are missions to request.
					// If we received a 0-length mission list, just respond with a MISSION_ACK error
					if (newListSize == 0) {
						if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ACK)) {
							while (1);
						}
						nextState = MISSION_STATE_ACK_ERROR;
					} else if (newListSize > mList.maxSize) {
						if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ACK)) {
							while (1);
						}
						nextState = MISSION_STATE_ACK_NO_SPACE;
					}
					// Otherwise we're set to start retrieving a new mission list so we request the first mission.
					else {
						mavlinkNewMissionListSize = newListSize;
						ClearMissionList();
						currentMissionIndex = 0;
						if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_REQUEST)) {
							while (1);
						}
						nextState = MISSION_STATE_SEND_REQUEST;
					}
				}
			} else if (event == MISSION_EVENT_CLEAR_ALL_RECEIVED) {
				// If we're in autonomous mode, don't allow for clearing the mission list
				if ((systemStatus.status & 0x0001) > 0) {
					if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ACK)) {
						while (1);
					}
					nextState = MISSION_STATE_ACK_ERROR;
				}
				// But if we're in manual mode, go ahead and clear everything.
				else {
					ClearMissionList();
					if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ACK)) {
						while (1);
					}
					nextState = MISSION_STATE_ACK_ACCEPTED;
				}
			} else if (event == MISSION_EVENT_SET_CURRENT_RECEIVED) {
				SetCurrentMission(*(uint8_t*)data);
				if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_CURRENT)) {
					while (1);
				}
				nextState = MISSION_STATE_SEND_CURRENT;
			}
			// If a MISSION_CURRENT message was scheduled and we aren't in any state, just transmit
			// it. This is likely due to a waypoint being reached.
			else if (event == MISSION_EVENT_CURRENT_DISPATCHED) {
				MavLinkSendCurrentMission();
			}
		break;

		case MISSION_STATE_SEND_MISSION_COUNT:
			if (event == MISSION_EVENT_COUNT_DISPATCHED) {
				MavLinkSendMissionCount();
				nextState = MISSION_STATE_COUNTDOWN;
			}
		break;

		case MISSION_STATE_COUNTDOWN:
			if (event == MISSION_EVENT_ENTER_STATE) {
				counter = 0;
			} else if (event == MISSION_EVENT_NONE) {
				if (counter++ > 400) {
					nextState = MISSION_STATE_INACTIVE;
				}
			} else if (event == MISSION_EVENT_REQUEST_RECEIVED) {
				if (*(uint8_t *)data == currentMissionIndex) {
					if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ITEM)) {
						while (1);
					}
					nextState = MISSION_STATE_SEND_MISSION_ITEM;
				}
			} else if (event == MISSION_EVENT_ACK_RECEIVED) {
				nextState = MISSION_STATE_INACTIVE;
			} else if (event == MISSION_EVENT_ITEM_RECEIVED) {

				mavlink_mission_item_t incomingMission = *(mavlink_mission_item_t *)data;

				// Make sure that they're coming in in the right order, and if they don't return an error in
				// the acknowledgment response.
				if (currentMissionIndex == incomingMission.seq) {
					Mission m = {
						{
							incomingMission.x,
							incomingMission.y,
							incomingMission.z
						},
						incomingMission.frame,
						incomingMission.command,
						{
							incomingMission.param1,
							incomingMission.param2,
							incomingMission.param3,
							incomingMission.param4
						},
						incomingMission.autocontinue
					};

					// Attempt to record this mission to the list, recording the result, which will be 0 for failure.
					// We also map all incoming Global Lat/Long/Alt messages to North-East-Down here.
					// These can be created in QGroundControl by just double-clicking on the Map. Once you write them
					// to this controller, they'll pop back out as NED.
					int8_t missionAddStatus;
					if (m.refFrame == MAV_FRAME_GLOBAL) {
						Mission convertedLocalMission = {
							{0.0, 0.0, 0.0},
							MAV_FRAME_LOCAL_NED,
							m.action,
							{m.parameters[0], m.parameters[1], m.parameters[2], m.parameters[3]},
							m.autocontinue
						};
						const int32_t x[3] = {
							(int32_t)(m.coordinates[0] * 1e7),
							(int32_t)(m.coordinates[1] * 1e7),
							(int32_t)(m.coordinates[2] * 1e7)
						};
						primary_node_LLA2LTP(true, x, convertedLocalMission.coordinates);

						AppendMission(&convertedLocalMission, &missionAddStatus);
					} else {
						AppendMission(&m, &missionAddStatus);
					}

					if (missionAddStatus != -1) {
						// If this is going to be the new current mission, then we should set it as such.
						if (incomingMission.current) {
							SetCurrentMission(incomingMission.seq);
						}

						// If this was the last mission we were expecting, respond with an ACK confirming that we've successfully
						// received the entire mission list. Otherwise we just increment and request the next mission.
						if (currentMissionIndex == mavlinkNewMissionListSize - 1) {
							if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ACK)) {
								while (1);
							}
							nextState = MISSION_STATE_ACK_ACCEPTED;
						} else {
							++currentMissionIndex;
							if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_REQUEST)) {
								while (1);
							}
							nextState = MISSION_STATE_SEND_REQUEST;
						}
					}
					// If we've run out of space before the last message, respond saying so.
					else {
						if (!AddMessageOnce(&mavlinkSchedule, MAVLINK_MSG_ID_MISSION_ACK)) {
							while (1);
						}
						nextState = MISSION_STATE_ACK_NO_SPACE;
					}
				}
			}
		break;

		case MISSION_STATE_SEND_MISSION_ITEM:
			if (event == MISSION_EVENT_ITEM_DISPATCHED) {
				MavLinkSendMissionItem(currentMissionIndex);
				++currentMissionIndex;
				nextState = MISSION_STATE_COUNTDOWN;
			}
		break;

		case MISSION_STATE_SEND_CURRENT:
			if (event == MISSION_EVENT_CURRENT_DISPATCHED) {
				MavLinkSendCurrentMission();
				nextState = MISSION_STATE_INACTIVE;
			}
		break;

		case MISSION_STATE_ACK_NO_SPACE:
			if (event == MISSION_EVENT_ACK_DISPATCHED) {
				MavLinkSendMissionAck(MAV_MISSION_NO_SPACE);
				nextState = MISSION_STATE_INACTIVE;
			}
		break;

		case MISSION_STATE_ACK_ERROR:
			if (event == MISSION_EVENT_ACK_DISPATCHED) {
				MavLinkSendMissionAck(MAV_MISSION_ERROR);
				nextState = MISSION_STATE_INACTIVE;
			}
		break;

		case MISSION_STATE_ACK_ACCEPTED:
			if (event == MISSION_EVENT_ACK_DISPATCHED) {
				MavLinkSendMissionAck(MAV_MISSION_ACCEPTED);
				nextState = MISSION_STATE_INACTIVE;
			}
		break;

		case MISSION_STATE_ACK_INVALID_SEQUENCE:
			if (event == MISSION_EVENT_ACK_DISPATCHED) {
				MavLinkSendMissionAck(MAV_MISSION_INVALID_SEQUENCE);
				nextState = MISSION_STATE_INACTIVE;
			}
		break;

		case MISSION_STATE_SEND_REQUEST: {
			if (event == MISSION_EVENT_REQUEST_DISPATCHED) {
				MavLinkSendMissionRequest(currentMissionIndex);
				nextState = MISSION_STATE_COUNTDOWN;
			}
		} break;
	}

	// Here is when we actually transition between states, calling init/exit code as necessary
	if (nextState != state) {
		MavLinkEvaluateMissionState(MISSION_EVENT_EXIT_STATE, NULL);
		state = nextState;
		MavLinkEvaluateMissionState(MISSION_EVENT_ENTER_STATE, NULL);
	}
}

/**
* @brief Receive communication packets and handle them. Should be called at the system sample rate.
*
* This function decodes packets on the protocol level and also handles
* their value by calling the appropriate functions.
*/
void MavLinkReceive(void)
{
	mavlink_message_t msg;
	mavlink_status_t status;

	// Track whether we actually handled any data in this function call.
	// Used for updating the number of MAVLink messages handled
	bool processedData = false;

	while (uart1RxBuffer.dataSize > 0) {
		processedData = true;
		uint8_t c;
		CB_ReadByte(&uart1RxBuffer, &c);
		// Parse another byte and if there's a message found process it.
		if (mavlink_parse_char(MAVLINK_COMM_0, c, &msg, &status)) {

			// Latch the groundstation system and component ID if we haven't yet.
			if (!groundStationSystemId && !groundStationComponentId) {
				groundStationSystemId = msg.sysid;
				groundStationComponentId = msg.compid;
			}

			switch(msg.msgid) {

				// Check for manual commands via Joystick from QGC.
				case MAVLINK_MSG_ID_MANUAL_CONTROL: {
					mavlink_manual_control_t manualControl;
					mavlink_msg_manual_control_decode(&msg, &manualControl);
					MavLinkReceiveManualControl(&manualControl);
				} break;

				// If we are not doing any mission protocol operations, record the size of the incoming mission
				// list and transition into the write missions state machine loop.
				case MAVLINK_MSG_ID_MISSION_COUNT: {
					uint8_t mavlinkNewMissionListSize = mavlink_msg_mission_count_get_count(&msg);
					MavLinkEvaluateMissionState(MISSION_EVENT_COUNT_RECEIVED, &mavlinkNewMissionListSize);
				} break;

				// Handle receiving a mission.
				case MAVLINK_MSG_ID_MISSION_ITEM: {
					mavlink_mission_item_t currentMission;
					mavlink_msg_mission_item_decode(&msg, &currentMission);
					MavLinkEvaluateMissionState(MISSION_EVENT_ITEM_RECEIVED, &currentMission);
				} break;

				// Responding to a mission request entails moving into the first active state and scheduling a MISSION_COUNT message
				case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:
					MavLinkEvaluateMissionState(MISSION_EVENT_REQUEST_LIST_RECEIVED, NULL);
				break;

				// When a mission request message is received, respond with that mission information from the MissionManager
				case MAVLINK_MSG_ID_MISSION_REQUEST: {
					uint8_t receivedMissionIndex = mavlink_msg_mission_request_get_seq(&msg);
					MavLinkEvaluateMissionState(MISSION_EVENT_REQUEST_RECEIVED, &receivedMissionIndex);
				} break;

				// Allow for clearing waypoints. Here we respond simply with an ACK message if we successfully
				// cleared the mission list.
				case MAVLINK_MSG_ID_MISSION_CLEAR_ALL:
					MavLinkEvaluateMissionState(MISSION_EVENT_CLEAR_ALL_RECEIVED, NULL);
				break;

				// Allow for the groundstation to set the current mission. This requires a WAYPOINT_CURRENT response message agreeing with the received current message index.
				case MAVLINK_MSG_ID_MISSION_SET_CURRENT: {
					uint8_t newCurrentMission = mavlink_msg_mission_set_current_get_seq(&msg);
					MavLinkEvaluateMissionState(MISSION_EVENT_SET_CURRENT_RECEIVED, &newCurrentMission);
				} break;

				case MAVLINK_MSG_ID_MISSION_ACK: {
					mavlink_msg_mission_ack_get_type(&msg);
					MavLinkEvaluateMissionState(MISSION_EVENT_ACK_RECEIVED, NULL);
				} break;

				// If they're requesting a list of all parameters, call a separate function that'll track the state and transmit the necessary messages.
				// This reason that this is an external function is so that it can be run separately at 20Hz.
				case MAVLINK_MSG_ID_PARAM_REQUEST_LIST: {
					MavLinkEvaluateParameterState(PARAM_EVENT_REQUEST_LIST_RECEIVED, NULL);
				} break;

				// If a request comes for a single parameter then set that to be the current parameter and move into the proper state.
				case MAVLINK_MSG_ID_PARAM_REQUEST_READ: {
					uint16_t currentParameter = mavlink_msg_param_request_read_get_param_index(&msg);
					MavLinkEvaluateParameterState(PARAM_EVENT_REQUEST_READ_RECEIVED, &currentParameter);
				} break;

				case MAVLINK_MSG_ID_PARAM_SET: {
					mavlink_param_set_t x;
					mavlink_msg_param_set_decode(&msg, &x);
					MavLinkEvaluateParameterState(PARAM_EVENT_SET_RECEIVED, &x);
				} break;

				default: break;
			}
		}
	}

	// Update the number of messages received, both successful and not. Note that the 'status' variable
	// will be updated on every call to *_parse_char(), so this will always be a valid value.
	if (processedData) {
		mavLinkMessagesReceived = status.packet_rx_success_count;
		mavLinkMessagesFailedParsing = status.packet_rx_drop_count;
	}
}

/**
 * This function handles transmission of MavLink messages taking into account transmission
 * speed, message size, and desired transmission rate.
 */
void MavLinkTransmit(void)
{

	// And now transmit all messages for this timestep
        uint8_t msgs[MAVLINK_MSGS_SIZE];
        uint8_t count = GetMessagesForTimestep(&mavlinkSchedule, msgs);
        int i;
	for (i = 0; i < count; ++i) {

		switch(msgs[i]) {

			/** Common Messages **/

			case MAVLINK_MSG_ID_HEARTBEAT: {
				MavLinkSendHeartbeat();
			} break;

			case MAVLINK_MSG_ID_SYSTEM_TIME: {
				MavLinkSendSystemTime();
			} break;

			case MAVLINK_MSG_ID_SYS_STATUS: {
				MavLinkSendStatus();
			} break;

			case MAVLINK_MSG_ID_ATTITUDE: {
				MavLinkSendAttitude();
			} break;

			case MAVLINK_MSG_ID_LOCAL_POSITION_NED: {
				MavLinkSendLocalPosition();
			} break;

			case MAVLINK_MSG_ID_GPS_RAW_INT: {
				MavLinkSendRawGps();
			} break;

			case MAVLINK_MSG_ID_GPS_GLOBAL_ORIGIN:
				MavLinkSendGpsGlobalOrigin();
			break;

			case MAVLINK_MSG_ID_RC_CHANNELS_SCALED: {
				MavLinkSendRcScaledData();
			} break;

			/** Parameter Protocol Messages **/

			case MAVLINK_MSG_ID_PARAM_VALUE: {
				MavLinkEvaluateParameterState(PARAM_EVENT_VALUE_DISPATCHED, NULL);
			} break;

			/** Mission Protocol Messages **/

			case MAVLINK_MSG_ID_MISSION_COUNT: {
				MavLinkEvaluateMissionState(MISSION_EVENT_COUNT_DISPATCHED, NULL);
			} break;

			case MAVLINK_MSG_ID_MISSION_ITEM:
				MavLinkEvaluateMissionState(MISSION_EVENT_ITEM_DISPATCHED, NULL);
			break;

			case MAVLINK_MSG_ID_MISSION_REQUEST: {
				MavLinkEvaluateMissionState(MISSION_EVENT_REQUEST_DISPATCHED, NULL);
			} break;

			case MAVLINK_MSG_ID_MISSION_CURRENT: {
				MavLinkEvaluateMissionState(MISSION_EVENT_CURRENT_DISPATCHED, NULL);
			} break;

			case MAVLINK_MSG_ID_MISSION_ACK: {
				MavLinkEvaluateMissionState(MISSION_EVENT_ACK_DISPATCHED, NULL);
			} break;

			/** Sealion Messages **/

			case MAVLINK_MSG_ID_STATUS_AND_ERRORS: {
				MavLinkSendStatusAndErrors();
			} break;

			case MAVLINK_MSG_ID_WSO100: {
				MavLinkSendWindAirData();
			} break;

			case MAVLINK_MSG_ID_BASIC_STATE:
				MavLinkSendBasicState();
			break;

			case MAVLINK_MSG_ID_RUDDER_RAW:
				MavLinkSendRudderRaw();
			break;

			case MAVLINK_MSG_ID_DST800:
				MavLinkSendDst800Data();
			break;

			case MAVLINK_MSG_ID_REVO_GS:
				MavLinkSendRevoGsData();
			break;

			case MAVLINK_MSG_ID_MAIN_POWER:
				MavLinkSendMainPower();
			break;

			default: {

			} break;
		}
	}
}
