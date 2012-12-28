/*
    Write pcm data from alsa device to file.

    gcc -o alsapcm2file alsapcm2file.c
*/
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#define BUFFLEN 8192

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>

int main(int argc, char **argv)
{
        int fd;
        int bytes_read;
        char buff[BUFFLEN];
        char fname[256];
        time_t now;
        struct tm datetime;
        now = time(NULL);
	datetime = *(localtime(&now));
	strftime(buff,BUFFLEN,"%Y%m%d%H%M%S", &datetime);
        sprintf(fname, "/home/dp130708/capture/%s.raw", buff);
        if((fd = open(fname, O_CREAT | O_WRONLY, S_IRUSR| S_IWUSR)) == -1) {
                perror("Failed to open file for writing");
                return(1);
        }
        while((bytes_read = read(0, buff, BUFFLEN))) {
                write(fd, buff, bytes_read);
        }
        close(fd);
        return(0);
}

/*
$ create ~/.asoundrc update path to alsapcm2file
xmcapture.pcm.file_truncate false
pcm.xmcapture {
type plug
slave { pcm "tee:hw,|/home/dp130708/capture/alsapcm2file"
        }
}
*/

/*
$ squeezeslave -L
Output devices:
 0: (ALSA) xmcapture (11/46)
*/
