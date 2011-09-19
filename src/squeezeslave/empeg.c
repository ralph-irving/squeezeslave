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


#ifdef EMPEG
#include "squeezeslave.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <net/if.h>
#include <ctype.h>
#include <fcntl.h>

#include <unistd.h>
#include <sys/mman.h>

#include <sys/ioctl.h>
#include <asm/arch-sa1100/empeg.h>
#include <linux/soundcard.h>

#define DISPLAY_DEV "/dev/display"
#define POWER_DEV "/dev/empeg_power"
#define IR_DEV "/dev/ir"
#define MIXER_DEV "/dev/mixer"
#define STATE_DEV "/dev/empeg_state"

/*
 * Front panel buttons
 */

#define IR_TOP_BUTTON_PRESSED       0x00000000
#define IR_TOP_BUTTON_RELEASED      0x00000001
#define IR_RIGHT_BUTTON_PRESSED     0x00000002
#define IR_RIGHT_BUTTON_RELEASED    0x00000003
#define IR_LEFT_BUTTON_PRESSED      0x00000004
#define IR_LEFT_BUTTON_RELEASED     0x00000005
#define IR_BOTTOM_BUTTON_PRESSED    0x00000006
#define IR_BOTTOM_BUTTON_RELEASED   0x00000007
#define IR_KNOB_BUTTON_PRESSED      0x00000008
#define IR_KNOB_BUTTON_RELEASED     0x00000009
#define IR_KNOB_UP                  0x0000000A
#define IR_KNOB_DOWN                0x0000000B

extern const int linelen;

int vfd_fd, power_fd, mixer_fd, ir_fd, state_fd;
caddr_t vfd_map;

volatile struct empeg_state_t empeg_state;

#define VFD_BRT_DEFAULT 100
int vfd_brt = VFD_BRT_DEFAULT;

char empeg_eq_sections[sizeof(struct empeg_eq_section_t) * 20 + 1];

int empeg_getmac(char * mac_addr)
{
    int s;
    struct ifreq buffer;

    s = socket(PF_INET, SOCK_DGRAM, 0);

    memset(&buffer, 0x00, sizeof(buffer));

    strcpy(buffer.ifr_name, "eth0");

    ioctl(s, SIOCGIFHWADDR, &buffer);

    close(s);

    memcpy(mac_addr, buffer.ifr_hwaddr.sa_data, 6);

    return 0;
}

void empeg_vfdbrt(int bright)
{
   if (bright <= 0)
   {
      ioctl(vfd_fd, EMPEG_DISPLAY_POWER, 0); // turn LCD and amp remote off
   }
   else
   {
      ioctl(vfd_fd, EMPEG_DISPLAY_POWER, 1); // turn LCD and amp remote on
      ioctl(vfd_fd, EMPEG_DISPLAY_SETBRIGHTNESS, &bright);
   }
}

// Close LCD connection (if open)
void close_lcd(void)
{
   empeg_vfdbrt(0);
   munmap(vfd_map, 2048);
   close (vfd_fd);
   close (power_fd);
   close (mixer_fd);
   close (ir_fd);
   close (state_fd);
}

void empeg_puteq_tofile(void)
{
   int wr_fd, io_fd, rc, four_ch;

   fprintf(stderr, "Writing current EQ to 'eq.dat'\n");
   io_fd = open(MIXER_DEV, O_RDWR);
   wr_fd = open("eq.dat", O_WRONLY | O_CREAT);
   ioctl(io_fd, EMPEG_MIXER_GET_EQ_FOUR_CHANNEL, &four_ch);
   empeg_eq_sections[0] = four_ch;
   ioctl(io_fd, EMPEG_MIXER_GET_EQ, &empeg_eq_sections[1]);
   rc = write(wr_fd, empeg_eq_sections, sizeof(empeg_eq_sections));
   fprintf(stderr, "Bytes written %i\n", rc);
   close(wr_fd);
   close(io_fd);
}

// convert from -6dB(32) - 0dB(64) - +6dB(128) to DSP value
int eq_gain[13] = {32, 36, 40, 44, 50, 56, 64, 72, 82, 90, 102, 114, 128 };

/*void empeg_seteq_section(int channel, int band, int cent_freq, int q, int gaindb)
{
   int i = channel * 5 + band;

   if (gaindb > 6) gaindb = 6;
   if (gaindb < -6) gaindb = -6;

   empeg_eq_sections[i].word1 = cent_freq;
   empeg_eq_sections[i].word2 = q << 7 | (eq_gain[gaindb + 6]);
}*/

