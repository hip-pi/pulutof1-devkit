//#define SPI_PRINT_DBG
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

	For Raspberry Pi 3, make sure that:
	dtparam=spi=on     is in /boot/config.txt uncommented
	/dev/spidev0.0 should exist

	Also:
	/boot/cmdline.txt:  spidev.bufsiz=65536
	This often defaults to 4096, which is ridiculously too small.
	(65535 is hardware maximum for STM32 DMA transfer, so bigger transfers
	couldn't be easily utilized, anyway.)

*/

#define _BSD_SOURCE  // glibc backwards incompatibility workaround to bring usleep back.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <unistd.h>
#include <math.h>

#include "pulutof.h"

#define PULUTOF_SPI_DEVICE "/dev/spidev0.0"

extern volatile int verbose_mode;

static int spi_fd;
static volatile int running = 1;

static const unsigned char spi_mode = SPI_MODE_0;
static const unsigned char spi_bits_per_word = 8;
static const unsigned int spi_speed = 32000000; // Hz

static int init_spi()
{
	spi_fd = open(PULUTOF_SPI_DEVICE, O_RDWR);

	if(spi_fd < 0)
	{
		printf("ERROR: Opening PULUTOF SPI device %s failed: %d (%s).\n", PULUTOF_SPI_DEVICE, errno, strerror(errno));
		return -1;
	}

	/*
		SPI_MODE_0 CPOL = 0, CPHA = 0, Clock idle low, data is clocked in on rising edge, output data (change) on falling edge
		SPI_MODE_1 CPOL = 0, CPHA = 1, Clock idle low, data is clocked in on falling edge, output data (change) on rising edge
		SPI_MODE_2 CPOL = 1, CPHA = 0, Clock idle high, data is clocked in on falling edge, output data (change) on rising edge
		SPI_MODE_3 CPOL = 1, CPHA = 1, Clock idle high, data is clocked in on rising, edge output data (change) on falling edge
	*/

	/*
		Several code examples, (for example, http://www.raspberry-projects.com/pi/programming-in-c/spi/using-the-spi-interface),
		have totally misunderstood what SPI_IOC_WR_* and SPI_IOC_RD_* mean. They are not different settings for SPI RX/TX respectively
		(that wouldn't make any sense: SPI by definition has synchronous RX and TX so clearly the settings are always the same). Instead,
		SPI_IOC_WR_* and SPI_IOC_RD_* write and read the driver settings, respectively, as explained in spidev documentation:
		https://www.kernel.org/doc/Documentation/spi/spidev

		Here, we just set what we need.
	*/

	if(ioctl(spi_fd, SPI_IOC_WR_MODE, &spi_mode) < 0)
	{
		printf("ERROR: Opening PULUTOF SPI devide: ioctl SPI_IOC_WR_MODE failed: %d (%s).\n", errno, strerror(errno));
		return -2;
	}

	if(ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &spi_bits_per_word) < 0)
	{
		printf("ERROR: Opening PULUTOF SPI devide: ioctl SPI_IOC_WR_BITS_PER_WORD failed: %d (%s).\n", errno, strerror(errno));
		return -2;
	}

	if(ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed) < 0)
	{
		printf("ERROR: Opening PULUTOF SPI devide: ioctl SPI_IOC_WR_MAX_SPEED_HZ failed: %d (%s).\n", errno, strerror(errno));
		return -2;
	}

	return 0;
}


static int deinit_spi()
{
	if(close(spi_fd) < 0)
	{
		printf("WARNING: Closing PULUTOF SPI devide failed: %d (%s).\n", errno, strerror(errno));
		return -1;
	}

	return 0;
}

volatile int dbg_id = 0;

void pulutof_decr_dbg()
{
	if(dbg_id) dbg_id--;
	printf("PULUTOF dbg_id=%d\n", dbg_id);
}

void pulutof_incr_dbg()
{
	dbg_id++;
	printf("PULUTOF dbg_id=%d\n", dbg_id);
}

