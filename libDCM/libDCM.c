// This file is part of MatrixPilot.
//
//    http://code.google.com/p/gentlenav/
//
// Copyright 2009-2011 MatrixPilot Team
// See the AUTHORS.TXT file for a list of authors of MatrixPilot.
//
// MatrixPilot is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// MatrixPilot is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with MatrixPilot.  If not, see <http://www.gnu.org/licenses/>.


#include "libDCM_internal.h"
#include "gpsParseCommon.h"
#include "../libUDB/heartbeat.h"
#include "../libUDB/magnetometer.h"
#include "../libUDB/barometer.h"
#include "estAltitude.h"
#include "mathlibNAV.h"
#include "rmat.h"


union dcm_fbts_word dcm_flags;

// Calibrate for 10 seconds before moving servos
#define CALIB_COUNT  400    // 10 seconds at 40 Hz
#define GPS_COUNT    1000   // 25 seconds at 40 Hz

#if (HILSIM == 1)
#if (USE_VARIABLE_HILSIM_CHANNELS != 1)
uint8_t SIMservoOutputs[] = {
	0xFF, 0xEE, //sync
	0x03, 0x04, //S1
	0x05, 0x06, //S2
	0x07, 0x08, //S3
	0x09, 0x0A, //S4
	0x0B, 0x0C, //S5
	0x0D, 0x0E, //S6
	0x0F, 0x10, //S7
	0x11, 0x12, //S8
	0x13, 0x14  //checksum
};
#define HILSIM_NUM_SERVOS 8
#else
#define HILSIM_NUM_SERVOS NUM_OUTPUTS
uint8_t SIMservoOutputs[(NUM_OUTPUTS*2) + 5] = {
	0xFE, 0xEF, // sync
	0x00        // output count
	            // Two checksum on the end
};
#endif // USE_VARIABLE_HILSIM_CHANNELS

void send_HILSIM_outputs(void);
#endif // HILSIM


void dcm_init(void)
{
	dcm_flags.W = 0;
	dcm_flags._.first_mag_reading = 1;
	dcm_init_rmat();
}

void dcm_run_init_step(uint16_t count)
{
	if (count == CALIB_COUNT)
	{
		// Finish calibration
		DPRINT("calib_finished\r\n");
		dcm_flags._.calib_finished = 1;
		dcm_calibrate();
	}

	if (count <= GPS_COUNT)
	{
		gps_startup_sequence(GPS_COUNT - count); // Counts down from GPS_COUNT to 0
		if (count == GPS_COUNT)
		{
			DPRINT("init_finished\r\n");
			dcm_flags._.init_finished = 1;
		}
	}
}

#if (BAROMETER_ALTITUDE == 1)

// We want to be reading both the magnetometer and the barometer at 4Hz
// The magnetometer driver returns a new result via the callback on each call
// The barometer driver needs to be called several times to get a single 
//  result set via the callback. Also on first invocation the barometer driver
//  reads calibration data, and hence requires one extra call

void do_I2C_stuff(void)
{
	static int toggle = 0;
	static int counter = 0;

	if (toggle) {
		if (counter++ > 0) {
//#if (MAG_YAW_DRIFT == 1 && HILSIM != 1)
#if (MAG_YAW_DRIFT == 1)
//			printf("rxMag %u\r\n", udb_heartbeat_counter);
			rxMagnetometer(udb_magnetometer_callback);
#endif
			counter = 0;
			toggle = 0;
		}
	} else {
		rxBarometer(udb_barometer_callback);
		if (counter++ > 6) {
			counter = 0;
			toggle = 1;
		}
	}
}
#endif // BAROMETER_ALTITUDE

// Called at HEARTBEAT_HZ
void udb_heartbeat_callback(void)
{
#if (BAROMETER_ALTITUDE == 1)
	if (udb_heartbeat_counter % (HEARTBEAT_HZ / 40) == 0)
	{
		do_I2C_stuff(); // TODO: this should always be be called at 40Hz
	}
#else
//#if (MAG_YAW_DRIFT == 1 && HILSIM != 1)
#if (MAG_YAW_DRIFT == 1)
	// This is a simple counter to do stuff at 4hz
//	if (udb_heartbeat_counter % 10 == 0)
	if (udb_heartbeat_counter % (HEARTBEAT_HZ / 4) == 0)
	{
		rxMagnetometer(udb_magnetometer_callback);
	}
#endif
#endif // BAROMETER_ALTITUDE

//  when we move the IMU step to the MPU call back, to run at 200 Hz, remove this
	if (dcm_flags._.calib_finished)
	{
		dcm_run_imu_step();
	}

	dcm_heartbeat_callback();    // this was called dcm_servo_callback_prepare_outputs();

	if (!dcm_flags._.init_finished)
	{
		if (udb_heartbeat_counter % (HEARTBEAT_HZ / 40) == 0)
		{
			dcm_run_init_step(udb_heartbeat_counter / (HEARTBEAT_HZ / 40));
		}
	}

#if (HILSIM == 1)
	send_HILSIM_outputs();
#endif
}

