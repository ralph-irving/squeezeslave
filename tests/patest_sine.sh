#!/bin/bash
gcc -I../lib/libportaudio/mingw32/include -s -mno-cygwin -o patest_sine patest_sine.c ../lib/libportaudio/mingw32/lib/libportaudio.a -lm -lstdc++ -lws2_32 -lwinmm -lole32 -luuid
