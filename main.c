/*
	PULUROBOT PULUTOF-DEVKIT development software for saving pointclouds

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



*/

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "tcp_comm.h"
#include "tcp_parser.h"

#include "pulutof.h"

volatile int verbose_mode = 0;
volatile int send_raw_tof = -1;
volatile int send_pointcloud = 0; // 0 = off, 1 = relative to robot, 2 = relative to actual world coords

double subsec_timestamp()
{
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);

	return (double)spec.tv_sec + (double)spec.tv_nsec/1.0e9;
}


void save_pointcloud(int n_points, xyz_t* cloud)
{
	static int pc_cnt = 0;
	char fname[256];
	snprintf(fname, 255, "cloud%05d.xyz", pc_cnt);
	printf("Saving pointcloud with %d samples to file %s.\n", n_points, fname);
	FILE* pc_csv = fopen(fname, "w");
	if(!pc_csv)
	{
		printf("Error opening file for write.\n");
	}
	else
	{
		for(int i=0; i < n_points; i++)
		{
			fprintf(pc_csv, "%d %d %d\n",cloud[i].x, -1*cloud[i].y, cloud[i].z);
		}
		fclose(pc_csv);
	}

	pc_cnt++;
	if(pc_cnt > 99999) pc_cnt = 0;
}



void request_tof_quit(void);

volatile int retval = 0;

void* main_thread()
{

	if(init_tcp_comm())
	{
		fprintf(stderr, "TCP communication initialization failed.\n");
		return NULL;
	}

	while(1)
	{
		// Calculate fd_set size (biggest fd+1)
		int fds_size = 0;
		if(tcp_listener_sock > fds_size) fds_size = tcp_listener_sock;
		if(tcp_client_sock > fds_size) fds_size = tcp_client_sock;
		if(STDIN_FILENO > fds_size) fds_size = STDIN_FILENO;
		fds_size+=1;

		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(tcp_listener_sock, &fds);
		if(tcp_client_sock >= 0)
			FD_SET(tcp_client_sock, &fds);

		struct timeval select_time = {0, 200};

		if(select(fds_size, &fds, NULL, NULL, &select_time) < 0)
		{
			fprintf(stderr, "select() error %d", errno);
			return NULL;
		}

		if(FD_ISSET(STDIN_FILENO, &fds))
		{
			int cmd = fgetc(stdin);
			if(cmd == 'q')
			{
				retval = 0;
				break;
			}
			if(cmd == 'z')
			{
				if(send_raw_tof >= 0) send_raw_tof--;
				printf("Sending raw tof from sensor %d\n", send_raw_tof);
			}
			if(cmd == 'x')
			{
				if(send_raw_tof < 3) send_raw_tof++;
				printf("Sending raw tof from sensor %d\n", send_raw_tof);
			}
			if(cmd >= '0' && cmd <= '3')
			{
				pulutof_cal_offset(cmd);
			}
			if(cmd == 'v')
			{
				verbose_mode = verbose_mode?0:1;
			}
			if(cmd == 'p')
			{
				if(send_pointcloud == 0)
				{
					printf("INFO: Will send pointclouds relative to robot origin\n");
					send_pointcloud = 1;
				}
				else if(send_pointcloud == 1)
				{
					printf("INFO: Will send pointclouds relative to world origin\n");
					send_pointcloud = 2;
				}
				else
				{
					printf("INFO: Will stop sending pointclouds\n");
					send_pointcloud = 0;
				}
			}
		}




		if(tcp_client_sock >= 0 && FD_ISSET(tcp_client_sock, &fds))
		{
			int ret = handle_tcp_client();
			if(ret == TCP_CR_MAINTENANCE_MID)
			{
				if(msg_cr_maintenance.magic == 0x12345678)
				{
					retval = msg_cr_maintenance.retval;
					break;
				}
				else
				{
					printf("WARN: Illegal maintenance message magic number 0x%08x.\n", msg_cr_maintenance.magic);
				}
			}		
		}

		if(FD_ISSET(tcp_listener_sock, &fds))
		{
			handle_tcp_listener();
		}


		tof3d_scan_t *p_tof;
		if( (p_tof = get_tof3d()) )
		{
			printf("yesbox\n");
			save_pointcloud(p_tof->n_points, p_tof->cloud);

			if(tcp_client_sock >= 0)
			{
				if(send_raw_tof >= 0 && send_raw_tof < 4)
				{
					tcp_send_picture(100, 2, 160, 60, (uint8_t*)p_tof->raw_depth);
					tcp_send_picture(101, 2, 160, 60, (uint8_t*)p_tof->ampl_images[send_raw_tof]);
				}
			}

		}

	}

	request_tof_quit();

	return NULL;
}


void* start_tof(void*);

int main(int argc, char** argv)
{
	pthread_t thread_main, thread_tof, thread_tof2;

	int ret;

	if( (ret = pthread_create(&thread_main, NULL, main_thread, NULL)) )
	{
		printf("ERROR: main thread creation, ret = %d\n", ret);
		return -1;
	}

	if( (ret = pthread_create(&thread_tof, NULL, pulutof_poll_thread, NULL)) )
	{
		printf("ERROR: tof3d access thread creation, ret = %d\n", ret);
		return -1;
	}

	#ifndef PULUTOF1_GIVE_RAWS
		if( (ret = pthread_create(&thread_tof2, NULL, pulutof_processing_thread, NULL)) )
		{
			printf("ERROR: tof3d processing thread creation, ret = %d\n", ret);
			return -1;
		}
	#endif

	pthread_join(thread_main, NULL);

	pthread_join(thread_tof, NULL);
	#ifndef PULUTOF1_GIVE_RAWS
	pthread_join(thread_tof2, NULL);
	#endif

	return retval;
}