#define PULUTOF_RINGBUF_LEN 16
volatile pulutof_frame_t pulutof_ringbuf[PULUTOF_RINGBUF_LEN];
volatile int pulutof_ringbuf_wr = 0;
volatile int pulutof_ringbuf_rd = 0;

#define TOF3D_RING_BUF_LEN 32

volatile tof3d_scan_t tof3ds[TOF3D_RING_BUF_LEN];
volatile int tof3d_wr;
volatile int tof3d_rd;

tof3d_scan_t* get_tof3d()
{
	if(tof3d_wr == tof3d_rd)
	{
		return 0;
	}
	
	tof3d_scan_t* ret = &tof3ds[tof3d_rd];
	tof3d_rd++; if(tof3d_rd >= TOF3D_RING_BUF_LEN) tof3d_rd = 0;
	return ret;
}


pulutof_frame_t* get_pulutof_frame()
{
	if(pulutof_ringbuf_wr == pulutof_ringbuf_rd)
	{
		return 0;
	}
	
	pulutof_frame_t* ret = &pulutof_ringbuf[pulutof_ringbuf_rd];
	pulutof_ringbuf_rd++; if(pulutof_ringbuf_rd >= PULUTOF_RINGBUF_LEN) pulutof_ringbuf_rd = 0;
	return ret;
}


static float x_angs[TOF_XS*TOF_YS];
static float y_angs[TOF_XS*TOF_YS];

#define GEOCAL_N_X 5
#define GEOCAL_N_Y 6

typedef struct
{
	int sens_x;
	int sens_y;
	float ang_x;
	float ang_y;
} geocal_point_t;

/*
	Pixel coordinates
*/
static const geocal_point_t lens_quadrant_coords[GEOCAL_N_Y+1][GEOCAL_N_X+1] =
{
	{ { 12,   2,   50.0, 25.0}, { 19,   1,   45.0, 25.0}, { 28,   0,   40.0, 25.0}, { 43,  -1,   30.0, 25.0}, { 62,  -2,   15.0, 25.0}, { 80,  -2,   0, 25.0} },
	{ { 11,   5,   50.0, 22.5}, { 19,   4,   45.0, 22.5}, { 27,   3,   40.0, 22.5}, { 42,   2,   30.0, 22.5}, { 62,   1,   15.0, 22.5}, { 80,   1,   0, 22.5} },
	{ { 11,   8,   50.0, 20.0}, { 19,   6,   45.0, 20.0}, { 27,   6,   40.0, 20.0}, { 42,   5,   30.0, 20.0}, { 62,   4,   15.0, 20.0}, { 80,   4,   0, 20.0} },
	{ { 10,  10,   50.0, 17.5}, { 18,   9,   45.0, 17.5}, { 27,   9,   40.0, 17.5}, { 42,   8,   30.0, 17.5}, { 62,   8,   15.0, 17.5}, { 80,   8,   0, 17.5} },
	{ { 10,  13,   50.0, 15.0}, { 18,  12,   45.0, 15.0}, { 27,  12,   40.0, 15.0}, { 42,  11,   30.0, 15.0}, { 62,  11,   15.0, 15.0}, { 80,  11,   0, 15.0} },
	{ { 10,  19,   50.0, 10.0}, { 18,  18,   45.0, 10.0}, { 26,  18,   40.0, 10.0}, { 42,  17,   30.0, 10.0}, { 62,  17,   15.0, 10.0}, { 80,  17,   0, 10.0} },
	{ { 10,  29,   50.0,  0.0}, { 18,  29,   45.0,  0.0}, { 26,  29,   40.0,  0.0}, { 42,  29,   30.0,  0.0}, { 62,  29,   15.0,  0.0}, { 80,  29,   0,  0.0} }
};

static float x_angs[TOF_XS*TOF_YS];
static float y_angs[TOF_XS*TOF_YS];


