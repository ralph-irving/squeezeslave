#include <curses.h>

int main( int argc, char** argv )
{
	int key;
	initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	printw("getch() should produce only one value when hitting an arrow.\n");
	while ( (key = getch() ) != 'q' ) {
                printw ("%c:%d:0%o\n", key, key, key );

	}
	endwin();
	return 0;
}
