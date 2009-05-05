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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include <assert.h>

#include "slimaudio/slimaudio_buffer.h"

#ifdef SLIMPROTO_DEBUG
  bool slimaudio_buffer_debug;
  bool slimaudio_buffer_debug_v;
  #define DEBUGF(...) if (slimaudio_buffer_debug) fprintf(stderr, __VA_ARGS__)
  #define VDEBUGF(...) if (slimaudio_buffer_debug_v) fprintf(stderr, __VA_ARGS__)
#else
  #define DEBUGF(...)
  #define VDEBUGF(...)
#endif

slimaudio_buffer_t *slimaudio_buffer_init(int size) {
	slimaudio_buffer_t *buf = (slimaudio_buffer_t *) malloc(sizeof(slimaudio_buffer_t));
	memset(buf, 0, sizeof(slimaudio_buffer_t));

	buf->buffer_start = (char *) malloc(size);
	buf->buffer_end = buf->buffer_start + size;
	buf->buffer_size = size;
	buf->read_ptr = buf->write_ptr = buf->buffer_start;
	
	pthread_mutex_init(&(buf->buffer_mutex), NULL);
	pthread_cond_init(&(buf->read_cond), NULL);
	pthread_cond_init(&(buf->write_cond), NULL);
	
	return buf;
}


void slimaudio_buffer_free(slimaudio_buffer_t *buf) {
	assert(buf);
	
	pthread_mutex_destroy(&(buf->buffer_mutex));
	pthread_cond_destroy(&(buf->read_cond));
	pthread_cond_destroy(&(buf->write_cond));

	free(buf->buffer_start);
	free(buf);	
}

void slimaudio_buffer_open(slimaudio_buffer_t *buf, void *user_data) {
	assert(buf);	
	
	struct buffer_stream *stream = (struct buffer_stream *) malloc(sizeof(struct buffer_stream));
	memset(stream, 0, sizeof(struct buffer_stream));
	
	stream->user_data = user_data;
	
	if (buf->write_stream != NULL)
		buf->write_stream->next = stream;	
	buf->write_stream = stream;

	if (buf->read_stream == NULL)
		buf->read_stream = stream;	
}

void slimaudio_buffer_close(slimaudio_buffer_t *buf) {
	pthread_mutex_lock(&buf->buffer_mutex);

	assert(buf);
	
	if (buf->write_stream != NULL)
		buf->write_stream->eof = true;
	
	if (buf->writer_blocked) {
		buf->writer_blocked = false;
		pthread_cond_signal(&buf->read_cond);
	}
		
	pthread_mutex_unlock(&buf->buffer_mutex);		
}

void slimaudio_buffer_flush(slimaudio_buffer_t *buf) {
	pthread_mutex_lock(&buf->buffer_mutex);

	assert(buf);
	
	DEBUGF("slimaudio_buffer_flush buf=%p\n", buf);
	
	struct buffer_stream *stream, *next_stream;
	
	stream = buf->read_stream;
	while (stream != NULL) {
		next_stream = stream->next;
		if (stream->user_data != NULL)
			free(stream->user_data);
		free(stream);

		stream = next_stream;	
	}

	buf->read_ptr = buf->write_ptr = buf->buffer_start;
	buf->total_available = 0;
	buf->write_stream = NULL;
	buf->read_stream = NULL;
	
	if (buf->writer_blocked) {
		buf->writer_blocked = false;
		pthread_cond_signal(&buf->read_cond);
	}
	if (buf->reader_blocked) {
		buf->reader_blocked = false;
		pthread_cond_signal(&buf->write_cond);
	}
	
	pthread_mutex_unlock(&buf->buffer_mutex);
}

void slimaudio_buffer_set_readopt(slimaudio_buffer_t *buf, int opt) {
	assert(buf);
	
	buf->read_opt = opt;
}