/*
	Sensor mount position 1:
	 _ _
	| | |
	| |L|
	|O|L|
	| |L|
	|_|_|  (front view)

	Sensor mount position 2:
	 _ _
	| | |
	|L| |
	|L|O|
	|L| |
	|_|_|  (front view)

	Sensor mount position 3:

	-------------
	|  L  L  L  |
	-------------
	|     O     |
	-------------

	Sensor mount position 4:

	-------------
	|     O     |
	-------------
	|  L  L  L  |
	-------------
*/

typedef struct
{
	int mount_mode;             // mount position 1,2,3 or 4
	float x_rel_robot;          // zero = robot origin. Positive = robot front (forward)
	float y_rel_robot;          // zero = robot origin. Positive = to the right of the robot
	float ang_rel_robot;        // zero = robot forward direction. positive = ccw
	float vert_ang_rel_ground;  // zero = looks directly forward. positive = looks up. negative = looks down
	float z_rel_ground;         // sensor height from the ground	
} sensor_mount_t;

#define NUM_PULUTOFS 4

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

#define RADTODEG(x) ((x)*(360.0/(2.0*M_PI)))
#define DEGTORAD(x) ((x)*((2.0*M_PI)/360.0))

static const sensor_mount_t sensor_mounts[NUM_PULUTOFS] =
{          //      mountmode    x     y       hor ang           ver ang      height    
 /*0: Left rear      */ { 2,  -276,  -233, DEGTORAD(      90), DEGTORAD(  0), 227 },
 /*1: Right rear     */ { 1,  -276,   233, DEGTORAD(     270), DEGTORAD(  0), 227 },
 /*2: Right front    */ { 2,   154,   164, DEGTORAD(       0), DEGTORAD(  0), 228 },
 /*3: Left front     */ { 1,   154,  -164, DEGTORAD(       0), DEGTORAD(  0), 228 }
};

