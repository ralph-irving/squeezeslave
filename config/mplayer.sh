#!/bin/bash
scriptpid=$$
apppid=/tmp/.alienbbc-app.$$.pid
app=mplayer

# For osx look for processor specific mplayer executable in locations used by mplayer installer
if [ `uname` = "Darwin" ] ; then
	if [ `uname -p` = "i386" ] && [ -x "/Applications/MPlayer OSX.app/Contents/Resources/External_Binaries/mplayer.app/Contents/MacOS/mplayer" ]; then
		app="/Applications/MPlayer OSX.app/Contents/Resources/External_Binaries/mplayer.app/Contents/MacOS/mplayer"
	elif [ `uname -p` = "powerpc" ] && [ -x "/Applications/MPlayer OSX.app/Contents/Resources/External_Binaries/mplayer_noaltivec.app/Contents/MacOS/mplayer" ]; then
		app="/Applications/MPlayer OSX.app/Contents/Resources/External_Binaries/mplayer_noaltivec.app/Contents/MacOS/mplayer"
	elif [ -x /usr/local/bin/mplayer ] ; then
		app=/usr/local/bin/mplayer
	fi
fi

if [ -e "${10}" ] ; then
    "$app" $1 $2 $3 $4 $5 $6 $7 $8 $9 "${10}" ${11} "${12}" "${13}" 3>&1 1>&2
    exit
fi

(
    "$app" $* 3>&1 1>&2 & 
    echo $! > $apppid
    wait
    rm -f $apppid
    kill $scriptpid 2> /dev/null
) < /dev/null &

if [ -s $apppid ] ; then
    kill `cat $apppid` 2> /dev/null
fi
