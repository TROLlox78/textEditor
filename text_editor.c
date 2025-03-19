#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <stdbool.h>
// this line is very long because we have to check for long string length constantstreamofCharacters
// this line is another in the saga of long lines to test charcter scrolling this time with spaces
#define EDITOR_VERSION "0.0.1"

#define CTRL_KEY(k) ((k) & 0x1f)

typedef struct erow {
	int size;
  int renderSize;
	char* chars;
  char* render;
} erow;

struct editorConfig {
	int cx, cy;
  int oldx, oldy;
	int rowoffs, coloffs;  // offsets from default screen position
	int screenrows, screencols;  // total screen space
  int textrows, textcols; // space where text goes
	int numrows;            // number of rows in the document
  int marginSize;
	erow* row;
   

  bool isNumberMargin;
  bool isBottomBar;
  bool wrapText;
	struct termios org_termios;
};
struct editorConfig E;

enum editorKey {
	ARROW_LEFT = 1000,
	ARROW_RIGHT,
	ARROW_UP,
	ARROW_DOWN,
	HOME,
	END,
	DEL,
	PAGE_UP,
	PAGE_DOWN,

};

void die(const char* s){
	write(STDOUT_FILENO, "\x1b[2J", 4); 
	write(STDOUT_FILENO, "\x1b[H", 3); 
	perror(s);
	exit(1);
}

void disableRawMode(){
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.org_termios)== -1)
		die("tcsetattr");
}

void enableRawMode(){
	if (tcgetattr(STDIN_FILENO, &E.org_termios)==-1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.org_termios;
	tcgetattr(STDIN_FILENO, &raw);
	
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // fix ctrl+m, disable ctrl+s ctrl+q
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // disable echo, cannonical mode, ctrl+v ,ctrl+c ctrl+z
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 1;

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw)==-1)die("tcsetattr");
}