static void distances_to_objmap(pulutof_frame_t *in)
{
	int sidx = in->sensor_idx;
	if(sidx > NUM_PULUTOFS-1)
	{
		printf("WARNING: distances_to_objmap: illegal sensor idx coming from hw.\n");
		return;
	}

	/*
		for converting to absolute world coordinates, if that's needed in the future:
	
	float robot_ang = ANG32TORAD(in->robot_pos.ang);
	float robot_x = in->robot_pos.x;
	float robot_y = in->robot_pos.y;

	float sensor_ang = robot_ang + sensor_mounts[sidx].ang_rel_robot;
	float sensor_x = robot_x + cos(robot_ang)*sensor_mounts[sidx].x_rel_robot;
	float sensor_y = robot_y + sin(robot_ang)*sensor_mounts[sidx].y_rel_robot;
	*/

	float sensor_ang = sensor_mounts[sidx].ang_rel_robot;
	float sensor_x = sensor_mounts[sidx].x_rel_robot;
	float sensor_y = sensor_mounts[sidx].y_rel_robot;
	float sensor_yang = sensor_mounts[sidx].vert_ang_rel_ground;
	float sensor_z = sensor_mounts[sidx].z_rel_ground;
	
	int do_send_pointcloud = send_pointcloud;


	for(int pyy = 1; pyy < TOF_YS-1; pyy++)
	{
		for(int pxx = 1; pxx < TOF_XS-1; pxx++)
		{
			int n_valids = 0;
			int avg = 0;
			for(int dyy=-1; dyy<=1; dyy++)
			{
				for(int dxx=-1; dxx<=1; dxx++)
				{
					int dist = in->depth[(pyy+dyy)*TOF_XS+(pxx+dxx)];
					if(dist != 0)
					{
						n_valids++;
						avg += dist;
					}
				}
			}

			if(n_valids > 4)
			{
				avg /= n_valids;
				int n_conforming = 0;
				int avg_conforming = 0;
				int cumul_dxx = 0, cumul_dyy = 0;
				for(int dyy=-1; dyy<=1; dyy++)
				{
					for(int dxx=-1; dxx<=1; dxx++)
					{
						int dist = in->depth[(pyy+dyy)*TOF_XS+(pxx+dxx)];
						if(dist != 0 && dist > avg-350 && dist < avg+350)
						{
							n_conforming++;
							avg_conforming += dist;
							cumul_dxx += dxx;
							cumul_dyy += dyy;
						}
					}
				}

				if(n_conforming > 2)
				{
					int py, px;
					if(cumul_dxx < -2) px = pxx-1; else if(cumul_dxx > 2) px = pxx+1; else px = pxx;
					if(cumul_dyy < -2) py = pyy-1; else if(cumul_dyy > 2) py = pyy+1; else py = pyy;

					float hor_ang, ver_ang;

					switch(sensor_mounts[sidx].mount_mode)
					{
						case 1: 
						hor_ang = -1*y_angs[py*TOF_XS+px];
						ver_ang = x_angs[py*TOF_XS+px];
						break;

						case 2: 
						hor_ang = y_angs[py*TOF_XS+px];
						ver_ang = -1*x_angs[py*TOF_XS+px];
						break;

						case 3: // direction in which the original geometrical calibration was calculated in
						hor_ang = -1*x_angs[py*TOF_XS+px];
						ver_ang = y_angs[py*TOF_XS+px];
						break;

						case 4: // Same as 3, but upside down
						hor_ang = x_angs[py*TOF_XS+px];
						ver_ang = -1*y_angs[py*TOF_XS+px];
						break;

						default: printf("ERROR: illegal mount_mode in sensor mount table.\n"); return;
					}

					// From spherical to cartesian coordinates

					float d = (float)avg_conforming/(float)n_conforming;

					float x = d * cos(ver_ang + sensor_yang) * cos(hor_ang + sensor_ang) + sensor_x;
					float y = -1* (d * cos(ver_ang + sensor_yang) * sin(hor_ang + sensor_ang)) + sensor_y;
					float z = d * sin(ver_ang + sensor_yang) + sensor_z;


					//printf("DIST = %.0f  x=%.0f  y=%.0f  z=%.0f  xspot=%d  yspot=%d  ver_ang=%.2f  sensor_yang=%.2f  hor_ang=%.2f  sensor_ang=%.2f\n", d, x, y, z, xspot, yspot, ver_ang, sensor_yang, hor_ang, sensor_ang); 

					if(do_send_pointcloud == 1) // relative to robot
					{
						if(tof3ds[tof3d_wr].n_points < 4*TOF_XS*TOF_YS)
						{
							tof3ds[tof3d_wr].cloud[tof3ds[tof3d_wr].n_points].x = x;
							tof3ds[tof3d_wr].cloud[tof3ds[tof3d_wr].n_points].y = y;
							tof3ds[tof3d_wr].cloud[tof3ds[tof3d_wr].n_points].z = z;
							tof3ds[tof3d_wr].n_points++;
						}
					}
					else if(do_send_pointcloud == 2) // in world coordinates
					{
						if(tof3ds[tof3d_wr].n_points < 4*TOF_XS*TOF_YS)
						{
							float robot_ang = 0;
							float x_world = d * cos(ver_ang + sensor_yang) * cos(hor_ang + sensor_ang + robot_ang) + sensor_x + in->robot_pos.x;
							float y_world = -1* (d * cos(ver_ang + sensor_yang) * sin(hor_ang + sensor_ang + robot_ang)) + sensor_y + in->robot_pos.y;

							tof3ds[tof3d_wr].cloud[tof3ds[tof3d_wr].n_points].x = x_world;
							tof3ds[tof3d_wr].cloud[tof3ds[tof3d_wr].n_points].y = y_world;
							tof3ds[tof3d_wr].cloud[tof3ds[tof3d_wr].n_points].z = z;
							tof3ds[tof3d_wr].n_points++;
						}
					}

				}
				
			}
			
		}
	}
}

static int calibrating = 0;
static int calib_sensor_idx = 0;

static void process_pulutof_frame(pulutof_frame_t *in);

