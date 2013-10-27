#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <ncurses.h>
#include <pthread.h>
#include <string.h>

#include "../error.h"
#include "../aldl-io.h"
#include "../config.h"
#include "../loadconfig.h"

enum {
  RED_ON_BLACK = 1,
  BLACK_ON_RED = 2,
  GREEN_ON_BLACK = 3,
  CYAN_ON_BLACK = 4,
  WHITE_ON_BLACK = 5
};

typedef enum _gaugetype {
  GAUGE_PROGRESSBAR,
  GAUGE_TEXT
} gaugetype_t;

typedef struct _gauge {
  int x, y; /* coords */
  int width, height; /* size */
  int data_a, data_b; /* assoc. data index */
  aldl_data_t prev_a, prev_b; /* prev. value */
  float bottom, top; /* bottom and top of a graph */
  gaugetype_t gaugetype;
} gauge_t;

typedef struct _consoleif_conf {
  int n_gauges;
  gauge_t *gauge; 
  dfile_t *dconf;
  int statusbar;
  int delay;
} consoleif_conf_t;

#define COLOR_STATUSSCREEN RED_ON_BLACK
#define COLOR_PROGRESSBAR RED_ON_BLACK

/* --- variables ---------------------------- */

int w_height, w_width; /* width and height of window */

aldl_conf_t *aldl; /* global pointer to aldl conf struct */

char *bigbuf; /* a large temporary string construction buffer */

aldl_record_t *rec; /* current record */

/* --- local functions ------------------------*/

consoleif_conf_t *consoleif_load_config(aldl_conf_t *aldl);

/* center half-width of an element on the screen */
int xcenter(int width);
int ycenter(int height);
 
/* print a centered string */
void print_centered_string(char *str);
void statusmessage(char *str);

/* clear screen and display waiting for connection messages */
void cons_wait_for_connection();

char *gconfig(char *parameter, int n);

void draw_h_progressbar(gauge_t *g);
void draw_statusbar();

/* --------------------------------------------*/

void *consoleif_init(void *aldl_in) {
  aldl = (aldl_conf_t *)aldl_in;

  bigbuf = malloc(512);

  /* load config file */
  consoleif_conf_t *conf = consoleif_load_config(aldl);

  /* initialize root window */
  WINDOW *root;
  if((root = initscr()) == NULL) {
    fatalerror(ERROR_NULL,"could not init ncurses");
  };

  curs_set(0);
  start_color();
  init_pair(RED_ON_BLACK,COLOR_RED,COLOR_BLACK);
  init_pair(BLACK_ON_RED,COLOR_BLACK,COLOR_RED);
  init_pair(GREEN_ON_BLACK,COLOR_GREEN,COLOR_BLACK);
  init_pair(CYAN_ON_BLACK,COLOR_CYAN,COLOR_BLACK);
  init_pair(WHITE_ON_BLACK,COLOR_WHITE,COLOR_BLACK);

  /* get initial screen size */
  getmaxyx(stdscr,w_height,w_width);

  cons_wait_for_connection();

  rec = newest_record(aldl);

  int x;

  while(1) {
    rec = newest_record_wait(aldl,rec);
    if(rec == NULL) { /* disconnected */
      cons_wait_for_connection();
      continue;
    };
    for(x=0;x<conf->n_gauges;x++) {
      draw_h_progressbar(&conf->gauge[x]);
    };
    if(conf->statusbar == 1) {
      draw_statusbar();
    };
    refresh();
    usleep(conf->delay);
  };

  sleep(4);
  delwin(root);
  endwin();
  refresh();

  pthread_exit(NULL);
  return NULL;
};

int xcenter(int width) {
  return ( w_width / 2 ) - ( width / 2 );
}

int ycenter(int height) {
  return ( w_height / 2 ) - ( height / 2 );
}

void print_centered_string(char *str) {
  mvaddstr(ycenter(0),xcenter(strlen(str)),str);
};

