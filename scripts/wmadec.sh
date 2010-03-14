#!/usr/bin/perl -w

#      
# This script is just a wrapper for mplayer that tries to behave 
# the same way as Slim Device's closed source wmadec.exe.
#

$0 =~ s|.*/||; # basename

use Getopt::Long qw(:config gnu_getopt);

GetOptions(
  'd|debug'   => \$DEBUG,
  'v|verbose' => \$VERBOSE,
  'h|help'    => \&usage,
  'r|b|n=s'   => sub { },
) || usage();

$URL = shift(@ARGV);                                                        
$URL   || usage();
@ARGV  && usage();
$DEBUG && $VERBOSE++;

if ( $URL =~ /^http:/ ) {
  $URL = "-playlist $URL";
}

$msglevel = ( $DEBUG ? 9 : $VERBOSE ? 1 : -9 );
$ENV{MPLAYER_VERBOSE} = "$msglevel";

$cmd =  q{ mplayer }
     .  q{  -cache 128 }
     .  q{  -novideo -vc null -vo null }
     .  q{  -af volume=0,resample=44100:0:1,channels=2 }
     .  q{  -ao pcm:file=/dev/fd/4 }
     . qq{   $URL }
     . qq{    4>&1 1>&2 };
$cmd .= q{    2>/dev/null } unless $VERBOSE;

exec("$cmd");

###############
# Subroutines #
###############

sub usage
{
print STDERR <<"EOF";

Usage:

  $0 -[h|-help] -[v|-verbose] -[d|-debug] <URL>

Examples:

  $0 mms://media3.abc.net.au/news-radio
  $0 http://publicbroadcast.net/njn/ppr/njn.asx

EOF
exit 1;
}

