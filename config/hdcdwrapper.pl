
# 
# Author: George Hines, mrthreeplates@yahoo.com
#
# Note: because windows i/o redirection is brain dead, you must invoke this
# script with perl command.pl.  See:
#    http://community.activestate.com/faq/perl-redirect-problems-on-windows
#

use strict;
use diagnostics;
use warnings;

use File::Basename;

binmode (STDIN);
binmode (STDOUT);

my $basedir=dirname $0;
my $hdcd=$basedir . '\hdcd\hdcd.exe';

my $nframes = 750 + 1;  # Add an extra frame to include the WAV header
my $framesize = 2352;   # 2352bytes = 1/75th second @ 44.1khz 16bit stereo
my $chunksize = $nframes * $framesize;  # roughly 10 seconds...

my $buffer;
my $nbytes;

my $offset = 0;
my $count = $chunksize;
# Read enough to test for HDCD, may come in multiple reads
while ($count > 0 and ($nbytes = sysread (STDIN, $buffer, $count, $offset)) > 0) {
  $count -= $nbytes;
  $offset += $nbytes;
}
$nbytes = $offset;

my $hdcd_detected = 0;
# launch pipe to check for HDCD stream
if (-x $hdcd) {
  open (CMD, "| $hdcd -i 2>nul");
  binmode (CMD);
  syswrite (CMD, $buffer, $nbytes);
  $hdcd_detected = close (CMD);
} else {
  print STDERR "$0: can't find executable $hdcd\n";
}

if ($hdcd_detected) {
  #print STDERR "$0: decoding HDCD\n";
  #launch pipe to decode HDCD stream
  #   Note: the pipe (child process) will share and write to our STDOUT
  open (CMD, "| $hdcd 2>nul");
  binmode (CMD);
} else {
  #print STDERR "$0: no HDCD found, just copy input to output.\n";
  *CMD = *STDOUT;
}

# write out entire input stream
do {
  syswrite (CMD, $buffer, $nbytes);
} while (($nbytes = sysread (STDIN, $buffer, $chunksize/8)) > 0);

close (CMD);

exit (0);