void empeg_seteq(void)
{
   int four_ch = empeg_eq_sections[0];
   /* Set all EQ sections */
   ioctl(mixer_fd, EMPEG_MIXER_SET_EQ, &empeg_eq_sections[1]);
   /* Set EQ to 4 channel */
   ioctl(mixer_fd, EMPEG_MIXER_SET_EQ_FOUR_CHANNEL, &four_ch);
}

void empeg_geteq_fromfile(void)
{
   int fd = open("eq.dat", O_RDONLY);
   memset(empeg_eq_sections, 0, sizeof(empeg_eq_sections));
   if (read(fd, empeg_eq_sections, sizeof(empeg_eq_sections)) > 0)
      empeg_seteq();
   else
      fprintf(stderr, "Can't read EQ data.\n");
   close(fd);
}

// If it succeeeds configure hardware
// If it fails, print a message, disable support and continue
void empeg_init(void)
{
   vfd_fd = open(DISPLAY_DEV, O_RDWR);
   if (vfd_fd <= 0)
   {
      fprintf(stderr,"Connect to LCD failed!\n");
      exit(1);
   }

   vfd_map = mmap(0,2048, PROT_READ | PROT_WRITE, MAP_SHARED, vfd_fd, 0);
   if (vfd_map == (caddr_t) -1)
   {
      close(vfd_fd);
      return;
   }
   memset(vfd_map, 0, 2048); // clear VFD
   ioctl(vfd_fd, EMPEG_DISPLAY_REFRESH);

   power_fd = open(POWER_DEV, O_RDWR);

   mixer_fd = open(MIXER_DEV, O_RDWR);

   ir_fd = open(IR_DEV, O_RDONLY/* | O_NDELAY*/);

   state_fd = open(STATE_DEV, O_RDWR | O_SYNC);
   read(state_fd, (void *)&empeg_state, sizeof(empeg_state));
   if (empeg_state.signature != 0x4D494C53)
   {
      memset((void *)&empeg_state, 0, sizeof(empeg_state));
      empeg_state.signature = 0x4D494C53;
   }

   fprintf(stderr,"Empeg hardware init complete!\n");
}

void empeg_writestate(void)
{
   write(state_fd, (void *)&empeg_state, sizeof(empeg_state));
}

bool power_flag = false;

int empeg_idle(void)
{
   static int bright_last = -1;
   static unsigned long power_timer = 0, state_timer = 0;
   int bright;
   unsigned int pwr_state;
   struct timeval tnow;

   gettimeofday(&tnow, NULL);
   ioctl(power_fd, EMPEG_POWER_READSTATE, &pwr_state);

   if (vfd_brt < 0)
   {
      /* Automatic brightness */
      if (pwr_state & EMPEG_POWER_FLAG_LIGHTS)
         bright = VFD_BRT_DEFAULT * 0.7;
      else
         bright = VFD_BRT_DEFAULT;
   }
   else
      bright = vfd_brt;

   if (bright != bright_last)
   {
      bright_last = bright;
      empeg_vfdbrt(bright);
   }


   if ((pwr_state & EMPEG_POWER_FLAG_DC) && !(pwr_state & EMPEG_POWER_FLAG_ACCESSORY))
   {
      if (!power_flag)
      {
         power_flag = true;
         power_timer = tnow.tv_sec;
         fprintf(stderr, "Power state:0x%X\n", pwr_state);
         return -1; // pause return code
      }
      else if (power_timer + 300 < tnow.tv_sec)
         return -4; // power down return code
      return -2;
   }
   else if (power_flag)
   {
      power_flag = false;
      return -3;
   }

   if (state_timer + 60 < tnow.tv_sec)
   {
      state_timer = tnow.tv_sec;
      empeg_writestate();
   }

   return 0;
}


void empeg_poweroff(void)
{
   empeg_vfdbrt(0);
   empeg_writestate();
   /* Wait for VFD/amp remote to turn off */
   sleep(5);
   /* Turn off empeg */
   ioctl(power_fd, EMPEG_POWER_TURNOFF, 0);
}

struct
{
    bool down;
    unsigned long timer;
    long downcode;
    long upcode;
    long downir;
    long upir;
} keys[5] = {
//   {false, 0, IR_TOP_BUTTON_PRESSED, IR_TOP_BUTTON_RELEASED, 0x00010012, 0x00020012 },
   {false, 0, IR_TOP_BUTTON_PRESSED, IR_TOP_BUTTON_RELEASED, 0x00010017, 0x00020017 },
   {false, 0, IR_BOTTOM_BUTTON_PRESSED, IR_BOTTOM_BUTTON_RELEASED, 0x0001000D, 0x0002000D },
   {false, 0, IR_RIGHT_BUTTON_PRESSED, IR_RIGHT_BUTTON_RELEASED, 0x00010011, 0x00020011 },
   {false, 0, IR_LEFT_BUTTON_PRESSED, IR_LEFT_BUTTON_RELEASED, 0x00010010, 0x00020010 },
   {false, 0, IR_KNOB_BUTTON_PRESSED, IR_KNOB_BUTTON_RELEASED, 0x0001000E, 0x0002000E },
};

