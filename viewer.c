#include "wc.h"
#include <SDL.h>
#include <SDL_ttf.h>

// TBD - is mutex needed on ctl.mode

// TBD LATER - copy fonts to this directory,  and incorporate in backup
// TBD LATER - debug commands to dump global data structures

//
// defines 
//

#define MAX_WEBCAM                  4
#define MAX_GETCL_ARGV              10
#define MAX_STR                     100

#define WIN_WIDTH_INITIAL           800
#define WIN_HEIGHT_INITIAL          480
#define CTL_WIDTH                   115

#define STATE_NO_WC_ID_STR         0
#define STATE_CONNECTING            1
#define STATE_CONNECTED             2
#define STATE_CONNECTING_ERROR      3
#define STATE_CONNECTED_ERROR       4
#define STATE_FATAL_ERROR           5

#define STATE_STR(state) \
   ((state) == STATE_NO_WC_ID_STR    ? "STATE_NO_WC_ID_STR"     : \
    (state) == STATE_CONNECTING       ? "STATE_CONNECTING"        : \
    (state) == STATE_CONNECTED        ? "STATE_CONNECTED"         : \
    (state) == STATE_CONNECTING_ERROR ? "STATE_CONNECTING_ERROR"  : \
    (state) == STATE_CONNECTED_ERROR  ? "STATE_CONNECTED_ERROR"   : \
    (state) == STATE_FATAL_ERROR      ? "STATE_FATAL_ERROR"         \
                                      : "????")

#define LONG_SLEEP_US               200000
#define SLEEP_US                    50000
#define SHORT_SLEEP_US              1000
#define HIGHLIGHT_TIME_US           2000000
#define RECONNECT_TIME_US           10000000

#define NO_HANDLE                   (-1)
#define NO_ZOOM                     (-1)
#define INVALID_ZOOM                (-2)

#define SDL_FLAGS                   (SDL_HWSURFACE | SDL_RESIZABLE)

#define FONT_PATH                   "/usr/share/fonts/gnu-free/FreeMonoBold.ttf"
#define FONTS_CHAR_WIDTH            fonts_cw
#define FONTS_CHAR_HEIGHT           fonts_ch
#define FONTL_CHAR_WIDTH            fontl_cw
#define FONTL_CHAR_HEIGHT           fontl_ch

#define IMAGE_DISPLAY_NONE    0
#define IMAGE_DISPLAY_TEXT    1
#define IMAGE_DISPLAY_IMAGE   2

#define CVT_INTERVAL_SECS_TO_DAY_HMS(dur_secs, days, hours, minutes, seconds) \
    do { \
        time_t d = (dur_secs); \
        (days) = d / 86400; \
        d -= ((days) * 86400); \
        (hours) = d / 3600; \
        d -= ((hours) * 3600); \
        (minutes) = d / 60; \
        d -= ((minutes) * 60); \
        (seconds) = d; \
    } while (0)

#define SET_CTL_MODE_LIVE() \
    do { \
        ctl.mode.mode = MODE_LIVE; \
        ctl.mode.pb_submode = PB_SUBMODE_STOP; \
        ctl.mode.pb_mode_entry_real_time_us = 0; \
        ctl.mode.pb_real_time_us = 0; \
        ctl.mode.pb_dir = PB_DIR_FWD; \
        ctl.mode.pb_speed = 1; \
        ctl.mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_STOP(init) \
    do { \
        ctl.mode.mode = MODE_PLAYBACK; \
        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) { \
            ctl.mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode); \
            if (ctl.mode.pb_real_time_us > (uint64_t)time(NULL) * 1000000) { \
                ctl.mode.pb_real_time_us = (uint64_t)time(NULL) * 1000000; \
            } \
        } \
        ctl.mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        if (init) { \
            ctl.mode.pb_real_time_us = get_real_time_us(); \
            ctl.mode.pb_dir = PB_DIR_FWD; \
            ctl.mode.pb_speed = 1; \
        } \
        ctl.mode.pb_submode = PB_SUBMODE_STOP; \
        ctl.mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_PAUSE(init) \
    do { \
        ctl.mode.mode = MODE_PLAYBACK; \
        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) { \
            ctl.mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode); \
        } \
        ctl.mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        if (init) { \
            ctl.mode.pb_real_time_us = get_real_time_us() - 2000000; \
            ctl.mode.pb_dir = PB_DIR_FWD; \
            ctl.mode.pb_speed = 1; \
        } \
        ctl.mode.pb_submode = PB_SUBMODE_PAUSE; \
        ctl.mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_PLAY() \
    do { \
        ctl.mode.mode = MODE_PLAYBACK; \
        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) { \
            ctl.mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode); \
        } \
        ctl.mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        ctl.mode.pb_submode = PB_SUBMODE_PLAY; \
        ctl.mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_TIME(delta_us) \
    do { \
        ctl.mode.mode = MODE_PLAYBACK; \
        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) { \
            ctl.mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode); \
        } \
        ctl.mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        ctl.mode.pb_real_time_us += (delta_us);   \
        if (ctl.mode.pb_submode == PB_SUBMODE_STOP) { \
            ctl.mode.pb_submode = PB_SUBMODE_PAUSE; \
        } \
        ctl.mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_DIR(dir) \
    do { \
        ctl.mode.mode = MODE_PLAYBACK; \
        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) { \
            ctl.mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode); \
        } \
        ctl.mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        ctl.mode.pb_dir = (dir); \
        ctl.mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_SPEED(speed)  \
    do { \
        ctl.mode.mode = MODE_PLAYBACK; \
        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) { \
            ctl.mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode); \
        } \
        ctl.mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        ctl.mode.pb_speed = (speed); \
        ctl.mode.mode_id++; \
    } while (0)

//
// typedefs
//

typedef struct {
    int             state;
    int             handle;
    uint64_t        last_state_change_time_us;
    uint64_t        last_status_msg_recv_time_us;
    uint64_t        last_dead_text_display_time_us;
    uint64_t        last_highlight_enable_time_us;
    uint32_t        last_frame_status;
    int32_t         last_zoom;
    struct mode_s   mode;
    struct status_s status;

    uint64_t        recvd_bytes;

    pthread_mutex_t image_mutex;
    char            image_wc_name[MAX_STR];
    bool            image_highlight;
    uint32_t        image_display;
    bool            image_update_request;
    uint8_t       * image;
    int             image_w;
    int             image_h;
    char            image_str1[MAX_STR];
    char            image_str2[MAX_STR];
    char            image_str3[MAX_STR];
} webcam_t;

typedef struct {
    SDL_Rect      wc_pos;
    SDL_Rect      win_id_pos;
    SDL_Rect      wc_name_pos;
    SDL_Rect      res_pos;
    SDL_Rect      image_pos;
    SDL_Rect      image_str1_pos;
    SDL_Rect      image_str2_pos;
    SDL_Rect      image_str3_pos;

    SDL_Overlay * ovl;
    int           ovl_w;
    int           ovl_h;

    bool          image_highlight_last;
    char          wc_name_last[MAX_STR];
    char          res_str_last[MAX_STR];
    uint32_t      mode_last;
    uint64_t      recvd_bytes_last;
} webcamdi_t;

typedef struct {
    struct mode_s mode;

    SDL_Rect ctl_pos;
    SDL_Rect mode_pos;
    SDL_Rect fulls_pos;
    SDL_Rect date_pos;
    SDL_Rect time_pos;
    SDL_Rect pb_time_pos;
    SDL_Rect pb_state_pos;
    SDL_Rect pb_stop_pos;
    SDL_Rect pb_play_pause_pos;
    SDL_Rect pb_dir_label_pos;
    SDL_Rect pb_dir_value_pos;
    SDL_Rect pb_speed_label_pos;
    SDL_Rect pb_speed_value_pos;
    SDL_Rect pb_sec_pos;
    SDL_Rect pb_sec_minus_pos;
    SDL_Rect pb_sec_plus_pos;
    SDL_Rect pb_min_pos;
    SDL_Rect pb_min_minus_pos;
    SDL_Rect pb_min_plus_pos;
    SDL_Rect pb_hour_pos;
    SDL_Rect pb_hour_minus_pos;
    SDL_Rect pb_hour_plus_pos;
    SDL_Rect pb_day_pos;
    SDL_Rect pb_day_minus_pos;
    SDL_Rect pb_day_plus_pos;
    SDL_Rect pb_record_dur_pos[MAX_WEBCAM];
    SDL_Rect con_info_title_pos;
    SDL_Rect con_info_pos[MAX_WEBCAM];
} ctl_t;

