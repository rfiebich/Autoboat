#include "Nmea2000.h"
#include "Nmea2000Encode.h"
#include "ecanDefinitions.h"
#include <math.h>

void PackagePgn127245(tCanMessage *msg, uint8_t sourceDevice, uint8_t instance, uint8_t dirOrder, float angleOrder, float position)
{
	/// Set CAN header information.
	msg->id = Iso11783Encode(PGN_RUDDER, sourceDevice, 0xFF, 2); // Leave the priority hardcoded to 2.
	msg->buffer = 0; // NOTE: This needs to be changed to the appropriate buffer before transmission.
	msg->message_type = CAN_MSG_DATA;
	msg->frame_type = CAN_FRAME_EXT;
	msg->validBytes = 6;

	/// Now fill in the data.
	msg->payload[0] = instance;
	msg->payload[1] = 0xFC | dirOrder;
	// Convert commanded rudder angle to 1e-4 radians
	int16_t angle;
	if (angleOrder == angleOrder) {
		angle = (int16_t)(angleOrder * 10000);
	} else {
		angle = 0xFFFF;
	}
	// Send current angle over the CAN bus
	msg->payload[2] = angle;
	msg->payload[3] = angle >> 8;
	// Convert current rudder angle to 1e-4 radians
	if (position == position) {
		angle = (int16_t)(position * 10000);
	} else {
		angle = 0xFFFF;
	}
	// Send current angle over the CAN bus
	msg->payload[4] = angle;
	msg->payload[5] = angle >> 8;
}

void PackagePgn127508(tCanMessage *msg, uint8_t sourceDevice, uint8_t battInstance, float voltage, float amperage, float temp, uint8_t sid)
{
	msg->id = Iso11783Encode(PGN_BATTERY_STATUS, sourceDevice, 0xFF, 3);
	msg->message_type = CAN_MSG_DATA;
	msg->frame_type = CAN_FRAME_EXT;
	msg->buffer = 0;
	msg->validBytes = 8;

	// Field 0: Battery instance
	msg->payload[0] = battInstance;

	// Field 1: Voltage (in .01V). Check that it's a valid voltage (basically NOT NaN).
	uint16_t x = 0xFFFF;
	if (voltage == voltage) {
		voltage *= 100.0f;
		x = (uint16_t)voltage;
	}
	msg->payload[1] = (uint8_t)x;
	msg->payload[2] = (uint8_t)(x >> 8);

	// Field 2: Current (in .1A)
	x = 0xFFFF;
	if (amperage == amperage) {
		amperage *= 10.0f;
		x = (uint16_t)amperage;
	}
	msg->payload[3] = (uint8_t)x;
	msg->payload[4] = (uint8_t)(x >> 8);

	// Field 3: Temperature (in 1K)
	// All 1s indicated no-measurement
	x = 0xFFFF;
	if (temp == temp) {
		temp = (temp + 273.15) * 100;
		x = (uint16_t)temp;
	}
	msg->payload[5] = (uint8_t)x;
	msg->payload[6] = (uint8_t)(x >> 8);

	// Field 4: Sequence ID
	msg->payload[7] = sid;
}

void PackagePgn130311(tCanMessage *msg, uint8_t sourceDevice, uint8_t sid, uint8_t tempInst, uint8_t humidInst, float temp, float humid, float press)
{
    // Specify a new CAN message w/ metadata
    msg->id = Iso11783Encode(PGN_ENV_PARAMETERS2, sourceDevice, 0xFF, 2);
    msg->buffer = 0;
    msg->message_type = CAN_MSG_DATA;
    msg->frame_type = CAN_FRAME_EXT;
    msg->validBytes = 8;

    // Now set the data.
    msg->payload[0] = sid;      // SID
    msg->payload[1] = tempInst & 0x03;           // Temp instance
    msg->payload[1] |= (humidInst & 0x03F) << 6; // Humidity instance
    // Convert temperature from Celius to units of .01Kelvin.
    uint16_t tempConverted = (temp == temp)?(uint16_t)((temp + 273.15) * 100):0xFFFF;
    msg->payload[2] = (uint8_t)tempConverted;
    msg->payload[3] = (uint8_t)(tempConverted >> 8);
    // Convert humidity from % to 0.004 %.
    uint16_t humidConverted = (humid == humid)?(uint16_t)(humid * 250):0xFFFF;
    msg->payload[4] = (uint8_t)humidConverted;
    msg->payload[5] = (uint8_t)(humidConverted >> 8);
    // Convert pressure from kPa and record as hPa.
    uint16_t pressConverted = (press == press)?(uint16_t)(press * 10):0xFFFF;
    msg->payload[6] = (uint8_t)pressConverted;
    msg->payload[7] = (uint8_t)(pressConverted >> 8);
}