void draw_statusbar() {
  lock_stats();
  float pps = aldl->stats->packetspersecond;
  unsigned int failcounter = aldl->stats->failcounter;
  unlock_stats();
  mvprintw(w_height - 1,1,"%s  TIMESTAMP: %i  PKT/S: %.1f  FAILED: %u  ",
           VERSION, rec->t, pps, failcounter);
};

void statusmessage(char *str) {
  clear();
  attron(COLOR_PAIR(COLOR_STATUSSCREEN));
  print_centered_string(str);
  mvaddstr(1,1,VERSION);
  attroff(COLOR_PAIR(COLOR_STATUSSCREEN));
  refresh();
  usleep(400);
};

void cons_wait_for_connection() {
  aldl_state_t s = ALDL_LOADING;
  aldl_state_t s_cache = ALDL_LOADING; /* cache to avoid redraws */
  while(s > 10) {
    s = get_connstate(aldl);
    if(s != s_cache) statusmessage(get_state_string(s));
    s_cache = s;
    usleep(2000);
  };

  statusmessage("Buffering...");
  pause_until_buffered(aldl);

  clear();
}

void draw_h_progressbar(gauge_t *g) {
  aldl_define_t *def = &aldl->def[g->data_a];
  float data = rec->data[g->data_a].f;
  int x;
  /* blank out title section */
  move(g->y,g->x);   
  for(x=0;x<g->width;x++) addch(' ');
  /* draw output text */
  sprintf(bigbuf,"%.1f %s",data,def->uom);
  mvaddstr(g->y,g->x + g->width - strlen(bigbuf),bigbuf);
  /* draw title text */
  mvaddstr(g->y,g->x,def->name);
  /* draw progress bar */
  move(g->y + 1, g->x);
  int filled = data / ( g->top / g->width );
  int remainder = g->width - filled;
  attron(COLOR_PAIR(COLOR_PROGRESSBAR)); 
  for(x=0;x<filled;x++) { /* draw filled section */
    addch(' '|A_REVERSE);
  }; 
  for(x=0;x<remainder;x++) { /* draw unfilled section */
    addch('-');
  };
  attroff(COLOR_PAIR(COLOR_PROGRESSBAR));
};

consoleif_conf_t *consoleif_load_config(aldl_conf_t *aldl) {
  consoleif_conf_t *conf = malloc(sizeof(consoleif_conf_t));
  if(aldl->consoleif_config == NULL) fatalerror(ERROR_CONFIG,
                       "no consoleif config specified");
  conf->dconf = dfile_load(aldl->consoleif_config);
  if(conf->dconf == NULL) fatalerror(ERROR_CONFIG,
                       "consoleif config file missing");
  dfile_t *config = conf->dconf;
  /* GLOBAL OPTIONS */
  conf->n_gauges = configopt_int_fatal(config,"N_GAUGES",1,99999);
  conf->statusbar = configopt_int(config,"STATUSBAR",0,1,0);
  conf->delay = configopt_int(config,"DELAY",0,65535,0);
  /* PER GAUGE OPTIONS */
  conf->gauge = malloc(sizeof(gauge_t) * conf->n_gauges);
  gauge_t *gauge;
  int n;
  for(n=0;n<conf->n_gauges;n++) {
    gauge = &conf->gauge[n]; 
    /*FIXME need to be able to retrieve by ID too ... */
    gauge->data_a = get_index_by_name(aldl,configopt_fatal(config,
                                 gconfig("A_NAME",n)));
    gauge->x = configopt_int_fatal(config,gconfig("X",n),0,10000);
    gauge->y = configopt_int_fatal(config,gconfig("Y",n),0,10000);
    gauge->width = configopt_int_fatal(config,gconfig("WIDTH",n),0,10000);
    gauge->height = configopt_int(config,gconfig("HEIGHT",n),0,10000,1);
    gauge->bottom = configopt_float_fatal(config,gconfig("MIN",n));
    gauge->top = configopt_float_fatal(config,gconfig("MAX",n));
  };
  return conf;
};

char *gconfig(char *parameter, int n) {
  sprintf(bigbuf,"G%i.%s",n,parameter);
  return bigbuf;
};