typedef struct {
    // general events
    bool quit_event;
    bool redisplay_event;
    bool fulls_event;
    bool con_info_event;

    // mode change event
    bool mode_event;

    // playback events
    bool pb_stop_event;
    bool pb_play_pause_event;
    bool pb_speed_event;
    char pb_speed_event_value_str[MAX_STR];
    bool pb_dir_event;
    bool pb_sec_minus_event;
    bool pb_sec_plus_event;
    bool pb_min_minus_event;
    bool pb_min_plus_event;
    bool pb_hour_minus_event;
    bool pb_hour_plus_event;
    bool pb_day_minus_event;
    bool pb_day_plus_event;

    // zoom event
    bool zoom_event;
    int  zoom_value;

    // window resize event
    bool resize_event;
    int  resize_w;
    int  resize_h;

    // webcam events
    struct wc_event_s {
        // webcam resolution event
        bool res_event;

        // webcam wc_name event
        bool wc_name_event;
        char wc_name[MAX_STR];
    } wc[MAX_WEBCAM];

    // used for text input
    bool wc_name_input_in_prog[MAX_WEBCAM];
    bool pb_speed_input_in_prog;
    char text_input_str[MAX_STR];
} event_t;

typedef uint32_t pixel_t;

//
// variables
//

p2p_routines_t * p2p;
char           * user_name;
char           * password;
SDL_Surface    * surface;
int              win_width;
int              win_height;
int              zoom;
int              webcam_threads_running_count;
bool             opt_zulu_time;

webcam_t         webcam[MAX_WEBCAM];
webcamdi_t       webcamdi[MAX_WEBCAM];
event_t          event;
ctl_t            ctl;

pixel_t          color_black;
pixel_t          color_white;
pixel_t          color_blue;
pixel_t          color_purple;

TTF_Font       * fonts;
TTF_Font       * fontl;
int              fonts_cw, fonts_ch;
int              fontl_cw, fontl_ch;

//
// prototypes
//

void event_handler(void);
void display_handler(void);
void * webcam_thread(void * cx);
void * debug_thread(void * cx);
bool getcl(int * argc, char ** argv);

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit  rl;
    char *         wc_name[MAX_WEBCAM];
    int            max_wc_name;
    pthread_t      debug_thread_id;
    pthread_t      webcam_thread_id[MAX_WEBCAM];
    char           opt_char;
    bool           opt_use_p2p2, opt_debug;
    char         * opt_zoom_str;  
    int            ret, i, count;

    // set resource limti to allow core dumps
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    ret = setrlimit(RLIMIT_CORE, &rl);
    if (ret < 0) {
        WARNING("setrlimit for core dump, %s\n", strerror(errno));
    }

    // get user_name and password from environment
    user_name = getenv("WC_USER_NAME");
    password  = getenv("WC_PASSWORD");

    // parse options
    opt_use_p2p2 = opt_debug = false;
    opt_zoom_str = NULL;
    while (true) {
        opt_char = getopt(argc, argv, "wdz:u:p:Z");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'w':
            opt_use_p2p2 = true;
            break;
       case 'd':
            opt_debug = true;
            break;
        case 'z':
            opt_zoom_str = optarg;
            opt_zoom_str[0] = toupper(opt_zoom_str[0]);
            break;
        case 'u':
            user_name = optarg;
            break;
        case 'p':
            password = optarg;
            break;
        case 'Z':
            opt_zulu_time = true;
            break;
        default:
            return 1;
        }
    }

    // verify user_name, password, and args
    if ((user_name == NULL) || (password == NULL) || 
        (opt_zoom_str != NULL && (strlen(opt_zoom_str) != 1 || opt_zoom_str[0] < 'A' || opt_zoom_str[0] > 'D')) ||
        (argc-optind > MAX_WEBCAM)) 
    {
        NOTICE("usage: wc_webcam <user_name> <password> <wc_name> ...\n");
        NOTICE("  -w: route network traffic to webcam via cloud server, using port 80\n");
        NOTICE("  -z <A|B|C|D>: sets zoom webcam\n");
        NOTICE("  -d: enable debug\n");
        NOTICE("  -u <user_name>: override WC_USER_NAME environment value\n");
        NOTICE("  -p <password>: override WC_PASSWORD environment value\n");
        NOTICE("  -Z: zulu time\n");
        return 1;
    }
    max_wc_name = 0;
    for (i = optind; i < argc; i++) {
        wc_name[max_wc_name++] = argv[i];
    }
    DEBUG("user_name=%s password=%s max_wc_name=%d\n", user_name, password, max_wc_name);
    for (i = 0; i < max_wc_name; i++) {
        DEBUG("wc_name[%d] = %s\n", i, wc_name[i]);
    }

    // init globals, non zero values only
    p2p         = !opt_use_p2p2 ? &p2p1 : &p2p2;
    win_width   = WIN_WIDTH_INITIAL;
    win_height  = WIN_HEIGHT_INITIAL;
    zoom        = NO_ZOOM;

    for (i = 0; i < MAX_WEBCAM; i++) {
        webcam[i].state = STATE_NO_WC_ID_STR;
        webcam[i].handle = NO_HANDLE;
        pthread_mutex_init(&webcam[i].image_mutex,NULL);
    }

    if (opt_zoom_str) {
        event.zoom_event = true;
        event.zoom_value = opt_zoom_str[0] - 'A';
    }
    for (i = 0; i < max_wc_name; i++) {
        event.wc[i].wc_name_event = true;
        strcpy(event.wc[i].wc_name, wc_name[i]);
    }

    SET_CTL_MODE_LIVE();

    // initialize Simple DirectMedia Layer  (SDL)
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        FATAL("SDL_Init failed\n");
    }
    if ((surface = SDL_SetVideoMode(win_width, win_height, 0, SDL_FLAGS)) == NULL) {
        FATAL("SDL_SetVideoMode failed\n");
    }
    SDL_WM_SetCaption("WEBCAM VIEWER", NULL);
    color_black  = SDL_MapRGB(surface->format, 0, 0, 0);
    color_white  = SDL_MapRGB(surface->format, 255, 255, 255);
    color_blue   = SDL_MapRGB(surface->format, 0, 0, 255);
    color_purple = SDL_MapRGB(surface->format, 0, 255, 255);

    // initialize True Type Font
    if (TTF_Init() < 0) {
        FATAL("TTF_Init failed\n");
    }
    fonts = TTF_OpenFont(FONT_PATH, 16);
    if (fonts == NULL) {
        FATAL("failed TTF_OpenFont %s\n", FONT_PATH);
    }
    fontl = TTF_OpenFont(FONT_PATH, 20);
    if (fontl == NULL) {
        FATAL("failed TTF_OpenFont %s\n", FONT_PATH);
    }
    TTF_SizeText(fonts, "X", &fonts_cw, &fonts_ch);
    TTF_SizeText(fontl, "X", &fontl_cw, &fontl_ch);

    // if debug mode enabled then create debug_thread
    debug_thread_id = 0;
    if (opt_debug > 0) {
        pthread_create(&debug_thread_id, NULL, debug_thread, NULL);
    }

    // create webcam threads 
    for (i = 0; i < MAX_WEBCAM; i++) {
        pthread_create(&webcam_thread_id[i], NULL, webcam_thread, (void*)(long)i);
    }

    // wait for up to 3 second for the webcam threads to initialize
    count = 0;
    while (webcam_threads_running_count != MAX_WEBCAM && count++ < 3000000/SLEEP_US) {
        usleep(SLEEP_US);
    }
    if (webcam_threads_running_count != MAX_WEBCAM) {
        FATAL("webcam threads failed to start\n");
    }

    // loop: process events and update display
    while (!event.quit_event) {
        event_handler();
        display_handler();
        usleep(SLEEP_US);
    }

    // wait for up to 3 second for the webcam threads to terminate
    count = 0;
    while (webcam_threads_running_count != 0 && count++ < 3000000/SLEEP_US) {
        usleep(SLEEP_US);
    }
    if (webcam_threads_running_count != 0) {
        WARNING("webcam threads failed to terminate\n");
    }

    // cancel the debug thread
    if (debug_thread_id) {
        pthread_cancel(debug_thread_id);
        pthread_join(debug_thread_id, NULL);
    }

    // SDL quit
    TTF_Quit();
    SDL_Quit();

    // return success
    return 0;
}

// -----------------  EVENT HANDLER  -------------------------------------

