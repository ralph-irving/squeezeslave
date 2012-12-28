#include <stdio.h>
#include "libmms/mmsx.h"
#include "libmms/mms.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

const char *url = "mms://live.cumulusstreaming.com/KPLX-FM";

void my_log_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
  printf(fmt, vargs);
}

int64_t seek_data(void *opaque, int64_t offset, int whence)
{
  printf("SEEK DATA\n");
  return -1;
}

int main(int argc, char *argv[])
{
  mmsx_t *this = NULL;
  mmsx_t *this2 = NULL;
  char buf[2048];
  char buf2[2048];
  int i, res;
  FILE* f;
  AVFormatContext* pFormatCtx;

  av_log_set_callback(my_log_callback);
  av_log_set_level(AV_LOG_VERBOSE);

  av_register_all();

  int read_data(void *opaque, char *buf, int buf_size);  

  AVInputFormat* pAVInputFormat = av_find_input_format("asf");
  if(!pAVInputFormat)
  {
    printf("Probe not successful\n");
  }
  else
  {
    printf("Probe successfull------%s-------%s\n",pAVInputFormat->name,
pAVInputFormat->long_name);
  }

  if((this = mmsx_connect(NULL, NULL, url, 1)))
    printf("Connect OK\n");

  //pAVInputFormat->flags |= AVFMT_NOFILE;

  ByteIOContext ByteIOCtx;
  if(init_put_byte(&ByteIOCtx, buf, 2048, 0, this, read_data, NULL,
seek_data) < 0)
  {
    printf("init_put_byte not successful\n");
  }
  else
  {
    printf("init_put_byte successful\n");
  }
  ByteIOCtx.is_streamed = 1;

  int ires = av_open_input_stream(&pFormatCtx, &ByteIOCtx, "",
pAVInputFormat,NULL);
  
  if(ires < 0)
  {
    printf("Open input stream not successful %d\n",ires);
  }
  else
  {
    printf("Open input stream successfull %d\n",ires);
  }

}

int read_data(void *opaque, char *buf, int buf_size)
{
  mmsx_t *this22 = (mmsx_t *)opaque;
  int cnt = mmsx_read(NULL, opaque, buf, buf_size);
  return cnt;
}

