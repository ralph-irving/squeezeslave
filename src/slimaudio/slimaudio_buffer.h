/*
 *   SlimProtoLib Copyright (c) 2004,2006 Richard Titmuss
 *
 *   This file is part of SlimProtoLib.
 *
 *   SlimProtoLib is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   SlimProtoLib is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with SlimProtoLib; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef _SLIMAUDIO_BUFFER_H_
#define _SLIMAUDIO_BUFFER_H_

#include <stdbool.h>
#include <pthread.h>

#define BUFFER_BLOCKING 0
#define BUFFER_NONBLOCKING 1

struct buffer_stream {
	int available;							/* bytes available in stream */
	int read_count;							/* number of bytes read from stream */
	bool eof;							/* true when eof */
	void *user_data;

	struct buffer_stream *next;
};

typedef struct {
	char *buffer_start;
	char *buffer_end;
	int buffer_size;
	int total_available;
	char *read_ptr;
	char *write_ptr;
	int read_opt;

	struct buffer_stream *read_stream;
	struct buffer_stream *write_stream;

	pthread_mutex_t buffer_mutex;
	pthread_cond_t write_cond;
	pthread_cond_t read_cond;
	bool writer_blocked;
	bool reader_blocked;
	bool buffer_closed;
} slimaudio_buffer_t;

typedef enum { SLIMAUDIO_BUFFER_STREAM_START=0, SLIMAUDIO_BUFFER_STREAM_CONTINUE, SLIMAUDIO_BUFFER_STREAM_END, SLIMAUDIO_BUFFER_STREAM_UNDERRUN } slimaudio_buffer_status;

slimaudio_buffer_t *slimaudio_buffer_init(int size);

void slimaudio_buffer_free(slimaudio_buffer_t *buf);

void slimaudio_buffer_open(slimaudio_buffer_t *buf, void *user_data);

void slimaudio_buffer_close(slimaudio_buffer_t *buf);

void slimaudio_buffer_flush(slimaudio_buffer_t *buf);

void slimaudio_buffer_set_readopt(slimaudio_buffer_t *buf, int opt);

void slimaudio_buffer_write(slimaudio_buffer_t *buf, char *data, int len);

slimaudio_buffer_status slimaudio_buffer_read(slimaudio_buffer_t *buf, char *data, int *data_len);

int slimaudio_buffer_available(slimaudio_buffer_t *buf);

#endif //_SLIMAUDIO_BUFFER_H_