void* pulutof_processing_thread()
{
	while(running)
	{
		pulutof_frame_t* p_tof;
		if( (p_tof = get_pulutof_frame()) )
		{
			process_pulutof_frame(p_tof);
		}
		else
		{
			usleep(5000);
		}

	}

	return NULL;

}

static void process_pulutof_frame(pulutof_frame_t *in)
{
	static int running_ok = 0;
	static int prev_sidx = -1;

	int sidx = in->sensor_idx;

	if(sidx > NUM_PULUTOFS-1)
	{
		printf("WARNING:process_pulutof_frame: illegal sensor idx coming from hw.\n");
		return;
	}

	int expected_sidx = prev_sidx+1; if(expected_sidx >= NUM_PULUTOFS) expected_sidx = 0;

	if(running_ok && expected_sidx != sidx)
	{
		printf("WARNING:process_pulutof_frame: unexpected sensor idx %d, previous was %d, was expecting %d. Ignoring until 0\n", sidx, prev_sidx, expected_sidx);
		running_ok = 0;
	}

	if(sidx == 0)
	{
		running_ok = 1;
		tof3ds[tof3d_wr].n_points = 0;
	}

	if(running_ok)
	{
		if(calibrating)
		{
			if(calib_sensor_idx == sidx)
			{
				distances_to_objmap(in);
			}
		}
		else
		{
			distances_to_objmap(in);

			printf("sidx = %d\n", sidx);


			if(sidx == 2)
			{
				tof3ds[tof3d_wr].robot_pos = in->robot_pos;
			}

			if(sidx == send_raw_tof)
			{
				memcpy(tof3ds[tof3d_wr].raw_depth, in->depth, sizeof tof3ds[tof3d_wr].raw_depth);
			}

			memcpy(tof3ds[tof3d_wr].ampl_images[sidx], in->ampl, sizeof in->ampl);

			if(sidx == NUM_PULUTOFS-1)
			{
				// All sensors done.
				tof3d_wr++; if(tof3d_wr >= TOF3D_RING_BUF_LEN) tof3d_wr = 0;
			}
		}
	}

	prev_sidx = sidx;
}




static void print_table()
{
	for(int yy = 0; yy < TOF_YS; yy++)
	{
		for(int xx=150; xx < 160; xx++)
		{
			printf("(%5.1f, %5.1f) ", x_angs[yy*TOF_XS+xx], y_angs[yy*TOF_XS+xx]);
		}
		printf("\n");
	}

	printf("\n");
	printf("\n");
}

static void outp_ang(int px, int py, float ax, float ay)
{
//	printf("(%3d, %3d)=(%5.1f, %5.1f)\n", px, py, ax, ay);

	if(py < 0 || py > TOF_YS-1 || px < 0 || px > TOF_XS-1)
		return;

	x_angs[py*TOF_XS+px] = DEGTORAD(ax);
	y_angs[py*TOF_XS+px] = DEGTORAD(ay);
}

