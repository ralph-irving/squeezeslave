#!/bin/sh
scriptpid=$$
apppid=/tmp/.mplayer-app.$$.pid
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

if [ -e "${12}" ] ; then
    echo Local File: Launching $app "$@" >&2
    "$app" "$@" 3>&1 1>&2
    exit
fi

echo Streaming: Launching $app "$@" >&2
(
    exec 3>&1
    sleep 0.1
    killall $app
    "$app" "$@" 1>&2 & 
    echo $! > $apppid
    wait
    killall $app
    rm -f $apppid
    kill $scriptpid 2> /dev/null
) < /dev/null &

cat > /dev/null
if [ -s $apppid ] ; then
    kill `cat $apppid` 2> /dev/null
fi
