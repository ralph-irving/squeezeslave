#include <curses.h>

int main( int argc, char** argv )
{
	int key;
	initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, FALSE);
	printw("getch() should produce several values when hitting an arrow.\n");
	while ( (key = getch() ) != 'q' ) {
		printw ("%c:%d:0%o\n", key, key, key );
	}
	endwin();
	return 0;
}