void slimaudio_buffer_write(slimaudio_buffer_t *buf, char *data, int len) {
	int free;

	pthread_mutex_lock(&buf->buffer_mutex);
	
	assert(buf);
	assert(data);

	if (buf->write_stream == NULL || buf->write_stream->eof) {
		// stream closed
		pthread_mutex_unlock(&buf->buffer_mutex);
		return;
	}
	
	free = buf->buffer_size - buf->total_available;
	VDEBUGF("buffer_write %p write_ptr=%p read_ptr=%p free=%i\n", buf, buf->write_ptr, buf->read_ptr, free);
	
	/* Buffer full; block until we have enough space */
	while (free < len) {
		VDEBUGF("buffer_write waiting (need %i bytes) ..\n", len);

		buf->writer_blocked = true;
		pthread_cond_wait(&buf->read_cond, &buf->buffer_mutex);

		if (buf->write_stream == NULL || buf->write_stream->eof) {
			pthread_mutex_unlock(&buf->buffer_mutex);
			DEBUGF("buffer_write closed/flushed %p\n", buf);
			return;
		}
		
		free = buf->buffer_size - buf->total_available;
	}

	int trailing_space = buf->buffer_end - buf->write_ptr;
	if ( len < trailing_space) {
		/* sufficient trailing space */
		memcpy(buf->write_ptr, data, len);
		buf->write_ptr += len;
		if (buf->write_ptr >= buf->buffer_end)
			buf->write_ptr = buf->buffer_start;
	}
	else {
		/* insufficient trailing space */
		memcpy(buf->write_ptr, data, trailing_space);
		data += trailing_space;

		/* copy remainder to start */
		int remainder_len = len - trailing_space;
		memcpy(buf->buffer_start, data, remainder_len);
		buf->write_ptr = buf->buffer_start + remainder_len;
	}
	
	buf->total_available += len;	
	buf->write_stream->available += len;
	
	if (buf->reader_blocked) {
		buf->reader_blocked = false;	
		pthread_cond_signal(&buf->write_cond);	
	}
	pthread_mutex_unlock(&buf->buffer_mutex);
}

slimaudio_buffer_status slimaudio_buffer_read(slimaudio_buffer_t *buf, char *data, int *data_len) {
	pthread_mutex_lock(&buf->buffer_mutex);
	
	assert(buf);
	assert(data);

	int len = *data_len;
	
	while (buf->total_available == 0) {
		if (buf->read_stream == NULL) {
			pthread_mutex_unlock(&buf->buffer_mutex);
			DEBUGF("total_available:0 SLIMAUDIO_BUFFER_STREAM_END\n");

			*data_len = 0;
			return SLIMAUDIO_BUFFER_STREAM_END;			
		}

		if ( (buf->read_opt & BUFFER_NONBLOCKING) > 0) {
			pthread_mutex_unlock(&buf->buffer_mutex);

			DEBUGF("total_available:0 BUFFER_NONBLOCKING:%i\n",(buf->read_opt & BUFFER_NONBLOCKING));

			*data_len = 0;
			return SLIMAUDIO_BUFFER_STREAM_CONTINUE;
		}

		buf->reader_blocked = true;

		DEBUGF("buffer_read  %p write_ptr=%p read_ptr=%p available=%i reader_blocked=%i writer_blocked=%i\n",
			buf, buf->write_ptr, buf->read_ptr, buf->read_stream->available, buf->reader_blocked,
			buf->writer_blocked);

		pthread_cond_wait(&buf->write_cond, &buf->buffer_mutex);
	}

	VDEBUGF("buffer_read  %p write_ptr=%p read_ptr=%p available=%i reader_blocked=%i writer_blocked=%i\n",
		buf, buf->write_ptr, buf->read_ptr, buf->read_stream->available, buf->reader_blocked,
		buf->writer_blocked);

	/* when the stream is complete, move on */
	while (buf->read_stream->available == 0) {
		if (buf->read_stream->user_data != NULL)
			free(buf->read_stream->user_data);
			
		buf->read_stream = buf->read_stream->next;		
	}

	/* limit to buffered data */
	len = (buf->read_stream->available < len) ? buf->read_stream->available : len;
	assert(len > 0);

	buf->total_available -= len;
	buf->read_stream->available -= len;

	int trailing_data = buf->buffer_end - buf->read_ptr;
	if (len < trailing_data) {
		/* sufficient trailing data */
		memcpy(data, buf->read_ptr, len);
		buf->read_ptr += len;
		if (buf->read_ptr >= buf->buffer_end)
			buf->read_ptr = buf->buffer_start;		
	}
	else {
		/* insufficient trailing data */
		memcpy(data, buf->read_ptr, trailing_data);
		data += trailing_data;

		/* copy remainder from start */		
		int remainder_len = len - trailing_data;
		memcpy(data, buf->buffer_start, remainder_len);
		buf->read_ptr = buf->buffer_start + remainder_len;		
	}

	if (buf->writer_blocked) {
		buf->writer_blocked = false;
		pthread_cond_signal(&buf->read_cond);	
	}
	
	slimaudio_buffer_status status = SLIMAUDIO_BUFFER_STREAM_CONTINUE;

	if (buf->read_stream->read_count == 0) {
		status = SLIMAUDIO_BUFFER_STREAM_START;		
	}

	buf->read_stream->read_count += len;
	*data_len = len;

	if ( (buf->read_stream->available == 0) && buf->read_stream->eof) {
		DEBUGF("slimaudio_buffer_read EOF\n");
		status = SLIMAUDIO_BUFFER_STREAM_END;
	}
	
	pthread_mutex_unlock(&buf->buffer_mutex);
	
	return status;
}

int slimaudio_buffer_available(slimaudio_buffer_t *buf) {
	return buf->total_available;
}

