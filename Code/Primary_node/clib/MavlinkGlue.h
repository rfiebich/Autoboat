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

#ifndef MAVLINK_GLUE_H
#define MAVLINK_GLUE_H

#include "MissionManager.h"
#include "primary_node.h"
#include "Types.h"
#include "mavlink.h"

/**
 * Initialize MAVLink transmission. This just sets up the MAVLink scheduler with the basic
 * repeatedly-transmit messages.
 */
void MavLinkInit(void);

/**
 * Simulink helper function that schedules a one-off MISSION_CURRENT message.
 */
void MavLinkScheduleCurrentMission(void);

/**
 * Simulink helper function that scheduls a one-off GPS_GLOBAL_ORIGIN message.
 */
void MavLinkScheduleGpsOrigin(void);

/**
 * This function creates a MAVLink heartbeat message with some basic parameters and
 * caches that message (along with its size) in the module-level variables declared
 * above. This buffer should be transmit at 1Hz back to the groundstation.
 */
void MavLinkSendHeartbeat(void);

/**
 * This function transmits the system time. Looks like it's necessary for QGC to
 * record timestamps on data reliably. For some reason it doesn't just use the local
 * time of message reception. Hopefully this fixes that.
 * This message is only transmitted if there actually is a global clock to sync with.
 */
void MavLinkSendSystemTime(void);

/**
 * This function transmits a MAVLink SYS_STATUS message. It relies on various external information such as sensor/actuator status
 * from ecanSensors.h, the internalVariables struct exported by Simulink, and the drop rate calculated within ecanSensors.c.
 */
void MavLinkSendStatus(void);

/**
 * Pull the raw GPS sensor data from the gpsDataStore struct within the GPS module and
 * transmit it via MAVLink over UART1.
 * TODO: Convert this message to a GLOBAL_POSITION_INT
 */
void MavLinkSendRawGps(void);

/**
  * Transmit the main battery state as obtained from the power node via the CAN bus.
  */
void MavLinkSendMainPower(void);


/**
 * Transmits the custom BASIC_STATE message. This just transmits a bunch of random variables
 * that are good to know but arbitrarily grouped.
 */
void MavLinkSendBasicState(void);

/**
 * Transmits the vehicle attitude. Right now just the yaw value.
 * Expects systemStatus.time to be in centiseconds which are then converted
 * to ms for transmission.
 * Yaw should be in radians where positive is eastward from north.
 */
void MavLinkSendAttitude(void);

/**
 * This function takes in the local position and local velocity (as 3-tuples) from
 * Matlab as real32s and ships them off over a MAVLink MAVLINK_MSG_ID_LOCAL_POSITION_NED
 * message.
 * It also expects a systemStatus struct with a uint32_t time element that holds the
 * current system time in centiseconds.
 */
void MavLinkSendLocalPosition(void);

/**
 * Only transmit scaled manual control data messages if manual control is enabled OR if 
 * the RC transmitter is enabled as the RC transmitter overrides everything.
 */
void MavLinkSendRcScaledData(void);

/**
 * Transmits the current GPS position of the origin of the local coordinate frame that the North-East-Down
 * coordinates are all relative too. They should be in units of 1e-7 degrees.
 */
void MavLinkSendGpsGlobalOrigin(void);

/**
 * Transmit the current mission index via UART1. GetCurrentMission returns a -1 if there're no missions,
 * so we check and only transmit valid current missions.
 */
void MavLinkSendCurrentMission(void);

/**
 * Transmit a mission acknowledgement message. The type of message is the sole argument to this
 * function (see enum MAV_MISSIONRESULT).
 */
void MavLinkSendMissionAck(uint8_t type);

void MavLinkSendMissionCount(void);

void MavLinkSendMissionItem(uint8_t currentMissionIndex);

void MavLinkSendMissionRequest(uint8_t currentMissionIndex);

/**
 * The following functions are helper functions for reading the various parameters aboard the boat.
 */
void _transmitParameter(uint16_t id);

/** Custom Sealion Messages **/

void MavLinkSendRudderRaw(void);

void MavLinkSendStatusAndErrors(void);

void MavLinkSendWindAirData(void);

void MavLinkSendDst800Data(void);

void MavLinkSendRevoGsData(void);

/**
 * Receives a manual control message from QGC and stores the commands from it for use 
 * with the Simulink controller.
 */
void MavLinkReceiveManualControl(mavlink_manual_control_t *msg);

void MatlabGetMavLinkManualControl(uint8_t *data);

/** Core MAVLink functions handling transmission and state machines **/

void MavLinkEvaluateParameterState(uint8_t event, void *data);

/**
 * This function implements the mission protocol state machine for the MAVLink protocol.
 * events can be passed as the first argument, or NO_EVENT if desired. data is a pointer
 * to data if there is any to be passed to the state logic. data is not guaranteed to persist
 * beyond the single call to this function.
 */
void MavLinkEvaluateMissionState(uint8_t event, void *data);

/**
* @brief Receive communication packets and handle them. Should be called at the system sample rate.
*
* This function decodes packets on the protocol level and also handles
* their value by calling the appropriate functions.
*/
void MavLinkReceive(void);

/**
 * This function handles transmission of MavLink messages taking into account transmission
 * speed, message size, and desired transmission rate.
 */
void MavLinkTransmit(void);

#endif // MAVLINK_GLUE_H