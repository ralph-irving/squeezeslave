all:
	gcc -Wall -O2 testCursesKeypadTrue.c -o testTrue -lcurses
	gcc -Wall -O2 testCursesKeypadFalse.c -o testFalse -lcurses