void event_handler(void)
{
    SDL_Event ev;
    SDLKey    key;
    bool      shift;
    int       i;

    #define MOUSE_AT_POS(pos) (ev.button.x >= (pos).x && \
                               ev.button.x < (pos).x + (pos).w && \
                               ev.button.y >= (pos).y && \
                               ev.button.y < (pos).y + (pos).h)

    #define CANCEL_TEXT_INPUT \
        do { \
            bzero(event.text_input_str, sizeof(event.text_input_str)); \
            bzero(event.wc_name_input_in_prog , sizeof(event.wc_name_input_in_prog)); \
            event.pb_speed_input_in_prog = false; \
        } while (0)

    //
    // loop while the quit_event is not set
    //

    while (!event.quit_event) {
        //
        // get the next event, break out of loop if no event
        //

        if (SDL_PollEvent(&ev) == 0) {
            break;
        }

        //
        // based on event received, set flags to notify other code to handle it
        //

        switch (ev.type) {
        case SDL_KEYDOWN:
            // set variables: key and shift
            key = ev.key.keysym.sym;
            shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;
            if (key >= 'a' && key <= 'z' && shift) {
                key -= ('a' - 'A');
            }
            if (key == '-' && shift) {
                key = '_';
            }

            // handle wc_name text input
            for (i = 0; i < MAX_WEBCAM; i++) {
                if (event.wc_name_input_in_prog[i]) {
                    if (isprint(key)) {
                        int len = strlen(event.text_input_str);
                        event.text_input_str[len] = key;
                    } else if (key == SDLK_BACKSPACE) {
                        int len = strlen(event.text_input_str);
                        if (len > 0) {
                            event.text_input_str[len-1] = '\0';
                        }
                    } else if (key == SDLK_ESCAPE) {
                        CANCEL_TEXT_INPUT;
                    } else if (key == SDLK_RETURN) {
                        strcpy(event.wc[i].wc_name, event.text_input_str);
                        event.wc[i].wc_name_event = true;
                        CANCEL_TEXT_INPUT;
                    }
                    break;
                }
            }
            if (i < MAX_WEBCAM) {
                break;
            }

            // handle pb_speed text input
            if (event.pb_speed_input_in_prog) {
                if (isprint(key)) {
                    int len = strlen(event.text_input_str);
                    event.text_input_str[len] = key;
                } else if (key == SDLK_BACKSPACE) {
                    int len = strlen(event.text_input_str);
                    if (len > 0) {
                        event.text_input_str[len-1] = '\0';
                    }
                } else if (key == SDLK_ESCAPE) {
                    CANCEL_TEXT_INPUT;
                } else if (key == SDLK_RETURN) {
                    strcpy(event.pb_speed_event_value_str, event.text_input_str);
                    event.pb_speed_event = true;
                    CANCEL_TEXT_INPUT;
                }
                break;
            }

            // check for program events that are associated with key events
            if (key == 'q') {
                event.quit_event = true;
                break;
            }
            if (key == 'r') {
                event.redisplay_event = true;
                break;
            }
            break;

        case SDL_KEYUP:
            break;

        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button != SDL_BUTTON_LEFT) {
                break;
            }

            CANCEL_TEXT_INPUT;
            break;

        case SDL_MOUSEBUTTONUP:
            DEBUG("MOUSE UP which=%d button=%s state=%s x=%d y=%d\n",
                   ev.button.which,
                   (ev.button.button == SDL_BUTTON_LEFT   ? "LEFT" :
                    ev.button.button == SDL_BUTTON_MIDDLE ? "MIDDLE" :
                    ev.button.button == SDL_BUTTON_RIGHT  ? "RIGHT" :
                                                            "???"),
                   (ev.button.state == SDL_PRESSED  ? "PRESSED" :
                    ev.button.state == SDL_RELEASED ? "RELEASED" :
                                                      "???"),
                   ev.button.x,
                   ev.button.y);

            if (ev.button.button != SDL_BUTTON_LEFT) {
                break;
            }

            CANCEL_TEXT_INPUT;

            // webcam events
            for (i = 0; i < MAX_WEBCAM; i++) {
                webcamdi_t * wcdi = &webcamdi[i];

                if (MOUSE_AT_POS(wcdi->image_pos)) {
                    event.zoom_event = true;
                    event.zoom_value = i;
                    break;
                }
                if (MOUSE_AT_POS(wcdi->res_pos) && ctl.mode.mode == MODE_LIVE) {
                    event.wc[i].res_event = true;
                    break;
                }
                if (MOUSE_AT_POS(wcdi->wc_name_pos)) {
                    event.wc_name_input_in_prog[i] = true;
                    break;
                }
            }
            if (i < MAX_WEBCAM) {
                break;
            }

            // control panel events
#ifdef FULLS_SUPPORT
            if (MOUSE_AT_POS(ctl.fulls_pos)) {
                event.fulls_event = true;
                break;
            }
#endif
            if (MOUSE_AT_POS(ctl.mode_pos)) {
                event.mode_event = true;
                break;
            }
            if (MOUSE_AT_POS(ctl.con_info_title_pos)) {
                event.con_info_event = true;
                break;
            }
            if (ctl.mode.mode == MODE_PLAYBACK) {
                if (MOUSE_AT_POS(ctl.pb_stop_pos)) {
                    event.pb_stop_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_play_pause_pos)) {
                    event.pb_play_pause_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_dir_value_pos)) {
                    event.pb_dir_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_speed_value_pos)) {
                    event.pb_speed_input_in_prog = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_sec_minus_pos)) {
                    event.pb_sec_minus_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_sec_plus_pos)) {
                    event.pb_sec_plus_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_min_minus_pos)) {
                    event.pb_min_minus_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_min_plus_pos)) {
                    event.pb_min_plus_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_hour_minus_pos)) {
                    event.pb_hour_minus_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_hour_plus_pos)) {
                    event.pb_hour_plus_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_day_minus_pos)) {
                    event.pb_day_minus_event = true;
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_day_plus_pos)) {
                    event.pb_day_plus_event = true;
                    break;
                }
            }
            break;

       case SDL_VIDEORESIZE:
            event.resize_w  = ev.resize.w;
            event.resize_h = ev.resize.h;
            event.resize_event = true;
            break;

        case SDL_QUIT:
            event.quit_event = true;
            break;
        }

        //
        // some of the event processing is done right here
        //

        // process mode change event
        if (event.mode_event) {
            if (ctl.mode.mode == MODE_PLAYBACK) {
                SET_CTL_MODE_LIVE();
            } else {
                SET_CTL_MODE_PLAYBACK_PAUSE(true);
            }
            event.mode_event = false;
        }

        // if not in MODE_PLAYBACK then clear all pb events and continue
        if (ctl.mode.mode != MODE_PLAYBACK) { 
            event.pb_stop_event        = false;
            event.pb_play_pause_event  = false;
            event.pb_speed_event       = false;
            event.pb_dir_event         = false;
            event.pb_sec_minus_event   = false;
            event.pb_sec_plus_event    = false;
            event.pb_min_minus_event   = false;
            event.pb_min_plus_event    = false;
            event.pb_hour_minus_event  = false;
            event.pb_hour_plus_event   = false;
            event.pb_day_minus_event   = false;
            event.pb_day_plus_event    = false;
            continue;
        }

        // process pb_stop_event
        if (event.pb_stop_event) {
            SET_CTL_MODE_PLAYBACK_STOP(false);
            event.pb_stop_event = false;
        }

        // process pb_play_pause_event
        if (event.pb_play_pause_event) {
            if (ctl.mode.pb_submode == PB_SUBMODE_STOP || ctl.mode.pb_submode == PB_SUBMODE_PAUSE) {
                SET_CTL_MODE_PLAYBACK_PLAY();
            } else {
                SET_CTL_MODE_PLAYBACK_PAUSE(false);
            }
            event.pb_play_pause_event = false;
        }

        // process pb_speed_event
        if (event.pb_speed_event) {
            double speed;
            if (sscanf(event.pb_speed_event_value_str, "%lf", &speed) == 1 &&
                speed >= PB_SPEED_MIN && speed <= PB_SPEED_MAX) 
            {
                SET_CTL_MODE_PLAYBACK_SPEED(speed);
            } else {
                WARNING("speed '%s' is invalid\n", event.pb_speed_event_value_str);
            }
            event.pb_speed_event = false;
        }

        // process pb_dir_event
        if (event.pb_dir_event) {
            SET_CTL_MODE_PLAYBACK_DIR(ctl.mode.pb_dir == PB_DIR_FWD ? PB_DIR_REV : PB_DIR_FWD);
            event.pb_dir_event = false;
        }

        // process pb_real_time_us change events
        int64_t delta_us = 0;
        if (event.pb_sec_minus_event) {
            delta_us = -1 * 1000000L;
            event.pb_sec_minus_event = false;
        }
        if (event.pb_sec_plus_event) {
            delta_us = 1 * 1000000L;
            event.pb_sec_plus_event = false;
        }
        if (event.pb_min_minus_event) {
            delta_us = -60 * 1000000L;
            event.pb_min_minus_event = false;
        }
        if (event.pb_min_plus_event) {
            delta_us = 60 * 1000000L;
            event.pb_min_plus_event = false;
        }
        if (event.pb_hour_minus_event) {
            delta_us = -3600 * 1000000L;
            event.pb_hour_minus_event = false;
        }
        if (event.pb_hour_plus_event) {
            delta_us = 3600 * 1000000L;
            event.pb_hour_plus_event = false;
        }
        if (event.pb_day_minus_event) {
            delta_us = -86400 * 1000000L;
            event.pb_day_minus_event = false;
        }
        if (event.pb_day_plus_event) {
            delta_us = 86400 * 1000000L;
            event.pb_day_plus_event = false;
        }
        if (delta_us != 0) {
            SET_CTL_MODE_PLAYBACK_TIME(delta_us);
        } 
    }

    //
    // if all connected webcam are eod or bod then stop playback
    //

    if (ctl.mode.mode == MODE_PLAYBACK && ctl.mode.pb_submode == PB_SUBMODE_PLAY) {
        bool all_eod = true;
        bool all_bod = true;
        for (i = 0; i < MAX_WEBCAM; i++) {
            webcam_t * wc = &webcam[i];
            if (wc->state == STATE_CONNECTED && wc->last_frame_status != STATUS_ERR_FRAME_BEFORE_BOD) {
                all_bod = false;
            }
            if (wc->state == STATE_CONNECTED && wc->last_frame_status != STATUS_ERR_FRAME_AFTER_EOD) {
                all_eod = false;
            }
        }
        if (all_eod || all_bod) {
            SET_CTL_MODE_PLAYBACK_STOP(false);
        }
    }
}

