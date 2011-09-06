/*
The MIT License

Copyright (c) 2010 UCSC Autonomous Systems Lab

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

*/

// ==============================================================
// This code provides a protocol decoder for the binary communications
// protocol used between the groundstation/HIL and the dsPIC in
// the Autoboat project. As most programming relies on Simulink and
// the Real-Time Workshop, retrieval functions here return arrays of
// data to be compatible (instead of much-nicer structs).
// A complete structure is passed byte-by-byte and assembled in an 
// internal buffer. This is then verified by its checksum and the 
//data pushed into the appropriate struct. This data can then be 
// retrieved via an accessor function.
//
// While this code was written specifically for the Autoboat and its
// protocol, it has been kept as modular as possible to be useful
// in other situations with the most minimal alterations.
// 
// Code by: Bryant W. Mairs
// First Revision: Aug 25 2010
// ==============================================================

#ifndef _COMMPROTOCOL_H_
#define _COMMPROTOCOL_H_

// Definitions of unions useful in transmitting data serially
typedef union {
	unsigned char    chData[2];
	unsigned short   usData;
} tUnsignedShortToChar; 

typedef union {
	unsigned char    chData[2];
 	short   		 shData;
} tShortToChar; 

typedef union {
	unsigned char   chData[4];
 	unsigned int   	uiData;
} tUnsignedIntToChar; 

typedef union {
	unsigned char   chData[4];
 	int   			inData;
} tIntToChar; 

typedef union {
	unsigned char   chData[4];
 	float   		flData;
	unsigned short	shData[2];
} tFloatToChar;

// Declaration of the relevant message structs used.
typedef struct {
	tShortToChar speed;
	tFloatToChar lat;
	tFloatToChar lon;
	tFloatToChar alt;
	unsigned char year;
	unsigned char month;
	unsigned char day;
	unsigned char hour;
	unsigned char minute;
	unsigned char second;
	tFloatToChar cog;
	tFloatToChar sog;
	unsigned char newGpsData;
	tUnsignedShortToChar r_Position;
	unsigned char r_SBLimit;
	unsigned char r_PortLimit;
	tUnsignedShortToChar b_Position;
	unsigned char b_SBLimit;
	unsigned char b_PortLimit;
	unsigned char newData;
	tUnsignedShortToChar timestamp;
} tSensorData;

typedef struct {
	unsigned char r_enable;
	unsigned char r_direction;
	tUnsignedShortToChar r_up;
	tUnsignedShortToChar r_period;
	unsigned char b_enable;
	unsigned char b_direction;
	tUnsignedIntToChar t_identifier;
	unsigned char data[6];
	unsigned char size;
	unsigned char trigger;
	tUnsignedShortToChar timestamp;
	tFloatToChar rudderAngle;
	unsigned char sensorOverride;
} tActuatorData;

/**
 * This function initializes all onboard UART communications
 */
void cpInitCommunications();

/**
 * This function builds a full message internally byte-by-byte,
 * verifies its checksum, and then pushes that data into the
 * appropriate struct.
 */
void buildAndCheckMessage(unsigned char characterIn);

void processNewCommData(unsigned char* message);

/**
 * The following functions change the UART2 baud rate to allow
 * for HIL mode (running at 115200baud) and back to the old baud rate.
 */
void setHilMode(unsigned char mode);

inline void enableHil();

inline void disableHil();

/**
 * This function calculates the checksum of some bytes in an
 * array by XORing all of them.
 */
unsigned char calculateChecksum(unsigned char* sentence, unsigned char size);

/**
 * Manage the sensor data struct.
 */
void setSensorData(unsigned char* data);

void getSensorData(unsigned char* data);

void clearSensorData();

/**
 * Manage the actuator data struct
 */
void setActuatorData(unsigned char* data);

void getActuatorData(unsigned char* data);

inline void uart2EnqueueActuatorData(unsigned char *data);

inline void uart1EnqueueStateData(unsigned char *data);

#endif /* _COMMPROTOCOL_H_ */