int editorReadKey(){
	int nread;
	char c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	if (c == '\x1b'){ // if 27h
		char seq[3];

		if (read(STDIN_FILENO, &seq[0], 1) != 1 ) return '\x1b';
		if (read(STDIN_FILENO, &seq[1], 1) != 1 ) return '\x1b';
		
		if (seq[0] == '['){
			
			if (seq[1]>='0' && seq[1]<='9'){
				if (read(STDIN_FILENO, &seq[2], 1) != 1 ) return '\x1b';
				if (seq[2] == '~'){
					switch (seq[1]){
						case '1': return HOME;
            case '3': return DEL;
            case '4': return END;
						case '5': return PAGE_UP;
						case '6': return PAGE_DOWN;
						case '7': return HOME;
            case '8': return END;
					}
				}
			} else{
				switch (seq[1]){
					case 'A': return ARROW_UP;
					case 'B': return ARROW_DOWN;
					case 'C': return ARROW_RIGHT;
					case 'D': return ARROW_LEFT;
					case 'H': return HOME;
					case 'F': return END;
				}
			}
		}
		else if (seq[0]== '0'){
			switch (seq[1]) {
				case 'H': return HOME;
				case 'F': return END;
      }
		}
		return '\x1b';

	}else {

		return c;
	}
}
int getWindowSize(int *rows, int *cols){
	struct winsize ws;

	if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
		return -1;
	}else{
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

/*** file IO */

void editorUpdateRow(erow* row){
  // EVERY TAB IS CONVERTED TO 2 SPACES;
  free(row->render);
  row->render = malloc(row->size+1);
  row->renderSize = row->size+1;
  int idx = 0;
  for (int i =0; i<row->size; i++){
    if (row->chars[i]=='\t'){
     row->render[idx++] = ' ';
     row->render[idx++] = ' ';
     row->renderSize++;
     row->render = realloc(row->render, row->renderSize+1);
    }else{
      row->render[idx++] = row->chars[i];
    }
  }
  row->render[idx]='\0';
}

void editorAppendRow( char* s, ssize_t len ){
	E.row = realloc(E.row, sizeof(erow)*(E.numrows+1));

	int at = E.numrows;
	E.row[at].size = len;
	E.row[at].chars = malloc(len +1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';
	E.numrows += 1;

  E.row[at].renderSize = 0;
  E.row[at].render = NULL;

  editorUpdateRow(&E.row[at]);
}


void editorOpen(char* filename){
	
	FILE* fp = fopen(filename, "r");
	if (!fp) die ("fopen");

	char* line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	while ((linelen = getline(&line, &linecap, fp)) != -1) {
		while (linelen >0 && (line[linelen -1] == '\n' || 
							line[linelen -1] == '\r' )){
			linelen--;
			}
		editorAppendRow(line,linelen);
	}
	free(line);
	fclose(fp);

}



/*** write buffer */

struct wbuf {
	char *b;
	int len;
};
#define WBUF_INIT {NULL,0}

void wbAppend(struct wbuf *wb, const char* s, int len){
	char* new = realloc(wb->b, wb->len + len);

	if (new == NULL) return;
	memcpy(&new[wb->len], s, len);
	wb->b = new;
	wb->len +=len;
}
void wbFree(struct wbuf *wb){
	free(wb->b);
}
char* lineNumber(int number){
  char chArr[5];
  char* numString = chArr;
  sprintf(numString, "%3d  ", number+1);

  return numString;
}

void drawLine(struct wbuf *wb, erow *row){
  // if lineLen>0ffset -> print line+offset
  // else print ""
  int of = E.coloffs;
  int lineLen = row->renderSize - of;

  if (lineLen < 0){
		wbAppend(wb, "",0);
    return;
  }
	if (lineLen>E.textcols){
    lineLen = E.textcols;
  }
	wbAppend(wb, row->render+of, lineLen);
}

void editorDrawRows(struct wbuf *wb){
	int y ;

  
	for (y=E.rowoffs; y< E.screenrows+E.rowoffs-E.isBottomBar; y++){
    wbAppend(wb, lineNumber(y),E.marginSize); // append a line number
//    wbAppend(wb, "\x1b[K", 3); // clear line maybe this don't do anythign?
		if (y<E.numrows){  // append line of characters
      
	
			// print lines
			//wbAppend(wb, E.row[y].chars, lineLen);
      drawLine(wb,E.row+y);
		}
		else{ // append eof symbol
			wbAppend(wb, "~", 1);
		}
    
    if (y<E.screenrows+E.rowoffs-1-E.isBottomBar){ // if not last line add clear
  		wbAppend(wb, "\x1b[K", 3); // clear line 
  		wbAppend(wb, "\r\n", 2); // next line
			wbAppend(wb, "\x1b[K", 3); // clear line
		     
    }
	}
  char bottom[100];
  sprintf(bottom, "     cur {x:%2d,%2d y:%2d,%2d} offs:%d, %d  txt:%dx%d scrn:%dx%d", 
          E.cx,E.oldx, E.cy,E.oldy, E.coloffs, E.rowoffs, E.textcols, E.textrows, E.screencols,E.screenrows);
    
	wbAppend(wb, "\r\n", 2); // next line
	wbAppend(wb, "\x1b[K", 3); // clear line
  wbAppend(wb,bottom,strlen(bottom));

	wbAppend(wb, "\x1b[K", 3); // clear line
}

erow* cursorLine(){
  if (E.cy+E.rowoffs<E.numrows)
    return &E.row[E.cy+E.rowoffs];
  
  return NULL;
}
void scrollCursor(){
  // to right side
  if (E.cx-E.coloffs+3>E.textcols){
    E.coloffs = E.cx-E.textcols +3 ;
  }// to the left side
   if (E.cx < E.coloffs) {
    E.coloffs=E.cx;
//    E.cx++;
  }
  if (E.cy+2> E.textrows ) { // to bottom
    E.rowoffs++;
    E.cy--;
//    E.cy--;
  }
  if (E.cy < 1 && E.rowoffs>0) { // to top
    E.rowoffs--;
    E.cy++;
  }
}

void editorRefreshScreen(){
  scrollCursor();
	struct wbuf wb = WBUF_INIT;

	wbAppend(&wb, "\x1b[?25l", 6); // hide cursor
	//wbAppend(&wb, "\x1b[2J", 4); // clear screen
	wbAppend(&wb, "\x1b[H", 3);  // reposition cursor to beginning

	//write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen
	//write(STDOUT_FILENO, "\x1b[H", 3);  // reposition cursor

	editorDrawRows(&wb);

	// printing the cursor to the screen
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy+1, E.cx+E.marginSize+1-E.coloffs); // reposition cursor
	wbAppend(&wb, buf, strlen(buf));

	
	wbAppend(&wb, "\x1b[?25h", 6); // show cursor
	//write(STDOUT_FILENO, "\x1b[H", 3); 

	write(STDOUT_FILENO, wb.b, wb.len);
	wbFree(&wb);

}
void editorMoveCursor(int key){
  switch (key) {
		case ARROW_LEFT:
			if (E.cx !=0){
				E.cx--;
        E.oldx = E.cx;
      }else if (E.cx ==0&& E.cy!=0){
        E.cy--;
        E.cx = cursorLine()->size;
        E.oldx = E.cx;
      }
			break;
		case ARROW_UP:
			if (E.cy !=0){
				E.cy--;

       if (cursorLine()->size <= E.oldx){
          E.cx = cursorLine()->size;

        }
        else {E.cx =E.oldx;}
      }
			break;
		case ARROW_DOWN:
			if (E.cy < E.screenrows -1 ){
				E.cy++;
       if (cursorLine()->size <= E.oldx){
          E.cx = cursorLine()->size;
        }
        else {E.cx =E.oldx;}
        // snap to line
      }
			break;
		case ARROW_RIGHT:
			if (E.cx - E.coloffs < E.screencols && E.cx< cursorLine()->renderSize-1){
				E.cx++;
        E.oldx = E.cx;
      }else if (E.cx>= cursorLine()->renderSize-1){
        E.cx= 0;
        E.cy++;
        E.oldx = E.cx;
      }
			break;
			  }

}
void editorRowOffset(int key){
	if (key == PAGE_DOWN){
		E.rowoffs+=1;
		return;
	}
	if (key == PAGE_UP && E.rowoffs>0)
	{E.rowoffs--;}
}

void editorProcessKeypress(){
	int c = editorReadKey();

	switch(c){
		case CTRL_KEY('q'):
			write(STDOUT_FILENO, "\x1b[2J", 4); 
			write(STDOUT_FILENO, "\x1b[H", 3); 
			exit(0);
			break;
    case CTRL_KEY('z'):
      E.coloffs--;
      E.cx--;
      E.oldx = E.cx;
      break;
    case CTRL_KEY('x'):
      E.coloffs++;
      E.cx++;
      E.oldx = E.cx;
      break;
		case ARROW_LEFT:
		case ARROW_RIGHT:
		case ARROW_UP:
		case ARROW_DOWN:
			editorMoveCursor(c);
			break;
		case PAGE_UP:
		case PAGE_DOWN:
			// temporary keybinding
			editorRowOffset(c);
			break;
		case HOME:
		case END:
			//cursor move
			break;
	}
}

void initEditor(){
	E.cx = 0;
  E.oldx = E.cx;
	E.cy = 0;
	E.rowoffs=0;
	E.coloffs=0;
	E.numrows = 0;
	E.row = NULL;
  E.isBottomBar = true;
  E.marginSize = 5;
  E.isNumberMargin = true;
	if (getWindowSize(&E.screenrows, &E.screencols) == -1 ){
		die("getWindowSize");
	}
  // setup textSpace
  E.textrows = E.screenrows - 1*E.isBottomBar; 
  E.textcols = E.screencols - E.marginSize*E.isNumberMargin;
}

int main (int argc, char* argv[]){
	enableRawMode();
	initEditor();
	if (argc>=2)
	editorOpen(argv[1]);

	while (1){
    editorRefreshScreen();
   
		editorProcessKeypress();
	}
	return 0;
}