// -----------------  DISPLAY HANDLER  -----------------------------------

void display_handler(void)
{
    #define INIT_POS(r,_x,_y,_w,_h) \
        do { \
            (r).x = (_x); \
            (r).y = (_y); \
            (r).w = (_w); \
            (r).h = (_h); \
        } while (0)

#define INIT_CTL_POS(fld,r,c,len) \
        do { \
            INIT_POS(ctl.fld, \
                     ctl_x + FONTS_CHAR_WIDTH/2 + (c) * FONTS_CHAR_WIDTH, \
                     ctl_y + (r) * FONTS_CHAR_HEIGHT + 1,  \
                     (len) * FONTS_CHAR_WIDTH,  \
                     FONTS_CHAR_HEIGHT); \
        } while (0)

    #define RENDER_BORDER(r,c) \
        do { \
            SDL_Rect line; \
            /* draw the 4 lines that make the border of the webcam window */ \
            line.x = (r).x; line.y = (r).y; line.w = (r).w; line.h = 1; \
            SDL_FillRect(surface, &line, (c)); \
            SDL_UpdateRect(surface, line.x, line.y, line.w, line.h); \
            line.y = (r).y + (r).h - 1; \
            SDL_FillRect(surface, &line, (c)); \
            SDL_UpdateRect(surface, line.x, line.y, line.w, line.h); \
            line.x = (r).x; line.y = (r).y; line.w = 1; line.h = (r).h; \
            SDL_FillRect(surface, &line, (c)); \
            SDL_UpdateRect(surface, line.x, line.y, line.w, line.h); \
            line.x = (r).x + (r).w - 1; \
            SDL_FillRect(surface, &line, (c)); \
            SDL_UpdateRect(surface, line.x, line.y, line.w, line.h); \
            /* draw the line below the title */ \
            line.x = (r).x; line.y = (r).y+FONTS_CHAR_HEIGHT+1; line.w = (r).w; line.h = 1; \
            SDL_FillRect(surface, &line, (c)); \
            SDL_UpdateRect(surface, line.x, line.y, line.w, line.h); \
        } while (0)

    #define RENDER_TEXT(font, str, pos, ctl) \
        do { \
            SDL_Surface *ts; \
            SDL_Color fg_color_normal = {255,255,255}; \
            SDL_Color fg_color_ctl = {0,255,255}; \
            SDL_Color bg_color = {0,0,0}; \
            SDL_FillRect(surface, &(pos), color_black); \
            ts = TTF_RenderText_Shaded((font), (str), (ctl) ? fg_color_ctl : fg_color_normal, bg_color); \
            if (ts) { \
                SDL_Rect pos2 = (pos); \
                SDL_BlitSurface(ts, NULL, surface, &pos2); \
                SDL_FreeSurface(ts); \
            }  \
            SDL_UpdateRect(surface, (pos).x, (pos).y, (pos).w, (pos).h); \
        } while (0)

    #define RENDER_TEXT_CENTERED(font, str, pos) \
        do { \
            SDL_Surface *ts; \
            SDL_Color fg_color = {255,255,255}; \
            SDL_Color bg_color = {0,0,0}; \
            SDL_FillRect(surface, &(pos), color_black); \
            ts = TTF_RenderText_Shaded((font), (str), fg_color, bg_color); \
            if (ts) { \
                SDL_Rect pos2 = (pos); \
                if (ts->w >= pos2.w) { \
                    SDL_BlitSurface(ts, NULL, surface, &pos2); \
                } else { \
                    pos2.x += (pos2.w - ts->w) / 2; \
                    pos2.w = ts->w; \
                    SDL_BlitSurface(ts, NULL, surface, &pos2); \
                } \
                SDL_FreeSurface(ts); \
            } \
            SDL_UpdateRect(surface, (pos).x, (pos).y, (pos).w, (pos).h); \
        } while (0)


    int               i;
    bool              display_all = false;

    static bool       first_call  = true;

    //
    // if first_call or event that requires redisplay
    //

    if (first_call || 
        event.zoom_event || 
        event.resize_event || 
#ifdef FULLS_SUPPORT
        event.fulls_event || 
#endif
        event.redisplay_event) 
    {
        int ctl_x, ctl_y, ctl_w, ctl_h;
        int small_win_count = 0;

        // clear first_call flag, and 
        // set display_all flag
        first_call = false;
        display_all = true;

        // handle redisplay event
        if (event.redisplay_event) {
            event.redisplay_event = false;
        }
            
        // handle resize event
        if (event.resize_event) {
            win_width = (event.resize_w >= WIN_WIDTH_INITIAL ? event.resize_w : WIN_WIDTH_INITIAL);
            win_height = (event.resize_h >= WIN_HEIGHT_INITIAL ? event.resize_h : WIN_HEIGHT_INITIAL);
            if ((surface = SDL_SetVideoMode(win_width, win_height, 0, SDL_FLAGS)) == NULL) {
                FATAL("SDL_SetVideoMode failed\n");
            }
            event.resize_event = false;
        }

#ifdef FULLS_SUPPORT
        // handle fulls event
        if (event.fulls_event) {
            SDL_WM_ToggleFullScreen(surface);
            event.fulls_event = false;
        }
#endif

        // handle zoom event
        if (event.zoom_event) {
            if (zoom == event.zoom_value) {
                zoom = NO_ZOOM;
            } else {
                zoom = event.zoom_value;
            }
            event.zoom_event = false;
        }

        // compute new ctl screen element positions
        ctl_x = win_width-CTL_WIDTH; 
        ctl_y = 0; 
        ctl_w = CTL_WIDTH; 
        ctl_h = win_height;
        INIT_POS(ctl.ctl_pos, ctl_x, ctl_y, ctl_w, ctl_h);
        INIT_CTL_POS(mode_pos,                0,  0,  8);
#ifdef FULLS_SUPPORT
        INIT_CTL_POS(fulls_pos,               0, 10,  1);
#endif
        INIT_CTL_POS(date_pos,                2,  0, 11);
        INIT_CTL_POS(time_pos,                3,  0, 11);
        INIT_CTL_POS(pb_time_pos,             4,  0, 11);
        INIT_CTL_POS(pb_state_pos,            5,  0, 11);
        INIT_CTL_POS(pb_stop_pos,             7,  0,  5);
        INIT_CTL_POS(pb_play_pause_pos,       7,  6,  5);
        INIT_CTL_POS(pb_dir_label_pos,        8,  0,  5);
        INIT_CTL_POS(pb_dir_value_pos,        8,  6,  5);
        INIT_CTL_POS(pb_speed_label_pos,      9,  0,  5);
        INIT_CTL_POS(pb_speed_value_pos,      9,  6,  5);
        INIT_CTL_POS(pb_sec_pos,             11,  0,  5);
        INIT_CTL_POS(pb_sec_minus_pos,       11,  6,  1);
        INIT_CTL_POS(pb_sec_plus_pos,        11,  9,  1);
        INIT_CTL_POS(pb_min_pos,             12,  0,  5);
        INIT_CTL_POS(pb_min_minus_pos,       12,  6,  1);
        INIT_CTL_POS(pb_min_plus_pos,        12,  9,  1);
        INIT_CTL_POS(pb_hour_pos,            13,  0,  5);
        INIT_CTL_POS(pb_hour_minus_pos,      13,  6,  1);
        INIT_CTL_POS(pb_hour_plus_pos,       13,  9,  1);
        INIT_CTL_POS(pb_day_pos,             14,  0,  5);
        INIT_CTL_POS(pb_day_minus_pos,       14,  6,  1);
        INIT_CTL_POS(pb_day_plus_pos,        14,  9,  1);
        INIT_CTL_POS(pb_record_dur_pos[0],   16,  0, 11);
        INIT_CTL_POS(pb_record_dur_pos[1],   17,  0, 11);
        INIT_CTL_POS(pb_record_dur_pos[2],   18,  0, 11);
        INIT_CTL_POS(pb_record_dur_pos[3],   19,  0, 11);

        INIT_CTL_POS(con_info_title_pos,     23,  0, 11);
        INIT_CTL_POS(con_info_pos[0],        24,  0, 11);
        INIT_CTL_POS(con_info_pos[1],        25,  0, 11);
        INIT_CTL_POS(con_info_pos[2],        26,  0, 11);
        INIT_CTL_POS(con_info_pos[3],        27,  0, 11);

        // compute new wc screen element positions
        for (i = 0; i < MAX_WEBCAM; i++) {
            webcamdi_t * wcdi = &webcamdi[i];
            int          wc_x, wc_y, wc_w, wc_h, wc_zw;

            // set wc_x, wc_y, wc_w, wc_h
            if (zoom == NO_ZOOM) {
                wc_w = (win_width - CTL_WIDTH) / 2;
                wc_h = win_height / 2;
                switch (i) {
                case 0: wc_x = 0;    wc_y = 0;    break;
                case 1: wc_x = wc_w; wc_y = 0;    break;
                case 2: wc_x = 0;    wc_y = wc_h; break;
                case 3: wc_x = wc_w; wc_y = wc_h; break;
                }
            } else {
                wc_zw = (double)(win_width - CTL_WIDTH) / 1.33;  
                if (i == zoom) {
                    wc_x = 0;
                    wc_y = 0;
                    wc_w = wc_zw;
                    wc_h = win_height;
                } else {
                    wc_x = wc_zw;
                    wc_y = small_win_count * (win_height / 3);
                    wc_w = win_width - CTL_WIDTH - wc_zw;
                    wc_h = (win_height / 3);
                    small_win_count++;
                }
            }
            INIT_POS(wcdi->wc_pos, wc_x, wc_y, wc_w, wc_h);
            INIT_POS(wcdi->win_id_pos, 
                     wc_x + FONTS_CHAR_WIDTH / 2,
                     wc_y + 1, 
                     2 * FONTS_CHAR_WIDTH, 
                     FONTS_CHAR_HEIGHT);
            INIT_POS(wcdi->wc_name_pos, 
                     wc_x + 2 * FONTS_CHAR_WIDTH + FONTS_CHAR_WIDTH / 2,
                     wc_y + 1, 
                     (wc_w / FONTS_CHAR_WIDTH - 7) * FONTS_CHAR_WIDTH, 
                     FONTS_CHAR_HEIGHT);
            INIT_POS(wcdi->res_pos,    
                     wc_x + wc_w - 3 * FONTS_CHAR_WIDTH - FONTS_CHAR_WIDTH / 2, 
                     wc_y + 1, 
                     3 * FONTS_CHAR_WIDTH, 
                     FONTS_CHAR_HEIGHT);
            INIT_POS(wcdi->image_pos,
                     wc_x + 1,
                     wc_y + 1 + FONTS_CHAR_HEIGHT + 1,
                     wc_w - 2 * 1,
                     wc_h - FONTS_CHAR_HEIGHT - 3 * 1);
            INIT_POS(wcdi->image_str1_pos,
                     wcdi->image_pos.x,
                     wcdi->image_pos.y + wcdi->image_pos.h / 2 - FONTL_CHAR_HEIGHT,
                     wcdi->image_pos.w,
                     FONTL_CHAR_HEIGHT);
            INIT_POS(wcdi->image_str2_pos,
                     wcdi->image_pos.x,
                     wcdi->image_pos.y + wcdi->image_pos.h / 2,
                     wcdi->image_pos.w,
                     FONTL_CHAR_HEIGHT);
            INIT_POS(wcdi->image_str3_pos,
                     wcdi->image_pos.x,
                     wcdi->image_pos.y + wcdi->image_pos.h / 2 + FONTL_CHAR_HEIGHT,
                     wcdi->image_pos.w,
                     FONTL_CHAR_HEIGHT);
        }
    }

    //
    // if display_all then clear screen
    //

    if (display_all) {
        SDL_FillRect(surface,NULL,color_black);
        SDL_UpdateRect(surface, 0, 0, 0, 0);
    }

    //
    // update the display for each webcam
    //

    for (i = 0; i < MAX_WEBCAM; i++) {
        webcam_t   * wc   = &webcam[i];
        webcamdi_t * wcdi = &webcamdi[i];
        char         wc_name_str[MAX_STR], res_str[MAX_STR], win_id_str[MAX_STR];
        uint64_t     curr_time_us = microsec_timer();

        // acquire wc mutex
        pthread_mutex_lock(&wc->image_mutex);

        // display border
        if (display_all || wc->image_highlight != wcdi->image_highlight_last) {
            RENDER_BORDER(wcdi->wc_pos, wc->image_highlight ? color_white : color_blue);
            wcdi->image_highlight_last = wc->image_highlight;
        }

        // display text line
        if (display_all) {
            sprintf(win_id_str, "%c", 'A'+i);
            RENDER_TEXT(fonts, win_id_str, wcdi->win_id_pos, false);
        }

        if (event.wc_name_input_in_prog[i]) {
            strcpy(wc_name_str, event.text_input_str);
            if ((curr_time_us % 1000000) < 500000) {
                strcat(wc_name_str, "_");
            } else {
                strcat(wc_name_str, " ");
            }
            int field_width    = wcdi->wc_name_pos.w / FONTS_CHAR_WIDTH;
            int len_wc_name_str = strlen(wc_name_str);
            if (len_wc_name_str > field_width) {
                memmove(wc_name_str, 
                        wc_name_str + (len_wc_name_str - field_width),
                        field_width + 1);
            }
        } else if (wc->image_wc_name[0] != '\0') {
            strcpy(wc_name_str, wc->image_wc_name);
        } else {
            strcpy(wc_name_str, "?");
        }
        if (display_all || strcmp(wc_name_str,wcdi->wc_name_last)) {
            RENDER_TEXT(fonts, wc_name_str, wcdi->wc_name_pos, true);
            strcpy(wcdi->wc_name_last, wc_name_str);
        }

        strcpy(res_str, 
               (wc->image_display != IMAGE_DISPLAY_IMAGE ? "   " :
                wc->image_w == 640                       ? "HI " :
                wc->image_w == 320                       ? "MED" :
                wc->image_w == 160                       ? "LOW" :
                                                           "?  "));
        if (display_all || strcmp(res_str,wcdi->res_str_last) || ctl.mode.mode != wcdi->mode_last) {
            RENDER_TEXT(fonts, res_str, wcdi->res_pos, ctl.mode.mode == MODE_LIVE);
            strcpy(wcdi->res_str_last, res_str);
            wcdi->mode_last = ctl.mode.mode;
        }

        // display image or text
        if (display_all || wc->image_update_request) {
            // clear image update request flag
            wc->image_update_request = false;

            // display image
            if (wc->image_display == IMAGE_DISPLAY_IMAGE) {
                // create new overlay, if needed
                if (wcdi->ovl == NULL || wcdi->ovl_w != wc->image_w || wcdi->ovl_h != wc->image_h) {
                    wcdi->ovl_w = wc->image_w;
                    wcdi->ovl_h = wc->image_h;
                    if (wcdi->ovl != NULL) {
                        SDL_FreeYUVOverlay(wcdi->ovl);
                    }
                    if ((wcdi->ovl = SDL_CreateYUVOverlay(wcdi->ovl_w, wcdi->ovl_h, SDL_YUY2_OVERLAY, surface)) == NULL) {
                        ERROR("SDL_CreateYUVOverlay failed\n");
                        exit(1);
                    }
                    DEBUG("created new overlay %dx%d\n", wcdi->ovl_w, wcdi->ovl_h);
                }

                // display the image
                SDL_LockYUVOverlay(wcdi->ovl);
                memcpy(wcdi->ovl->pixels[0], wc->image, wcdi->ovl_w * wcdi->ovl_h * 2);
                SDL_UnlockYUVOverlay(wcdi->ovl);
                SDL_DisplayYUVOverlay(wcdi->ovl, &wcdi->image_pos);
            }

            // display text
            if (wc->image_display == IMAGE_DISPLAY_TEXT) {
                // clear image and display strings   
                SDL_FillRect(surface, &wcdi->image_pos, color_black);
                SDL_UpdateRect(surface, wcdi->image_pos.x, wcdi->image_pos.y, wcdi->image_pos.w, wcdi->image_pos.h);
                RENDER_TEXT_CENTERED(fontl, wc->image_str1, wcdi->image_str1_pos);
                RENDER_TEXT_CENTERED(fontl, wc->image_str2, wcdi->image_str2_pos);
                RENDER_TEXT_CENTERED(fontl, wc->image_str3, wcdi->image_str3_pos);
            }
        }

        // relsease wc mutex
        pthread_mutex_unlock(&wc->image_mutex);
    }

    //
    // display control/status ...
    //

    //
    //     123456789 1
    //     -----------
    // 00: PLAYBACK  F
    // 01:
    // 02: 06/07/58
    // 03: 11:12:13
    // 04: 25:01:14 
    // 05: FWD   1X        ...  STOPPED  |  PAUSED  |  FWD 4x  |  REV 0.1X
    // 06:
    // 07: STOP  PAUSE     ...  STOP PLAY
    // 08: DIR   FWD
    // 09  SPEED .5
    // 10
    // 11: SEC   -  +
    // 12: MIN   -  +
    // 13: HOUR  -  +
    // 14: DAY   -  +
    // 15:
    // 16: A 33:23:14 
    // 17: B 33:01:23 
    // 18: C 42:01:23 
    // 19: D 42:01:23 
    // 20:
    // 21:
    // 22:
    // 23: RATE Mb/Sec     ... etc ...
    // 24: A 3.123
    // 25: B 3.123
    // 26: C 3.123
    // 27: D 3.123

    bool            mode_change;
    bool            time_sec_tick;
    time_t          time_sec_now;
    uint64_t        curr_time_us;

    static uint64_t mode_id_last;
    static time_t   time_sec_last;
    static uint64_t last_con_info_display_time_us;

    // if mode changed then clear ctl_pos
    mode_change = (ctl.mode.mode_id != mode_id_last);
    mode_id_last = ctl.mode.mode_id;
    if (mode_change) {
        SDL_FillRect(surface, &ctl.ctl_pos, color_black);
        SDL_UpdateRect(surface, ctl.ctl_pos.x, ctl.ctl_pos.y, ctl.ctl_pos.w, ctl.ctl_pos.h);
    }

    // determine if there has been a seconds time tick
    time_sec_now  = time(NULL);
    time_sec_tick = (time_sec_now != time_sec_last);
    time_sec_last = time_sec_now;

    // readthe microsec_timer
    curr_time_us = microsec_timer();

    // update mode
    if (display_all || mode_change) {
        RENDER_TEXT(fonts, MODE_STR(ctl.mode.mode), ctl.mode_pos, true);
    }

#ifdef FULLS_SUPPORT
    // update full screen control
    if (display_all || mode_change) {
        RENDER_TEXT(fonts, "F", ctl.fulls_pos, true);
    }
#endif

    // update date and time
    if (display_all || mode_change || time_sec_tick || (ctl.mode.mode == MODE_PLAYBACK && ctl.mode.pb_speed > 1)) {
        char date_and_time_str[MAX_TIME_STR];
        time_t secs;
        if (ctl.mode.mode == MODE_LIVE) {
            secs = time(NULL);
        } else {
            if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) {
                secs = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode) / 1000000;
            } else {
                secs = ctl.mode.pb_real_time_us / 1000000;
            }
        }
        time2str(date_and_time_str, secs, opt_zulu_time);
        if (opt_zulu_time) {
            strcpy(date_and_time_str+17, " Z");
        }
        date_and_time_str[8] = '\0';
        RENDER_TEXT(fonts, date_and_time_str, ctl.date_pos, false);
        RENDER_TEXT(fonts, date_and_time_str+9, ctl.time_pos, false);
    }

    // update playback time
    if ((ctl.mode.mode == MODE_PLAYBACK) &&
        (display_all || mode_change || time_sec_tick || ctl.mode.pb_speed > 1))
    {
        int64_t  interval_secs;
        uint32_t days, hours, minutes, seconds;
        char     playback_time_str[32];
        char   * sign_str;

        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) {
            interval_secs = (int64_t)(get_real_time_us() - PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode)) / 1000000;
        } else {
            interval_secs = (int64_t)(get_real_time_us() - ctl.mode.pb_real_time_us) / 1000000;
        }

        if (interval_secs >= 0) {
            sign_str = "";
        } else {
            interval_secs = -interval_secs;
            sign_str = "-";
        }

        CVT_INTERVAL_SECS_TO_DAY_HMS(interval_secs, days, hours, minutes, seconds);

        sprintf(playback_time_str, "%s%d:%02d:%02d", sign_str, 24*days+hours, minutes, seconds);

        RENDER_TEXT(fonts, playback_time_str, ctl.pb_time_pos, false);
    }

    // update playback state
    if ((ctl.mode.mode == MODE_PLAYBACK) &&
        (display_all || mode_change || event.pb_speed_input_in_prog))
    {
        char speed_str[32], state_str[32], play_pause_str[32], dir_str[32];

        // init state_str
        if (ctl.mode.pb_submode == PB_SUBMODE_STOP) {
            strcpy(state_str, "STOPPED");
        } else if (ctl.mode.pb_submode == PB_SUBMODE_PAUSE) {
            strcpy(state_str, "PAUSED");
        } else { // PLAY
            char spdstr[32];
            if ((int)ctl.mode.pb_speed == ctl.mode.pb_speed) {
                sprintf(spdstr, "%3d", (int)ctl.mode.pb_speed);
            } else {
                sprintf(spdstr, "%3.1f", ctl.mode.pb_speed);
            }
            sprintf(state_str, "%s %sX", PB_DIR_STR(ctl.mode.pb_dir), spdstr);
        }

        // init play_pause_str
        if (ctl.mode.pb_submode == PB_SUBMODE_STOP ||  
            ctl.mode.pb_submode == PB_SUBMODE_PAUSE) 
        {
            strcpy(play_pause_str, "PLAY");
        } else {
            strcpy(play_pause_str, "PAUSE");
        }

        // init dir_str
        strcpy(dir_str, PB_DIR_STR(ctl.mode.pb_dir));

        // init speed_str
        if (event.pb_speed_input_in_prog) {
            strcpy(speed_str, event.text_input_str);
            if ((curr_time_us % 1000000) < 500000) {
                strcat(speed_str, "_");
            } else {
                strcat(speed_str, " ");
            }
            int field_width    = ctl.pb_speed_value_pos.w / FONTS_CHAR_WIDTH;
            int len_speed_str = strlen(speed_str);
            if (len_speed_str > field_width) {
                memmove(speed_str, 
                        speed_str + (len_speed_str - field_width),
                        field_width + 1);
            }
        } else {
            if ((int)ctl.mode.pb_speed == ctl.mode.pb_speed) {
                sprintf(speed_str, "%-3d", (int)ctl.mode.pb_speed);
            } else {
                sprintf(speed_str, "%-3.1f", ctl.mode.pb_speed);
            }
        }

        // render text for playback state
        RENDER_TEXT(fonts, state_str,      ctl.pb_state_pos,       false);
        RENDER_TEXT(fonts, "STOP",         ctl.pb_stop_pos,        true);
        RENDER_TEXT(fonts, play_pause_str, ctl.pb_play_pause_pos,  true);
        RENDER_TEXT(fonts, "DIR",          ctl.pb_dir_label_pos,   false);
        RENDER_TEXT(fonts, dir_str,        ctl.pb_dir_value_pos,   true);
        RENDER_TEXT(fonts, "SPEED",        ctl.pb_speed_label_pos, false);
        RENDER_TEXT(fonts, speed_str,      ctl.pb_speed_value_pos, true);
    }

    // update playback time control
    if ((ctl.mode.mode == MODE_PLAYBACK) &&
        (display_all || mode_change)) 
    {
        RENDER_TEXT(fonts, "SEC",   ctl.pb_sec_pos,       false);
        RENDER_TEXT(fonts, "-",     ctl.pb_sec_minus_pos, true);
        RENDER_TEXT(fonts, "+",     ctl.pb_sec_plus_pos,  true);
        RENDER_TEXT(fonts, "MIN",   ctl.pb_min_pos,       false);
        RENDER_TEXT(fonts, "-",     ctl.pb_min_minus_pos, true);
        RENDER_TEXT(fonts, "+",     ctl.pb_min_plus_pos,  true);
        RENDER_TEXT(fonts, "HOUR",  ctl.pb_hour_pos,      false);
        RENDER_TEXT(fonts, "-",     ctl.pb_hour_minus_pos,true);
        RENDER_TEXT(fonts, "+",     ctl.pb_hour_plus_pos, true);
        RENDER_TEXT(fonts, "DAY",   ctl.pb_day_pos,       false);
        RENDER_TEXT(fonts, "-",     ctl.pb_day_minus_pos, true);
        RENDER_TEXT(fonts, "+",     ctl.pb_day_plus_pos,  true);
    }

    // update playback record duration
    if ((ctl.mode.mode == MODE_PLAYBACK) &&
        (display_all || mode_change || time_sec_tick)) 
    {
        for (i = 0; i < MAX_WEBCAM; i++) {
            char     record_dur_str[32];
            uint32_t days, hours, minutes, seconds;

            if (webcam[i].state == STATE_CONNECTED) {
                CVT_INTERVAL_SECS_TO_DAY_HMS(webcam[i].status.rp_duration_us/1000000, 
                                            days, hours, minutes, seconds);
                sprintf(record_dur_str, "%c %d:%02d:%02d", 'A'+i, 24*days+hours, minutes, seconds);
            } else {
                sprintf(record_dur_str, "%c", 'A'+i);
            }

            RENDER_TEXT(fonts, record_dur_str, ctl.pb_record_dur_pos[i], false);
        }
    }

    // update connection info
    // XXX add connect-time stat
    if (display_all || mode_change || curr_time_us - last_con_info_display_time_us > 2000000 || event.con_info_event) {
        int        i;
        char       str[32];
        static int con_info_select;

        if (event.con_info_event) {
            event.con_info_event = false;
            con_info_select = (con_info_select + 1) % 3;
        }

        switch (con_info_select) {
        case 0:
            RENDER_TEXT(fonts, "TOTAL MB", ctl.con_info_title_pos, true);
            for (i = 0; i < MAX_WEBCAM; i++) {
                sprintf(str, "%c %d.%3.3d", 
                        'A'+i, 
                        (int)(webcam[i].recvd_bytes/1000000), 
                        (int)((webcam[i].recvd_bytes%1000000)/1000));
                RENDER_TEXT(fonts, str, ctl.con_info_pos[i], false);
            }
            break;
        case 1: {
            uint64_t delta_us;
            double   mbit_per_sec;

            delta_us = (last_con_info_display_time_us != 0
                        ? (curr_time_us - last_con_info_display_time_us)
                        : 0);
            RENDER_TEXT(fonts, "RATE Mb/S", ctl.con_info_title_pos, true);
            for (i = 0; i < MAX_WEBCAM; i++) {
                mbit_per_sec = (delta_us != 0 
                                ? 8.0 * (webcam[i].recvd_bytes - webcamdi[i].recvd_bytes_last) / delta_us
                                : 0.0);
                sprintf(str, "%c %5.3f", 'A'+i, mbit_per_sec);
                RENDER_TEXT(fonts, str, ctl.con_info_pos[i], false);
            }
            break; }
        case 2:
            RENDER_TEXT(fonts, "P2P SND DUP", ctl.con_info_title_pos, true);
            for (i = 0; i < MAX_WEBCAM; i++) {
                sprintf(str, "%c %4d %4d",
                        'A'+i, 
                        webcam[i].status.p2p_resend_cnt,
                        webcam[i].status.p2p_recvdup_cnt);
                RENDER_TEXT(fonts, str, ctl.con_info_pos[i], false);
            }
            break;
        }

        for (i = 0; i < MAX_WEBCAM; i++) {
            webcamdi[i].recvd_bytes_last = webcam[i].recvd_bytes;
        }
        last_con_info_display_time_us = curr_time_us;
    }
}