// dcm_calibrate is called twice during the startup sequence.
// Firstly 10 seconds after startup, then immediately before the first waggle, which is 10 seconds after getting radio link.  
// This makes sure we get initialized when there's no radio, or when bench testing, 
// and 2nd time is at a time the user knows to keep the plane steady before a flight.
void dcm_calibrate(void)
{
	// Don't allow re/calibrating before the initial calibration period has finished
	if (dcm_flags._.calib_finished)
	{
		udb_a2d_record_offsets();
	}
}

void dcm_set_origin_location(int32_t o_lon, int32_t o_lat, int32_t o_alt)
{
	union longbbbb accum_nav;

	lat_origin.WW = o_lat;
	lon_origin.WW = o_lon;
	alt_origin.WW = o_alt;

	// scale the latitude from GPS units to gentleNAV units
	accum_nav.WW = __builtin_mulss(LONGDEG_2_BYTECIR, lat_origin._.W1);

	unsigned char lat_cir;
	lat_cir = accum_nav.__.B2;
	// estimate the cosine of the latitude, which is used later computing desired course
	cos_lat = cosine(lat_cir);
}

struct relative3D dcm_absolute_to_relative(struct waypoint3D absolute)
{
	struct relative3D rel;

	rel.z = absolute.z;
	rel.y = (absolute.y - lat_origin.WW) / 90; // in meters
	rel.x = long_scale((absolute.x - lon_origin.WW) / 90, cos_lat);
	return rel;
}

#ifdef USE_EXTENDED_NAV
struct relative3D_32 dcm_absolute_to_relative_32(struct waypoint3D absolute)
{
	struct relative3D_32 rel;

	rel.z = absolute.z;
	rel.y = (absolute.y - lat_origin.WW) / 90; // in meters
	rel.x = long_scale((absolute.x - lon_origin.WW) / 90, cos_lat);
	return rel;
}
#endif // USE_EXTENDED_NAV

vect3D_32 dcm_rel2abs(vect3D_32 rel)
{
	vect3D_32 abs;

	abs.z = rel.z;
	abs.y = (rel.y * 90) + lat_origin.WW;
	abs.x = (rel.x * 90) + lon_origin.WW;
//	abs.x = long_scale((rel.x * 90), cos_lat) + lon_origin.WW;

	return abs;
}

#if (HILSIM == 1)

void send_HILSIM_outputs(void)
{
	// Setup outputs for HILSIM
	int16_t i;
	uint8_t CK_A = 0;
	uint8_t CK_B = 0;
	union intbb TempBB;

#if (USE_VARIABLE_HILSIM_CHANNELS != 1)
	for (i = 1; i <= NUM_OUTPUTS; i++)
	{
		TempBB.BB = udb_pwOut[i];
		SIMservoOutputs[2*i] = TempBB._.B1;
		SIMservoOutputs[(2*i)+1] = TempBB._.B0;
	}

	for (i = 2; i < HILSIM_NUM_SERVOS*2+2; i++)
	{
		CK_A += SIMservoOutputs[i];
		CK_B += CK_A;
	}
	SIMservoOutputs[i] = CK_A;
	SIMservoOutputs[i+1] = CK_B;

	// Send HILSIM outputs
	gpsoutbin(HILSIM_NUM_SERVOS*2+4, SIMservoOutputs);
#else
	for (i = 1; i <= NUM_OUTPUTS; i++)
	{
		TempBB.BB = udb_pwOut[i];
		SIMservoOutputs[(2*i)+1] = TempBB._.B1;
		SIMservoOutputs[(2*i)+2] = TempBB._.B0;
	}

	SIMservoOutputs[2] = NUM_OUTPUTS;

	// Calcualte checksum
	for (i = 3; i < (NUM_OUTPUTS*2)+3; i++)
	{
		CK_A += SIMservoOutputs[i];
		CK_B += CK_A;
	}
	SIMservoOutputs[i] = CK_A;
	SIMservoOutputs[i+1] = CK_B;

	// Send HILSIM outputs
	gpsoutbin((HILSIM_NUM_SERVOS*2)+5, SIMservoOutputs);
#endif // USE_VARIABLE_HILSIM_CHANNELS
}

#endif // HILSIM