#define REPEAT_RATE 30

long empeg_getkey(void)
{
   int i;
   long key = -1;
   struct pollfd pollfd = { ir_fd, POLLIN, 0 };
   struct timeval tnow;
   unsigned long timer;

   gettimeofday(&tnow, NULL);
   timer = tnow.tv_sec * 1000 + tnow.tv_usec / 1000;

   for (i = 0; i < 5; i ++)
   {
      if (keys[i].down && keys[i].timer + (1000 / REPEAT_RATE) < timer)
      {
         keys[i].timer = timer;
         return keys[i].downcode;
      }
   }

   if (!poll(&pollfd, 1, (1000 / REPEAT_RATE))) return -1;
   read(ir_fd, (char *) &key, 4);
   return key;
}

long empeg_getircode(long key)
{
   int i;
   struct timeval tnow;
   unsigned long timer;

   gettimeofday(&tnow, NULL);
   timer = tnow.tv_sec * 1000 + tnow.tv_usec / 1000;

   if (key == IR_KNOB_UP)
      return 0x0001005B;
   else if (key == IR_KNOB_DOWN)
      return 0x0001005A;

   for (i = 0; i < 5; i ++) 
   {
      if (key == keys[i].downcode)
      {
         keys[i].down = true;
         keys[i].timer = timer;
         return keys[i].downir;
      }
      else if (key == keys[i].upcode)
      {
         if (!keys[i].down) return 0;
         keys[i].down = false;
         return keys[i].upir;
      }
   }
   return key; /* Pass code through if unknown */
}

/* Called by the library when a vfd grfe command is received */
int empeg_vfd_callback(slimproto_t *p, const unsigned char * buf, int len , void *user_data) 
{
   int i = 10, j, row, clm;
   char temp;
   unsigned char msg[SLIMPROTO_MSG_SIZE];
   int offset = buf[6] << 8 | buf[7]; // unpackN2
   char transition = buf[8];
//   char tParam = buf[9];
   int width = (len - 9) / 4;

   if (width != 128 || offset != 0) return 0;

   if (transition != 'c')
   {
      /* We don't handle transitions, we just reply animation complete */
      memset(msg, 0, sizeof(msg));
      packA4(msg, 0, "ANIC");
      packN4(msg, 4, 0);
      slimproto_send(p, msg);
   }
   if (p->state == PROTO_CONNECTED) {
       /* Translate from row->column 1 bpp to column->row, 4 bpp */
       for (clm = 0; clm < 128/2; clm ++)
       {
          for (row = 0; row < 32; row += 8, i ++)
          {
             temp = buf[i];
             for (j = 0; j < 8; j ++)
             {
                vfd_map[((row + j) << 6) + clm] = temp >> 7 ? 0x3 : 0; // Set both bits if pixel on
                temp <<= 1;
             }
          }
          for (row = 0; row < 32; row += 8, i ++)
          {
             temp = buf[i];
             for (j = 0; j < 8; j ++)
             {
                vfd_map[((row + j) << 6) + clm] |= (temp >> 7 ? 0x3 : 0) << 4;
                temp <<= 1;
             }
          }
       }

       /* Delay for 5ms as stacking writes to the display too fast causes problems */
       Pa_Sleep(5);
       ioctl(vfd_fd, EMPEG_DISPLAY_REFRESH);
   }
   return 0;
}

int empeg_vfdbrt_callback(slimproto_t *p, const unsigned char * buf, int len , void *user_data)
{
   int b = buf[6] << 8 | buf[7];

   if (b < 0 || b > 5)
      vfd_brt = -1;
   else
      vfd_brt = b * 20;

   return 0;
}


int empeg_aude_callback(slimproto_t *p, const unsigned char * buf, int len , void *user_data)
{
   static bool prevState = false;
   empeg_state.power_on = buf[6] || buf[7];

   if (prevState != empeg_state.power_on)
   {
      int source = SOUND_MIXER_PCM;

      prevState = empeg_state.power_on;
      if (!empeg_state.power_on) /* If powered off, switch to aux in */
         source = SOUND_MIXER_LINE;
      ioctl(mixer_fd, EMPEG_MIXER_WRITE_SOURCE, &source);
   }

   return 0;
}

#endif