static void gen_ang_tables()
{
	for(int i=0; i<TOF_XS*TOF_YS; i++)
	{
		x_angs[i] = 999.0; // uninit marker
		y_angs[i] = 999.0;
	}

	for(int calyy=0; calyy<GEOCAL_N_Y+1; calyy++)
	{
		// Left half, extrapolate the first X record
		{
			float dpx = lens_quadrant_coords[calyy][1].sens_x - lens_quadrant_coords[calyy][0].sens_x;
			float dpy = lens_quadrant_coords[calyy][1].sens_y - lens_quadrant_coords[calyy][0].sens_y;
			float dax = lens_quadrant_coords[calyy][1].ang_x - lens_quadrant_coords[calyy][0].ang_x;
			float ax_per_px = dax/dpx;
			float py_per_px = dpy/dpx;
			float cur_ax = -1*lens_quadrant_coords[calyy][0].ang_x + ax_per_px*lens_quadrant_coords[calyy][0].sens_x;
			float cur_ay = -1*lens_quadrant_coords[calyy][0].ang_y;
			float cur_py = lens_quadrant_coords[calyy][0].sens_y - py_per_px*lens_quadrant_coords[calyy][0].sens_x;
			for(int pxx=0; pxx<lens_quadrant_coords[calyy][0].sens_x; pxx++)
			{
				outp_ang(pxx, (int)(cur_py+0.5), cur_ax, cur_ay);
				cur_py += py_per_px;
				cur_ax -= ax_per_px;
			}
		}
		for(int calxx=0; calxx<GEOCAL_N_X; calxx++)
		{
			// Interpolate between the two
			float dpx = lens_quadrant_coords[calyy][calxx+1].sens_x - lens_quadrant_coords[calyy][calxx+0].sens_x;
			float dpy = lens_quadrant_coords[calyy][calxx+1].sens_y - lens_quadrant_coords[calyy][calxx+0].sens_y;
			float dax = lens_quadrant_coords[calyy][calxx+1].ang_x - lens_quadrant_coords[calyy][calxx+0].ang_x;
			float ax_per_px = dax/dpx;
			float py_per_px = dpy/dpx;
			float cur_ax = -1*lens_quadrant_coords[calyy][calxx].ang_x;
			float cur_ay = -1*lens_quadrant_coords[calyy][calxx].ang_y;
			float cur_py = lens_quadrant_coords[calyy][calxx].sens_y;

			for(int pxx=lens_quadrant_coords[calyy][calxx].sens_x; pxx<lens_quadrant_coords[calyy][calxx+1].sens_x; pxx++)
			{
				outp_ang(pxx, (int)(cur_py+0.5), cur_ax, cur_ay);
				cur_py += py_per_px;
				cur_ax -= ax_per_px;
			}
			
		}

		outp_ang(lens_quadrant_coords[calyy][GEOCAL_N_X].sens_x, lens_quadrant_coords[calyy][GEOCAL_N_X].sens_y, lens_quadrant_coords[calyy][GEOCAL_N_X].ang_x, -1*lens_quadrant_coords[calyy][GEOCAL_N_X].ang_y);

	}

	// Fill the missing cells in Y direction
	// Extrapolates at the start if necessary; but expects the last cell (at TOF_XS/2-1) to be populated.
	for(int pxx=0; pxx<TOF_XS/2+1; pxx++)
	{
		float prev_ax, prev_ay;
		int prev_at = 999;
		for(int pyy=0; pyy < TOF_YS/2; pyy++)
		{
			if(x_angs[pyy*TOF_XS+pxx] != 999.0)
			{
				if(prev_at == 999 && pyy > 0) // need to extrapolate - there were empty spots before this
				{
					// Find next:
					int next_py = pyy+1;
					while(x_angs[next_py*TOF_XS+pxx] == 999.0) next_py++;

					float first_ax = x_angs[pyy*TOF_XS+pxx];
					float first_ay = y_angs[pyy*TOF_XS+pxx];
					float second_ax = x_angs[next_py*TOF_XS+pxx];
					float second_ay = y_angs[next_py*TOF_XS+pxx];
					float dax_per_step = (second_ax - first_ax) / (next_py - pyy);
					float day_per_step = (second_ay - first_ay) / (next_py - pyy);
					float cur_ax = first_ax - dax_per_step*pyy; 
					float cur_ay = first_ay - day_per_step*pyy;

					for(int iy=0; iy<pyy; iy++)
					{
						x_angs[iy*TOF_XS+pxx] = cur_ax;
						y_angs[iy*TOF_XS+pxx] = cur_ay;
						cur_ax += dax_per_step;
						cur_ay += day_per_step;
					}
				}

				if(prev_at != 999) // interpolate
				{
					float now_ax = x_angs[pyy*TOF_XS+pxx];
					float now_ay = y_angs[pyy*TOF_XS+pxx];
					float dax_per_step = (prev_ax - now_ax) / (prev_at - pyy);
					float day_per_step = (prev_ay - now_ay) / (prev_at - pyy);
					float cur_ax = prev_ax;
					float cur_ay = prev_ay;
					for(int iy=prev_at+1; iy<pyy; iy++)
					{
						cur_ax += dax_per_step;
						cur_ay += day_per_step;
						x_angs[iy*TOF_XS+pxx] = cur_ax;
						y_angs[iy*TOF_XS+pxx] = cur_ay;
					}
				}

				prev_ax = x_angs[pyy*TOF_XS+pxx];
				prev_ay = y_angs[pyy*TOF_XS+pxx];
				prev_at = pyy;
			}
		}
	}

	// Fill all four quadrants by mirroring
	// Fill bottom left
	for(int pxx=0; pxx<TOF_XS/2+1; pxx++)
	{
		for(int pyy=0; pyy < TOF_YS/2; pyy++)
		{
			int out_pyy = TOF_YS-pyy-2;
			x_angs[out_pyy*TOF_XS+pxx] = x_angs[pyy*TOF_XS+pxx];
			y_angs[out_pyy*TOF_XS+pxx] = -1*y_angs[pyy*TOF_XS+pxx];
		}
		// Extrapolate the last line:
		
		x_angs[(TOF_YS-1)*TOF_XS+pxx] = x_angs[(TOF_YS-2)*TOF_XS+pxx] - (x_angs[(TOF_YS-3)*TOF_XS+pxx] - x_angs[(TOF_YS-2)*TOF_XS+pxx]);
		y_angs[(TOF_YS-1)*TOF_XS+pxx] = y_angs[(TOF_YS-2)*TOF_XS+pxx] - (y_angs[(TOF_YS-3)*TOF_XS+pxx] - y_angs[(TOF_YS-2)*TOF_XS+pxx]);
	}

	// Fill the right half

	for(int pyy=0; pyy<TOF_YS; pyy++)
	{
		for(int pxx=0; pxx < TOF_XS/2; pxx++)
		{
			int out_pxx = TOF_XS-pxx-0;
			x_angs[pyy*TOF_XS+out_pxx] = -1*x_angs[pyy*TOF_XS+pxx];
			y_angs[pyy*TOF_XS+out_pxx] = y_angs[pyy*TOF_XS+pxx];	
		}
	}

	//print_table();

}