// -----------------  WEBCAM THREAD  -------------------------------------

void * webcam_thread(void * cx) 
{
    int                 id                        = (int)(long)cx;
    char                id_char                   = 'A' + id;
    webcam_t          * wc                        = &webcam[id];
    struct wc_event_s * wcev                      = &event.wc[id];

    #define STATE_CHANGE(new_state, s1, s2, s3) \
        do { \
            NOTICE("wc %c: %s -> %s '%s' '%s' %s\n", \
                   id_char, STATE_STR(wc->state), STATE_STR(new_state), s1, s2, s3); \
            wc->state = (new_state); \
            wc->last_state_change_time_us = microsec_timer(); \
            DISPLAY_TEXT(s1,s2,s3); \
        } while (0)

    #define DISPLAY_IMAGE(_image, _motion) \
        do { \
            pthread_mutex_lock(&wc->image_mutex); \
            if (_motion) { \
                wc->image_highlight = true; \
                wc->last_highlight_enable_time_us = microsec_timer(); \
            } \
            if (wc->image) { \
                free(wc->image); \
            } \
            wc->image = (_image); \
            wc->image_w = width; \
            wc->image_h = height; \
            wc->image_display = IMAGE_DISPLAY_IMAGE;  \
            wc->image_update_request = true;  \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    #define DISPLAY_TEXT(s1,s2,s3); \
        do { \
            DEBUG("wc %c: DISPLAY_TEXT: %s - %s - %s\n", id_char, s1, s2, s3); \
            pthread_mutex_lock(&wc->image_mutex); \
            wc->image_highlight = false; \
            strcpy(wc->image_str1, s1); \
            strcpy(wc->image_str2, s2); \
            strcpy(wc->image_str3, s3); \
            wc->image_display = IMAGE_DISPLAY_TEXT; \
            wc->image_update_request = true; \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    #define DISPLAY_CLEAR_HIGHLIGHT() \
        do { \
            if (wc->image_highlight == false) { \
                break; \
            } \
            pthread_mutex_lock(&wc->image_mutex); \
            wc->image_highlight = false; \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    #define DISPLAY_WC_NAME(dn) \
        do { \
            pthread_mutex_lock(&wc->image_mutex); \
            strcpy(wc->image_wc_name, (dn)); \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    __sync_fetch_and_add(&webcam_threads_running_count,1);

    STATE_CHANGE(STATE_NO_WC_ID_STR, "NO SELECTION", "", "");

    while (true) {
        // wc_name event processing
        if (wcev->wc_name_event) {
            if (wc->handle != NO_HANDLE) {
                p2p_disconnect(wc->handle);
                wc->handle = NO_HANDLE;
            }
            DISPLAY_WC_NAME(wcev->wc_name);
            wcev->wc_name_event = false;
            if (wc->image_wc_name[0] != '\0') {
                STATE_CHANGE(STATE_CONNECTING, "CONNECTING", "", "");
            } else {
                STATE_CHANGE(STATE_NO_WC_ID_STR, "NO SELECTION", "", "");
            }
        }

        // if not in STATE_CONNECTED 
        // - clear resolution change event
        // - clear border highlight
        // endif
        if (wc->state != STATE_CONNECTED) {
            wcev->res_event = false;
            DISPLAY_CLEAR_HIGHLIGHT();  
        }

        // state processing
        switch (wc->state) {
        case STATE_NO_WC_ID_STR:
            usleep(SLEEP_US);
            break;

        case STATE_CONNECTING: {
            int h;

            // attempt to connect to wc_name
            DEBUG("wc %c: STATE_CONNECTING connected to %s\n", id_char, wc->wc_name);
            h = p2p_connect(user_name, password, wc->image_wc_name, SERVICE_WEBCAM);
            if (h < 0) {
                STATE_CHANGE(STATE_CONNECTING_ERROR, "CONNECT ERROR", "", "");
                break;
            }

            // connect succeeded; 
            // initialize connection variables
            wc->handle = h;
            wc->last_state_change_time_us = microsec_timer();
            wc->last_status_msg_recv_time_us = microsec_timer();
            wc->last_dead_text_display_time_us = microsec_timer();
            wc->last_highlight_enable_time_us = microsec_timer();
            wc->last_frame_status = STATUS_INFO_OK;
            wc->last_zoom = INVALID_ZOOM;
            bzero(&wc->mode, sizeof(struct mode_s));
            bzero(&wc->status, sizeof(struct status_s));
            STATE_CHANGE(STATE_CONNECTED, "CONNECTED", "", "");
            break; }

        case STATE_CONNECTED: {
            webcam_msg_t msg;
            int          ret, tmp_zoom;
            uint32_t     data_len, width, height;
            uint8_t      data[RP_MAX_FRAME_DATA_LEN];
            uint8_t    * image;
            uint64_t     curr_time_us;
            char         int_str[MAX_INT_STR];

            // if mode has changed then send message to webcam
            if (wc->mode.mode_id != ctl.mode.mode_id) {
                wc->mode = ctl.mode;

#ifdef DEBUG_PRINTS
                // issue debug for new mode
                if (wc->mode.mode != MODE_PLAYBACK) {
                    DEBUG("wc %c: mode is now: %s id=%"PRId64" \n", 
                           id_char, MODE_STR(wc->mode.mode), wc->mode.mode_id);
                } else {
                    char ts1[MAX_TIME_STR], ts2[MAX_TIME_STR];
                    DEBUG("wc %c: mode is now: %s id=%"PRId64" %s %s speed=%f mode_entry=%s play=%s\n",
                           id_char, MODE_STR(wc->mode.mode), wc->mode.mode_id,
                           PB_SUBMODE_STR(wc->mode.pb_submode),
                           PB_DIR_STR(wc->mode.pb_dir),
                           wc->mode.pb_speed,
                           time2str(ts1,wc->mode.pb_mode_entry_real_time_us/1000000,opt_zulu_time),
                           time2str(ts2,wc->mode.pb_real_time_us/1000000,opt_zulu_time));
                }
#endif

                // send the new mode message to webcam 
                bzero(&msg,sizeof(msg));
                msg.msg_type = MSG_TYPE_CMD_SET_MODE;   
                msg.u.mt_cmd_set_mode = wc->mode;
                if ((ret = p2p_send(wc->handle, &msg, sizeof(webcam_msg_t))) < 0) {
                    STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "send mode msg", "");
                    break;
                }

#if 0
                // display 'LOADING IMAGE' text
                DISPLAY_TEXT(status2str(STATUS_INFO_LOADING_IMAGE), "", "");
#endif
            }

            // if zoom has changed then send message to webcam
            tmp_zoom = zoom;
            if (tmp_zoom != wc->last_zoom) {
                uint64_t intvl_us = (tmp_zoom == id ? 0 : tmp_zoom == NO_ZOOM ? 150000 : 250000);

                DEBUG("wc %c: send MSG_TYPE_CMD_SET_MIN_SEND_INTVL_US intvl=%"PRId64" us\n", id_char, intvl_us);
                bzero(&msg,sizeof(msg));
                msg.msg_type = MSG_TYPE_CMD_SET_MIN_SEND_INTVL_US;   
                msg.u.mt_cmd_min_send_intvl.us = intvl_us;
                if ((ret = p2p_send(wc->handle, &msg, sizeof(webcam_msg_t))) < 0) {
                    STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "send intvl msg", "");
                    break;
                }
                wc->last_zoom = tmp_zoom;
            }

            // process resolution change event
            if (wcev->res_event) {
                if (wc->mode.mode == MODE_LIVE) {
                    bzero(&msg,sizeof(msg));
                    msg.msg_type = MSG_TYPE_CMD_LIVE_MODE_CHANGE_RES;
                    if ((ret = p2p_send(wc->handle, &msg, sizeof(webcam_msg_t))) < 0) {
                        STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "send res msg", "");
                        break;
                    }
                    DISPLAY_TEXT(status2str(STATUS_INFO_CHANGING_RESOLUTION), "", "");
                    usleep(LONG_SLEEP_US);
                }
                wcev->res_event = false;
            }

            // clear highlight if it is currently enabled and the last time it was
            // enabled is greater than HIGHLIGHT_TIME_US
            curr_time_us = microsec_timer();
            if ((wc->image_highlight) &&
                (curr_time_us - wc->last_highlight_enable_time_us > HIGHLIGHT_TIME_US) &&
                !(wc->mode.mode == MODE_PLAYBACK && wc->mode.pb_submode == PB_SUBMODE_PAUSE))
            {
                DISPLAY_CLEAR_HIGHLIGHT();
            }

            // if haven't received a status msg in 10 seconds then display error 
            if (curr_time_us - wc->last_status_msg_recv_time_us > 10000000) {
                if (curr_time_us - wc->last_dead_text_display_time_us > 1000000) {
                    DISPLAY_TEXT(status2str(STATUS_ERR_DEAD), "", "");
                    wc->last_dead_text_display_time_us = curr_time_us;
                }
            }

            // receive msg header  
            ret = p2p_recv(wc->handle, &msg, sizeof(msg), RECV_NOWAIT_ALL);
            if (ret != sizeof(msg)) {
                if (ret != RECV_WOULDBLOCK) {
                    STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "recv msg hdr", "");
                }
                usleep(SHORT_SLEEP_US);
                break;
            }
            wc->recvd_bytes += ret;

            // process the msg
            switch (msg.msg_type) {
            case MSG_TYPE_FRAME:
                // verify data_len
                data_len = msg.u.mt_frame.data_len;
                if (data_len > RP_MAX_FRAME_DATA_LEN) {
                    int2str(int_str, data_len);
                    STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "invld data len", int_str);
                    break;
                }

                // receive msg data  
                if (data_len > 0) {
                    if ((ret = p2p_recv(wc->handle, data, data_len, RECV_WAIT_ALL)) != data_len) {
                        STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "recv msg data", "");
                        break;
                    }
                    wc->recvd_bytes += ret;
                }

                // if mode_id in received msg does not match our mode_id then discard
                if (msg.u.mt_frame.mode_id != wc->mode.mode_id) {
                    WARNING("wc %c: discarding frame msg because msg mode_id %"PRId64" is not expected %"PRId64"\n",
                            id_char, msg.u.mt_frame.mode_id, wc->mode.mode_id);
                    break;
                }

                // don't display if the status for the mode we're in is not okay
                if ((wc->mode.mode == MODE_LIVE && wc->status.cam_status != STATUS_INFO_OK) ||
                    (wc->mode.mode == MODE_PLAYBACK && wc->status.rp_status != STATUS_INFO_OK))
                {
                    uint32_t status = (wc->mode.mode == MODE_LIVE ? wc->status.cam_status 
                                                                  : wc->status.rp_status);
                    WARNING("wc %c: discarding frame msg because %s status is %s\n",
                            id_char, MODE_STR(wc->mode.mode), STATUS_STR(status));
                    break;
                }

#ifdef DEBUG_RECEIVED_FRAME_TIME
                // sanity check the received frame time
                if (wc->mode.mode == MODE_PLAYBACK) {
                    uint64_t time_diff_us, exp_time_us;

                    exp_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&wc->mode);
                    time_diff_us = (exp_time_us > msg.u.mt_frame.real_time_us
                                    ? exp_time_us - msg.u.mt_frame.real_time_us
                                    : msg.u.mt_frame.real_time_us - exp_time_us);
                    if (time_diff_us > 1000000) {
                        ERROR("wc %c: playback mode received frame time diff %d.%3.3d\n",
                              id_char, (int32_t)(time_diff_us / 1000000), (int32_t)(time_diff_us % 1000000) / 1000);
                    }
                }
#endif

                // if data_len is 0 that means webcam is responding with no image;
                // such as for a playback time when nothing was recorded
                if (data_len == 0) {
                    wc->last_frame_status = msg.u.mt_frame.status;
                    DISPLAY_TEXT(status2str(wc->last_frame_status), "", "");
                    break;
                }

                // jpeg decode
                ret = jpeg_decode(id, JPEG_DECODE_MODE_YUY2,
                                  data, data_len,             // jpeg
                                  &image, &width, &height);   // pixels
                if (ret < 0) {
                    ERROR("wc %c: jpeg decode ret=%d\n", id_char, ret);
                    wc->last_frame_status = STATUS_ERR_JPEG_DECODE;
                    DISPLAY_TEXT(status2str(wc->last_frame_status), "", "");
                    break;
                }

                // display the image
                wc->last_frame_status = STATUS_INFO_OK;
                DISPLAY_IMAGE(image, msg.u.mt_frame.motion);
                break;

            case MSG_TYPE_STATUS:
                // save status and last time status recvd
                wc->status = msg.u.mt_status;
                wc->last_status_msg_recv_time_us = microsec_timer();

                // if error status for the mode we're in then display text 
                if (wc->mode.mode == MODE_LIVE && wc->status.cam_status != STATUS_INFO_OK) {
                    DISPLAY_TEXT(status2str(wc->status.cam_status), "", "");
                } else if (wc->mode.mode == MODE_PLAYBACK && wc->status.rp_status != STATUS_INFO_OK) {
                    DISPLAY_TEXT(status2str(wc->status.rp_status), "", "");
                }
                break;

            default:
                int2str(int_str, msg.msg_type);
                STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "invld msg type", int_str);
                break;
            }
            break; }

        case STATE_CONNECTING_ERROR:
        case STATE_CONNECTED_ERROR:
            if (wc->handle != NO_HANDLE) {
                p2p_disconnect(wc->handle);
                wc->handle = NO_HANDLE;
            }
            if (microsec_timer() - wc->last_state_change_time_us < RECONNECT_TIME_US) {
                usleep(SLEEP_US);
                break;
            }
            STATE_CHANGE(STATE_CONNECTING, "CONNECTING", "", "");
            break;

        case STATE_FATAL_ERROR:
            break;

        default: {
            char int_str[MAX_INT_STR];

            int2str(int_str, wc->state);
            STATE_CHANGE(STATE_FATAL_ERROR, "DISABLED", "invalid state", int_str);
            break; }
        }

        // if quit event of in fatal error state then exit this thread
        if (event.quit_event || wc->state == STATE_FATAL_ERROR) {
            break;
        }
    }

    // disconnect
    if (wc->handle != NO_HANDLE) {
        p2p_disconnect(wc->handle);
        wc->handle = NO_HANDLE;
    }

    // exit thread
    __sync_fetch_and_add(&webcam_threads_running_count,-1);
    return NULL;
}

