//#define PULUTOF_EXTRA
/*
	PULUROBOT RN1-HOST Computer-on-RobotBoard main software

	(c) 2017-2018 Pulu Robotics and other contributors
	Maintainer: Antti Alhonen <antti.alhonen@iki.fi>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License version 2, as 
	published by the Free Software Foundation.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	GNU General Public License version 2 is supplied in file LICENSING.



	Driver for SPI-connected PULUTOF 3D Time-of-Flight add-on

*/

#ifndef PULUTOF_H
#define PULUTOF_H


typedef struct __attribute__((packed))
{
	int32_t ang; // int32_t range --> -180..+180 deg; let it overflow freely. 1 unit = 83.81903171539 ndeg
	int32_t x;   // in mm
	int32_t y;
} pos_t;

typedef struct __attribute__((packed))
{
	int32_t x;
	int32_t y;
	int32_t z;
} xyz_t;


#define PULUTOF_STATUS_OVERFLOW  253
#define PULUTOF_STATUS_MULTIPLE  254
#define PULUTOF_STATUS_AVAILABLE 255
// 0..250: suggested sleep interval in 1ms units.


#define TOF_XS 160
#define TOF_YS 60

/*
	PORTABILITY WARNING:
	Assuming little endian on both sides. Developed for Raspi <-> Cortex M7.
*/
typedef struct __attribute__((packed))
{
	uint32_t header;
	uint8_t status; // Only read this and deassert chip select for polling the status
	uint8_t dummy1;
	uint8_t dummy2;
	uint8_t sensor_idx;

	pos_t robot_pos; // Robot pose during the acquisition

	uint16_t depth[TOF_XS*TOF_YS];
	uint8_t  ampl[TOF_XS*TOF_YS];
//	uint8_t  ambient[EPC_XS*EPC_YS];
#ifdef PULUTOF_EXTRA

	uint16_t uncorrected_depth[TOF_XS*TOF_YS];

	uint8_t dbg_id;
	uint8_t dbg[2*TOF_XS*TOF_YS];
#endif
	uint16_t timestamps[24]; // 0.1ms unit timestamps of various steps for analyzing the timing of low-level processing
	int32_t  dbg_i32[8];

} pulutof_frame_t;

void request_tof_quit();
void* pulutof_poll_thread();
void* pulutof_processing_thread();

pulutof_frame_t* get_pulutof_frame();

void pulutof_decr_dbg();
void pulutof_incr_dbg();
void pulutof_cal_offset(uint8_t idx);


extern volatile int send_raw_tof; // which sensor id to send as raw_depth, <0 = N/A
extern volatile int send_pointcloud; // 0 = off, 1 = relative to robot, 2 = relative to actual world coords

typedef struct
{
	pos_t robot_pos;
	uint16_t raw_depth[160*60]; // for development purposes: populated only when enabled, with only 1 sensor at the time
	uint8_t ampl_images[4][160*60];

	// Point cloud is only populated when enabled:
	int n_points;
	xyz_t cloud[4*TOF_XS*TOF_YS];
} tof3d_scan_t;

tof3d_scan_t* get_tof3d();




#endif