/*
	As you should know, SPI is a bidirectional, synchronous "show yours, I'll show mine" protocol. As a master, we'll only send something, and get something back.

	Unfortunately, Raspberry Pi can only be a master, in SPI terms.

	However, the PULUTOF in kind of the "real" master, because it decides when to image the frames, and will do it regardless of what we are
	doing on the Raspberry.

	poll_availability commands a small (five-byte) transfer, which will only return the first status byte. If we are happy with the status (i.e., there is
	data available), we can go on with the full transfer. This status byte further gives insight how long to wait before next polling is needed.

	When poll_availability informs there is a frame available, read_frame can be called - it assumes the data is indeed available and performs a full
	frame read. If the data is not available - the status byte clearly tells this, but if you still read it, you'll hit an old frame, or
	possibly a corrupted combination of two frames.
*/


static uint8_t txbuf[65536];

static int poll_availability()
{
	txbuf[4] = dbg_id&0xff;	
	struct spi_ioc_transfer xfer;
	struct response { uint32_t header; uint8_t status;} response;

	memset(&xfer, 0, sizeof(xfer)); // unused fields need to be initialized zero.
	//xfer.tx_buf left at 0 - documented spidev feature to send out zeroes - we don't have anything to send, just want to get what the sensor wants to send us!
	xfer.tx_buf = txbuf;
	xfer.rx_buf = &response;
	xfer.len = sizeof response;
	xfer.cs_change = 0; // deassert chip select after the transfer

	if(ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
	{
		printf("ERROR: spi ioctl transfer operation failed: %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	if(response.header != 0x11223344 || response.status == 0)
	{
		printf("ERROR: Illegal response in poll_availability: header=0x%08x  status=%d\n", response.header, response.status);
		return -1;
	}
	//printf("status=%d\n", response.status);
	return response.status;
}

static int read_frame()
{
	txbuf[4] = dbg_id&0xff;	
	struct spi_ioc_transfer xfer;
	memset(&xfer, 0, sizeof(xfer)); // unused fields need to be initialized zero.

	//xfer.tx_buf left at 0 - documented spidev feature to send out zeroes - we don't have anything to send, just want to get what the sensor wants to send us!
	xfer.tx_buf = txbuf;
	xfer.rx_buf = &pulutof_ringbuf[pulutof_ringbuf_wr];
	xfer.len = sizeof(pulutof_frame_t);
	xfer.cs_change = 0; // deassert chip select after the transfer

	if(ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
	{
		printf("ERROR: spi ioctl transfer operation failed: %d (%s)\n", errno, strerror(errno));
		return -1;
	}

	if(verbose_mode)
	{
		printf("Frame (sensor_idx= %d) read ok, pose=(%d,%d,%d). Timing data:\n", pulutof_ringbuf[pulutof_ringbuf_wr].sensor_idx, pulutof_ringbuf[pulutof_ringbuf_wr].robot_pos.x, pulutof_ringbuf[pulutof_ringbuf_wr].robot_pos.y, pulutof_ringbuf[pulutof_ringbuf_wr].robot_pos.ang);
		for(int i=0; i<24; i++)
		{
			printf("%d:%.1f ", i, (float)pulutof_ringbuf[pulutof_ringbuf_wr].timestamps[i]/10.0);
		}
		printf("\n");
		printf("Time deltas to:\n");
		for(int i=1; i<24; i++)
		{
			printf(">%d:%.1f ", i, (float)(pulutof_ringbuf[pulutof_ringbuf_wr].timestamps[i]-pulutof_ringbuf[pulutof_ringbuf_wr].timestamps[i-1])/10.0);
		}
		printf("\n");
		printf("dbg_i32:\n");
		for(int i=0; i<8; i++)
		{
			printf("[%d] %11d  ", i, pulutof_ringbuf[pulutof_ringbuf_wr].dbg_i32[i]);
		}
		printf("\n");
		printf("\n");
	}

	int ret = pulutof_ringbuf[pulutof_ringbuf_wr].status;

	pulutof_ringbuf_wr++; if(pulutof_ringbuf_wr >= PULUTOF_RINGBUF_LEN) pulutof_ringbuf_wr = 0;

	return ret;
}

void request_tof_quit()
{
	running = 0;
}

void pulutof_cal_offset(uint8_t idx)
{
	printf("Requesting offset calib\n");
	struct spi_ioc_transfer xfer;
	struct cmd { uint32_t header; uint8_t sensor_idx;} cmd;

	cmd.header = 0xca0ff5e7;
	cmd.sensor_idx = idx;

	memset(&xfer, 0, sizeof(xfer)); // unused fields need to be initialized zero.
	xfer.tx_buf = &cmd;
	xfer.rx_buf = NULL;
	xfer.len = sizeof cmd;
	xfer.cs_change = 0; // deassert chip select after the transfer

	if(ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0)
	{
		printf("ERROR: spi ioctl transfer operation failed: %d (%s)\n", errno, strerror(errno));
		return;
	}

}


void* pulutof_poll_thread()
{
	gen_ang_tables();
	init_spi();
	while(running)
	{
		int next = pulutof_ringbuf_wr+1; if(next >= PULUTOF_RINGBUF_LEN) next = 0;
		if(next == pulutof_ringbuf_rd)
		{
			printf("WARNING: PULUTOF ringbuf overflow prevented, ignoring images...\n");
			usleep(250000);
			continue;
		}


		int avail = poll_availability();

		if(avail < 0)
		{
#ifdef SPI_PRINT_DBG
		//	printf("Sleeping 2 s\n");
#endif
			sleep(2);
			continue;
		}

		if(avail < 250)
		{
#ifdef SPI_PRINT_DBG
		//	printf("Sleeping %d ms\n", avail);
#endif
			usleep(1000*avail);
			continue;
		}

		read_frame();


		usleep(1000);
	}
	deinit_spi();

	return NULL;
}