// -----------------  DEBUG THREAD  -------------------------------------------------

void * debug_thread(void * cx)
{
    int    argc;
    char * argv[MAX_GETCL_ARGV];

    // enable this thread to be cancelled
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    // loop until eof on debug input or thread cancelled
    while (true) {
        // print prompt and read cmd/args
        printf("DEBUG> ");
        if (getcl(&argc,argv) == false) {
            break;
        }

        // if blank line then continue
        if (argc == 0) {
            continue;
        }

        // cmd: help
        if (strcmp(argv[0], "help") == 0) {
            printf("p2p_debug_con [<handle>]\n");
            printf("p2p_monitor_ctl <handle> <secs>\n");
            continue;
        }

        // cmd: p2p_debug_con 
        if (strcmp(argv[0], "p2p_debug_con") == 0) {
            int handle;

            if (argc == 1) {
                handle = -1;
            } else if (sscanf(argv[1], "%d", &handle) != 1) {
                printf("usage: p2p_debug_con [<handle>]\n");
                continue;
            }
            p2p_debug_con(handle);
            continue;
        }

        // cmd: p2p_monitor_ctl
        if (strcmp(argv[0], "p2p_monitor_ctl") == 0) {
            int handle, secs;

            if (argc != 3 || 
                sscanf(argv[1], "%d", &handle) != 1 ||
                sscanf(argv[2], "%d", &secs) != 1)
            {
                printf("usage: p2p_monitor_ctl <handle> <secs>\n");
                continue;
            }
            p2p_monitor_ctl(handle, secs);
            continue;
        }
    }

    // eof on debug input
    event.quit_event = true;
    return NULL;
}

bool getcl(int * argc, char ** argv)
{
    char * saveptr;
    char * token;
    char   b[100];

    *argc = 0;

    if (fgets(b, sizeof(b), stdin) == NULL) {
        return false;
    }

    while (true) {
        token = strtok_r(*argc==0?b:NULL, "   \n", &saveptr);
        if (token == NULL) {
            return true;
        }
        argv[*argc] = token;
        *argc = *argc + 1;
        if (*argc == MAX_GETCL_ARGV) {
            return true;
        }
    }
}
