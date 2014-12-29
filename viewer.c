#include "wc.h"
#include "button_sound.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_mixer.h>

// XXX use SET_CTL_MODE_PLAYBACK_TIME(delta_us)
// ZZZ review all struct fields
// ZZZ mode locking

//
// defines 
//

#define MAX_WEBCAM                  4
#define MAX_STR                     100  //ZZZ delete or search and pick more reasonable size

#define WIN_WIDTH_INITIAL           1280
#define WIN_HEIGHT_INITIAL          800
#define WIN_WIDTH_MIN               800
#define WIN_HEIGHT_MIN              500
#define CTL_COLS                    14
#define CTL_WIDTH                   (CTL_COLS * font[0].char_width)
#define CTLB_ROWS                   7

#define MS                          1000
#define HIGHLIGHT_TIME_US           (2000*MS)
#define RECONNECT_TIME_US           (10000*MS)

#define NO_WCNAME                   "<none>"
#define NO_USERNAME                 "<username>"
#define NO_PASSWORD                 "<password>"

#define CONFIG_USERNAME             (config[0].value)
#define CONFIG_PASSWORD             (config[1].value)
#define CONFIG_PROXY                (config[2].value[0])    // N, Y
#define CONFIG_LOCALTIME            (config[3].value[0])    // N, Y
#define CONFIG_WC_NAME(idx)         (config[4+(idx)].value)
#define CONFIG_ZOOM                 (config[8].value[0])    // A, B, C, D, N
#define CONFIG_DEBUG                (config[9].value[0])    // N, Y

#ifndef ANDROID 
#define SDL_FLAGS                   SDL_WINDOW_RESIZABLE
#else
#define SDL_FLAGS                   SDL_WINDOW_FULLSCREEN
#endif

#define MAX_FONT                    2
#define PANE_COLS(p,fid)            ((p)->w / font[fid].char_width)
#define PANE_ROWS(p,fid)            ((p)->h / font[fid].char_height)
#ifndef ANDROID
#define FONT_PATH                   "/usr/share/fonts/gnu-free/FreeMonoBold.ttf"
#else
#define FONT_PATH                   "/system/fonts/DroidSansMono.ttf"
#endif

#define MAX_MOUSE_EVENT                       256
#define MOUSE_EVENT_NONE                      0
#define MOUSE_EVENT_QUIT                      1
#define MOUSE_EVENT_MODE_SELECT               2
#define MOUSE_EVENT_CONFIG_SELECT             3
#define MOUSE_EVENT_STATUS_SELECT             4
#define MOUSE_EVENT_PLAYBACK_STOP             8
#define MOUSE_EVENT_PLAYBACK_PLAY             9
#define MOUSE_EVENT_PLAYBACK_PAUSE            10
#define MOUSE_EVENT_PLAYBACK_FORWARD          11
#define MOUSE_EVENT_PLAYBACK_REVERSE          12
#define MOUSE_EVENT_PLAYBACK_FASTER           13
#define MOUSE_EVENT_PLAYBACK_SLOWER           14
#define MOUSE_EVENT_CONFIG_PROXY              20
#define MOUSE_EVENT_CONFIG_TIME               21
#define MOUSE_EVENT_CONFIG_SERVER_CHECK       22
#define MOUSE_EVENT_CONFIG_USERNAME           23
#define MOUSE_EVENT_CONFIG_PASSWORD           24
#define MOUSE_EVENT_CONFIG_KEY_ASCII_FIRST    32     // ascii space
#define MOUSE_EVENT_CONFIG_KEY_ASCII_LAST     126    // ascii '~'
#define MOUSE_EVENT_CONFIG_KEY_SHIFT          127
#define MOUSE_EVENT_CONFIG_KEY_ESC            128
#define MOUSE_EVENT_CONFIG_KEY_BS             129
#define MOUSE_EVENT_CONFIG_KEY_ENTER          130
#define MOUSE_EVENT_WC_ZOOM                   140
#define MOUSE_EVENT_WC_NAME                   150    // 4 webcams
#define MOUSE_EVENT_WC_RES                    160    // 4 webcams
#define MOUSE_EVENT_WC_NAME_LIST              170    // 8 list events per wc, 32 total

#define STATE_NOT_CONNECTED                   0
#define STATE_CONNECTING                      1
#define STATE_CONNECTED                       2
#define STATE_CONNECTING_ERROR                3
#define STATE_CONNECTED_ERROR                 4
#define STATE_FATAL_ERROR                     5

#define STATE_STR(state) \
   ((state) == STATE_NOT_CONNECTED     ? "STATE_NOT_CONNECTED"     : \
    (state) == STATE_CONNECTING        ? "STATE_CONNECTING"        : \
    (state) == STATE_CONNECTED         ? "STATE_CONNECTED"         : \
    (state) == STATE_CONNECTING_ERROR  ? "STATE_CONNECTING_ERROR"  : \
    (state) == STATE_CONNECTED_ERROR   ? "STATE_CONNECTED_ERROR"   : \
    (state) == STATE_FATAL_ERROR       ? "STATE_FATAL_ERROR"         \
                                       : "????")

#define SERVER_CHECK_STATUS_NOT_RUN           0
#define SERVER_CHECK_STATUS_IN_PROGRESS       1
#define SERVER_CHECK_STATUS_OK                2
#define SERVER_CHECK_STATUS_NO_USERNAME       3
#define SERVER_CHECK_STATUS_NO_PASSWORD       4
#define SERVER_CHECK_STATUS_ACCESS_DENIED     5
#define SERVER_CHECK_STATUS_UNREACHABLE       6

#define SERVER_CHECK_STATUS_STR(x) \
    ((x) == SERVER_CHECK_STATUS_NOT_RUN        ? "NOT_RUN"       : \
     (x) == SERVER_CHECK_STATUS_IN_PROGRESS    ? "IN_PROGRESS"   : \
     (x) == SERVER_CHECK_STATUS_OK             ? "OK"            : \
     (x) == SERVER_CHECK_STATUS_NO_USERNAME    ? "NO_USERNAME"   : \
     (x) == SERVER_CHECK_STATUS_NO_PASSWORD    ? "NO_PASSWORD"   : \
     (x) == SERVER_CHECK_STATUS_ACCESS_DENIED  ? "ACCESS_DENIED" : \
     (x) == SERVER_CHECK_STATUS_UNREACHABLE    ? "UNREACHABLE"     \
                                               : "????")

#define SET_CTL_MODE_LIVE() \
    do { \
        mode.mode = MODE_LIVE; \
        mode.pb_submode = PB_SUBMODE_STOP; \
        mode.pb_mode_entry_real_time_us = 0; \
        mode.pb_real_time_us = 0; \
        mode.pb_dir = PB_DIR_FWD; \
        mode.pb_speed = 1; \
        mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_STOP(init) \
    do { \
        mode.mode = MODE_PLAYBACK; \
        if (mode.pb_submode == PB_SUBMODE_PLAY) { \
            mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&mode); \
            if (mode.pb_real_time_us > get_real_time_us()) { \
                mode.pb_real_time_us = get_real_time_us(); \
            } \
        } \
        mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        if (init) { \
            mode.pb_real_time_us = get_real_time_us(); \
            mode.pb_dir = PB_DIR_FWD; \
            mode.pb_speed = 1; \
        } \
        mode.pb_submode = PB_SUBMODE_STOP; \
        mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_PAUSE(init) \
    do { \
        mode.mode = MODE_PLAYBACK; \
        if (mode.pb_submode == PB_SUBMODE_PLAY) { \
            mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&mode); \
        } \
        mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        if (init) { \
            mode.pb_real_time_us = get_real_time_us() - 2000000; \
            mode.pb_dir = PB_DIR_FWD; \
            mode.pb_speed = 1; \
        } \
        mode.pb_submode = PB_SUBMODE_PAUSE; \
        mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_PLAY() \
    do { \
        mode.mode = MODE_PLAYBACK; \
        if (mode.pb_submode == PB_SUBMODE_PLAY) { \
            mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&mode); \
        } \
        mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        mode.pb_submode = PB_SUBMODE_PLAY; \
        mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_TIME(delta_us) \
    do { \
        mode.mode = MODE_PLAYBACK; \
        if (mode.pb_submode == PB_SUBMODE_PLAY) { \
            mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&mode); \
        } \
        mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        mode.pb_real_time_us += (delta_us);   \
        if (mode.pb_submode == PB_SUBMODE_STOP) { \
            mode.pb_submode = PB_SUBMODE_PAUSE; \
        } \
        mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_DIR(dir) \
    do { \
        mode.mode = MODE_PLAYBACK; \
        if (mode.pb_submode == PB_SUBMODE_PLAY) { \
            mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&mode); \
        } \
        mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        mode.pb_dir = (dir); \
        mode.mode_id++; \
    } while (0)

#define SET_CTL_MODE_PLAYBACK_SPEED(speed)  \
    do { \
        mode.mode = MODE_PLAYBACK; \
        if (mode.pb_submode == PB_SUBMODE_PLAY) { \
            mode.pb_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&mode); \
        } \
        mode.pb_mode_entry_real_time_us = get_real_time_us(); \
        mode.pb_speed = (speed); \
        mode.mode_id++; \
    } while (0)

#define PLAY_BUTTON_SOUND() \
    do { \
        Mix_PlayChannel(-1, button_sound, 0); \
    } while (0)

#define CONFIG_WRITE() \
    do { \
        config_write(config_path, config, config_version); \
    } while (0)

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

//
// typedefs
//

typedef struct {//ZZZ comment fields sections
    uint32_t        state;
    struct mode_s   mode;
    struct status_s status; 
    int             p2p_id;
    uint64_t        recvd_bytes;
    uint64_t        recvd_frames;
    uint32_t        frame_status;

    SDL_Texture   * texture;
    int             texture_w;
    int             texture_h;

    bool            change_resolution_request;
    int             change_name_request;
    bool            name_select_mode;

    pthread_mutex_t image_mutex;
    uint64_t        image_change;
    char            image_name[MAX_STR];
    char            image_res[MAX_STR];
    bool            image_display;
    bool            image_highlight;
    uint8_t       * image;
    int             image_w;
    int             image_h;
    char            image_notification_str1[MAX_STR];
    char            image_notification_str2[MAX_STR];
    char            image_notification_str3[MAX_STR];
} webcam_t;

typedef struct {
    int      mouse_event;
    SDL_Rect mouse_event_pos[MAX_MOUSE_EVENT];

    bool     window_resize_event;
    int      window_resize_width;
    int      window_resize_height;

    bool     quit_event;
} event_t;

typedef struct {
    TTF_Font * font; 
    int        char_width;
    int        char_height;
} font_t;

//
// variables
//

struct mode_s    mode;

int              server_check_status;
char             webcam_names[MAX_USER_WC+1][MAX_WC_NAME+1];
int              max_webcam_names;

int              webcam_threads_running_count;

SDL_Window     * window;
SDL_Renderer   * renderer;
int              win_width;
int              win_height;

font_t           font[MAX_FONT];

Mix_Chunk      * button_sound;

webcam_t         webcam[MAX_WEBCAM];

event_t          event;

char             config_path[MAX_STR];
const int        config_version = 1;
config_t         config[] = { { "username",  NO_USERNAME },
                              { "password",  NO_PASSWORD },
                              { "proxy",     "N"         },
                              { "localtime", "Y"         },
                              { "wc_name_A", NO_WCNAME   },
                              { "wc_name_B", NO_WCNAME   },
                              { "wc_name_C", NO_WCNAME   },
                              { "wc_name_D", NO_WCNAME   },
                              { "zoom",      "A",        },
                              { "debug",     "N"         },
                              { "",          ""          } };

//
// prototypes
//

void server_check(void);
void * server_check_thread(void * cx);
void display_handler(void);
void render_text(SDL_Rect * pane, int row, int col, char * str, int mouse_event);
void render_text_ex(SDL_Rect * pane, int row, int col, char * str, int mouse_event, int field_cols, bool center, int font_id);
void * webcam_thread(void * cx);
#ifndef ANDROID
void * debug_thread(void * cx);
#endif

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit  rl;
    int            ret, i, count;
    const char   * config_dir;

    // set resource limti to allow core dumps
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    ret = setrlimit(RLIMIT_CORE, &rl);
    if (ret < 0) {
        WARN("setrlimit for core dump, %s\n", strerror(errno));
    }

    // init real time clock
    real_time_init();

    // read viewer config
#ifndef ANDROID
    config_dir = getenv("HOME");
    if (config_dir == NULL) {
        FATAL("env var HOME not set\n");
    }
#else
    config_dir = SDL_AndroidGetInternalStoragePath();
    if (config_dir == NULL) {
        FATAL("android internal storage path not set\n");
    }
#endif
    sprintf(config_path, "%s/.viewer_config", config_dir);
    if (config_read(config_path, config, config_version) < 0) {
        FATAL("config_read failed for %s\n", config_path);
    }

    // call server_check to verify login to cloud server, and 
    // get the list of webcams owned by this user
    server_check();

    // initialize to live mode
    SET_CTL_MODE_LIVE();

    // initialize Simple DirectMedia Layer  (SDL)
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) < 0) {
        FATAL("SDL_Init failed\n");
    }

    // create SDL Window and Renderer
    if (SDL_CreateWindowAndRenderer(WIN_WIDTH_INITIAL, WIN_HEIGHT_INITIAL, 
                                    SDL_FLAGS, &window, &renderer) != 0) 
    {
        FATAL("SDL_CreateWindowAndRenderer failed\n");
    }
    SDL_GetWindowSize(window, &win_width, &win_height);

    // init button_sound
    if (Mix_OpenAudio( 22050, MIX_DEFAULT_FORMAT, 2, 4096) < 0) {
        FATAL("Mix_OpenAudio failed\n");
    }
    button_sound = Mix_QuickLoad_WAV(button_sound_wav);
    Mix_VolumeChunk(button_sound,MIX_MAX_VOLUME/4);
    if (button_sound == NULL) {
        FATAL("Mix_QuickLoadWAV failed\n");
    }

    // initialize True Type Font
    if (TTF_Init() < 0) {
        FATAL("TTF_Init failed\n");
    }
    font[0].font = TTF_OpenFont(FONT_PATH, 20);
    if (font[0].font == NULL) {
        FATAL("failed TTF_OpenFont %s\n", FONT_PATH);
    }
    TTF_SizeText(font[0].font, "X", &font[0].char_width, &font[0].char_height);
    font[1].font = TTF_OpenFont(FONT_PATH, 32);
    if (font[1].font == NULL) {
        FATAL("failed TTF_OpenFont %s\n", FONT_PATH);
    }
    TTF_SizeText(font[1].font, "X", &font[1].char_width, &font[1].char_height);

#ifndef ANDROID
    // if debug mode enabled then create debug_thread
    pthread_t debug_thread_id = 0;
    if (CONFIG_DEBUG == 'Y') {
        pthread_create(&debug_thread_id, NULL, debug_thread, NULL);
    }
#endif

    // create webcam threads 
    for (i = 0; i < MAX_WEBCAM; i++) {
        pthread_t webcam_thread_id;
        pthread_create(&webcam_thread_id, NULL, webcam_thread, (void*)(long)i);
    }

    // wait for up to 3 second for the webcam threads to initialize
    count = 0;
    while (webcam_threads_running_count != MAX_WEBCAM && count++ < 3000/10) {
        usleep(10*MS);
    }
    if (webcam_threads_running_count != MAX_WEBCAM) {
        FATAL("webcam threads failed to start\n");
    }

    // loop: processing events and updating display
    while (!event.quit_event) {
        display_handler();
        usleep(10*MS);
    }

    // wait for up to 3 second for the webcam threads to terminate
    count = 0;
    while (webcam_threads_running_count != 0 && count++ < 3000/10) {
        usleep(10*MS);
    }
    if (webcam_threads_running_count != 0) {
        WARN("webcam threads failed to terminate\n");
    }

#ifndef ANDROID
    // cancel the debug thread
    if (debug_thread_id) {
        pthread_cancel(debug_thread_id);
        pthread_join(debug_thread_id, NULL);
    }
#endif

    // cleanup
    Mix_FreeChunk(button_sound);
    Mix_CloseAudio();
    TTF_Quit();
    SDL_Quit();

    // return success
    INFO("program terminating\n");
    return 0;
}

// -----------------  SERVER_CHECK  --------------------------------------

void server_check(void)
{
    pthread_t thread_id;

    // if already in progress then return
    if (server_check_status == SERVER_CHECK_STATUS_IN_PROGRESS) {
        return;
    }

    // init server_check_status to in-progress'
    server_check_status = SERVER_CHECK_STATUS_IN_PROGRESS;

    // init list with just the NO_WCNAME entry
    max_webcam_names = 0;
    strcpy(webcam_names[max_webcam_names++], NO_WCNAME);

    // if username or password has not yet been confgured then return
    if (strcmp(CONFIG_USERNAME, NO_USERNAME) == 0) {
        server_check_status = SERVER_CHECK_STATUS_NO_USERNAME;
        return;
    }
    if (strcmp(CONFIG_PASSWORD, NO_PASSWORD) == 0) {
        server_check_status = SERVER_CHECK_STATUS_NO_PASSWORD;
        return;
    }

    // create thread to complete the server check asynchronously
    pthread_create(&thread_id, NULL, server_check_thread, NULL);
}

void * server_check_thread(void * cx)
{
    int    sfd;
    FILE * fp;
    char   s[MAX_STR];
    bool   access_denied;

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // short delay to allow time for the SERVER_CHECK_STATUS_IN_PROGRESS to be seen on display
    usleep(500*MS);

    // login to cloud server
    sfd = connect_to_cloud_server(CONFIG_USERNAME, CONFIG_PASSWORD, "command", &access_denied);
    if (sfd < 0) {
        server_check_status = (access_denied ? SERVER_CHECK_STATUS_ACCESS_DENIED : SERVER_CHECK_STATUS_UNREACHABLE);
        return NULL;
    }

    // issue the 'ls' command, and read the list of my webcams
    fp = fdopen(sfd, "w+");   // mode: read/write
    if (fp == NULL) {
        FATAL("fdopen errno=%d\n", errno);
    }
    fputs("ls\n", fp);
    while (fgets(s, sizeof(s), fp) != NULL) {
        if (max_webcam_names == MAX_USER_WC+1) {
            break;
        }
        strcpy(webcam_names[max_webcam_names++], strtok(s, " "));
    }
    fclose(fp);

    // return success
    server_check_status = SERVER_CHECK_STATUS_OK;
    return 0;
}

// -----------------  DISPLAY HANDLER  -----------------------------------

void display_handler(void)
{
    #define CONFIG_KEYBD_MODE_INACTIVE 0
    #define CONFIG_KEYBD_MODE_USERNAME 1
    #define CONFIG_KEYBD_MODE_PASSWORD 2

    #define INIT_POS(r,_x,_y,_w,_h) \
        do { \
            (r).x = (_x); \
            (r).y = (_y); \
            (r).w = (_w); \
            (r).h = (_h); \
        } while (0)

    #define MOUSE_AT_POS(pos) ((ev.button.x >= (pos).x - 5) && \
                               (ev.button.x < (pos).x + (pos).w + 5) && \
                               (ev.button.y >= (pos).y - 5) && \
                               (ev.button.y < (pos).y + (pos).h + 5))

    int         i, j, len;
    char        str[MAX_STR];
    char        date_and_time_str[MAX_TIME_STR];
    SDL_Event   ev;
    bool        event_handled;
    uint64_t    curr_us;

    SDL_Rect    ctlpane;
    SDL_Rect    ctlbpane;
    SDL_Rect    keybdpane;
    SDL_Rect    wcpane[MAX_WEBCAM];
    SDL_Rect    wctitlepane[MAX_WEBCAM];
    SDL_Rect    wcimagepane[MAX_WEBCAM];

    static int  config_keybd_mode;
    static int  config_mode;
    static char config_keybd_str[MAX_STR];
    static bool config_keybd_shift;
    static int  status_select;

    // ----------------------------------------
    // ---- check if an event has occurred ----
    // ----------------------------------------

    while (true) {
        // get the next event, break out of loop if no event
        if (SDL_PollEvent(&ev) == 0) {
            break;
        }

        // process the SDL event, this code sets one or more of
        // the following event indicators
        // - event.mouse_event
        // - event.quit_event
        // - event.window_resize_event
        switch (ev.type) {
        case SDL_MOUSEBUTTONDOWN: {
            int i;
            DEBUG("MOUSE DOWN which=%d button=%s state=%s x=%d y=%d\n",
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

            for (i = 0; i < MAX_MOUSE_EVENT; i++) {
                if (MOUSE_AT_POS(event.mouse_event_pos[i])) {
                    event.mouse_event = i;
                    PLAY_BUTTON_SOUND();
                    break;
                }
            }
            break; }

        case SDL_KEYDOWN: {
            int  key = ev.key.keysym.sym;
            bool shift = (ev.key.keysym.mod & KMOD_SHIFT) != 0;

            if (config_keybd_mode == CONFIG_KEYBD_MODE_INACTIVE) {
                break;
            }

            if (key == 27) {
                event.mouse_event = MOUSE_EVENT_CONFIG_KEY_ESC;
            } else if (key == 8) {
                event.mouse_event = MOUSE_EVENT_CONFIG_KEY_BS;
            } else if (key == 13) {
                event.mouse_event = MOUSE_EVENT_CONFIG_KEY_ENTER;
            } else if (!shift && ((key >= 'a' && key <= 'z') || (key >= '0' && key <= '9'))) {
                event.mouse_event = key;
            } else if (shift && (key >= 'a' && key <= 'z')) {
                event.mouse_event = 'A' + (key - 'a');
            } else if (shift && key == '-') {
                event.mouse_event = '_';
            } else {
                break;
            }
            PLAY_BUTTON_SOUND();
            break; }

       case SDL_WINDOWEVENT: {
            switch (ev.window.event)  {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                event.window_resize_width = ev.window.data1;
                event.window_resize_height = ev.window.data2;
                event.window_resize_event = true;
                PLAY_BUTTON_SOUND();
                break;
            }
            break; }

        case SDL_QUIT: {
            event.quit_event = true;
            PLAY_BUTTON_SOUND();
            break; }
        }

        // break if mouse_event or quit_event is set
        if (event.mouse_event != MOUSE_EVENT_NONE || event.quit_event) {
            break; 
        }
    }

    // -----------------------------------------------
    // ---- handle events found by the above code ----
    // -----------------------------------------------

    // start with event_handled flag clear
    event_handled = false;

    // quit event
    if (event.quit_event) {
        return;
    }

    // window resize event
    if (event.window_resize_event) {
        win_width = event.window_resize_width;
        win_height = event.window_resize_height;
        if (win_width < WIN_WIDTH_MIN || win_height < WIN_HEIGHT_MIN) {
            if (win_width < WIN_WIDTH_MIN) {
                win_width = WIN_WIDTH_MIN;
            }
            if (win_height < WIN_HEIGHT_MIN) {
                win_height = WIN_HEIGHT_MIN;
            }
            usleep(1000*MS);  // XXX why is this delay needed?
            SDL_SetWindowSize(window, win_width, win_height);
        }
        event.window_resize_event = false;
        event_handled = true;
    }

    // mouse events
    if (event.mouse_event != MOUSE_EVENT_NONE) {
        if (event.mouse_event == MOUSE_EVENT_QUIT) {
            event.quit_event = true;
            return;

        } else if (event.mouse_event == MOUSE_EVENT_MODE_SELECT) {
            if (mode.mode == MODE_PLAYBACK) {
                SET_CTL_MODE_LIVE();
            } else {
                SET_CTL_MODE_PLAYBACK_PAUSE(true);
            }

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_SELECT) {
            config_mode         = !config_mode;
            config_keybd_mode   = CONFIG_KEYBD_MODE_INACTIVE;
            config_keybd_str[0] = '\0';
            config_keybd_shift  = false;

        } else if (event.mouse_event == MOUSE_EVENT_STATUS_SELECT) {
            status_select = (status_select + 1) % 5;

        } else if (event.mouse_event == MOUSE_EVENT_PLAYBACK_STOP) {
            SET_CTL_MODE_PLAYBACK_STOP(false);

        } else if (event.mouse_event == MOUSE_EVENT_PLAYBACK_PLAY) {
            SET_CTL_MODE_PLAYBACK_PLAY();

        } else if (event.mouse_event == MOUSE_EVENT_PLAYBACK_PAUSE) {
            SET_CTL_MODE_PLAYBACK_PAUSE(false);

        } else if (event.mouse_event == MOUSE_EVENT_PLAYBACK_FORWARD) {
            SET_CTL_MODE_PLAYBACK_DIR(PB_DIR_FWD);

        } else if (event.mouse_event == MOUSE_EVENT_PLAYBACK_REVERSE) {
            SET_CTL_MODE_PLAYBACK_DIR(PB_DIR_REV);

        } else if (event.mouse_event == MOUSE_EVENT_PLAYBACK_FASTER) {
            double speed = mode.pb_speed;
            if (speed < 1000) {
                speed *= 2;
            }
            if (speed > 0.9 && speed < 1.1) {
                speed = 1.0;
            }
            SET_CTL_MODE_PLAYBACK_SPEED(speed);

        } else if (event.mouse_event == MOUSE_EVENT_PLAYBACK_SLOWER) {
            double speed = mode.pb_speed;
            if (speed > 0.20) {
                speed /= 2;
            }
            if (speed > 0.9 && speed < 1.1) {
                speed = 1.0;
            }
            SET_CTL_MODE_PLAYBACK_SPEED(speed);

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_PROXY) {
            CONFIG_PROXY = (CONFIG_PROXY == 'N' ? 'Y' : 'N');
            CONFIG_WRITE();

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_TIME) {
            CONFIG_LOCALTIME = (CONFIG_LOCALTIME == 'N' ? 'Y' : 'N');
            CONFIG_WRITE();

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_SERVER_CHECK) {
            server_check();

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_USERNAME) {
            config_keybd_mode   = CONFIG_KEYBD_MODE_USERNAME;
            config_keybd_str[0] = '\0';
            config_keybd_shift  = false;

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_PASSWORD) {
            config_keybd_mode   = CONFIG_KEYBD_MODE_PASSWORD;
            config_keybd_str[0] = '\0';
            config_keybd_shift  = false;

        } else if (event.mouse_event >= MOUSE_EVENT_CONFIG_KEY_ASCII_FIRST && 
                   event.mouse_event <= MOUSE_EVENT_CONFIG_KEY_ASCII_LAST) {
            len = strlen(config_keybd_str);
            if (len < MAX_STR-1) {
                config_keybd_str[len] = event.mouse_event;
                config_keybd_str[len+1] = '\0';
            }

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_KEY_SHIFT) {
            config_keybd_shift = !config_keybd_shift;

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_KEY_ESC) {
            config_keybd_mode   = CONFIG_KEYBD_MODE_INACTIVE;
            config_keybd_str[0] = '\0';
            config_keybd_shift  = false;

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_KEY_BS) {
            if ((config_keybd_mode != CONFIG_KEYBD_MODE_INACTIVE) && 
                ((len = strlen(config_keybd_str)) > 0)) 
            {
                config_keybd_str[len-1] = '\0';
            }

        } else if (event.mouse_event == MOUSE_EVENT_CONFIG_KEY_ENTER) {
            if (config_keybd_mode == CONFIG_KEYBD_MODE_USERNAME) {
                len = strlen(config_keybd_str);
                if (len >= MIN_USER_NAME && len <= MAX_USER_NAME) {
                    strcpy(CONFIG_USERNAME, config_keybd_str);
                } else {
                    strcpy(CONFIG_USERNAME, NO_USERNAME);
                }
                strcpy(CONFIG_PASSWORD, NO_PASSWORD);
                CONFIG_WRITE();
                server_check();
                for (i = 0; i < MAX_WEBCAM; i++) {
                    webcam[i].change_name_request = 0;  // NO_WCNAME
                }
            } else if (config_keybd_mode == CONFIG_KEYBD_MODE_PASSWORD) {
                len = strlen(config_keybd_str);
                if (len >= MIN_PASSWORD && len <= MAX_PASSWORD) {
                    strcpy(CONFIG_PASSWORD, config_keybd_str);
                } else {
                    strcpy(CONFIG_PASSWORD, NO_PASSWORD);
                }
                CONFIG_WRITE();
                server_check();
                for (i = 0; i < MAX_WEBCAM; i++) {
                    webcam[i].change_name_request = 0;  // NO_WCNAME
                }
            }
            config_keybd_mode   = CONFIG_KEYBD_MODE_INACTIVE;
            config_keybd_str[0] = '\0';
            config_keybd_shift  = false;

        } else if (event.mouse_event >= MOUSE_EVENT_WC_ZOOM && 
                   event.mouse_event < MOUSE_EVENT_WC_ZOOM + MAX_WEBCAM) {
            int idx = event.mouse_event - MOUSE_EVENT_WC_ZOOM;
            CONFIG_ZOOM = (CONFIG_ZOOM == ('A'+idx) ? 'N' : ('A'+idx));
            CONFIG_WRITE();

        } else if (event.mouse_event >= MOUSE_EVENT_WC_NAME && 
                   event.mouse_event < MOUSE_EVENT_WC_NAME+4) {
            int idx = event.mouse_event - MOUSE_EVENT_WC_NAME;
            webcam[idx].name_select_mode = !webcam[idx].name_select_mode;

        } else if (event.mouse_event >= MOUSE_EVENT_WC_RES && 
                   event.mouse_event < MOUSE_EVENT_WC_RES+4) {
            int idx = event.mouse_event - MOUSE_EVENT_WC_RES;
            webcam[idx].change_resolution_request = true;

        } else if (event.mouse_event >= MOUSE_EVENT_WC_NAME_LIST && 
                   event.mouse_event < MOUSE_EVENT_WC_NAME_LIST+32) {
            int idx = (event.mouse_event - MOUSE_EVENT_WC_NAME_LIST) / 8;
            int name_idx = (event.mouse_event - MOUSE_EVENT_WC_NAME_LIST) % 8;
            webcam[idx].change_name_request = name_idx;
            webcam[idx].name_select_mode = false;

        } else {
            ERROR("invalid mouse_event %d\n", event.mouse_event);
        }

        event.mouse_event = MOUSE_EVENT_NONE;
        event_handled = true;
    }

    // -------------------------------------------------------------------------------------
    // ---- if playback mode and all connected webcam are eod or bod then stop playback ----
    // -------------------------------------------------------------------------------------

    if (mode.mode == MODE_PLAYBACK && mode.pb_submode == PB_SUBMODE_PLAY && mode.pb_dir == PB_DIR_FWD) {
        bool all_eod = true;
        for (i = 0; i < MAX_WEBCAM; i++) {
            webcam_t * wc = &webcam[i];
            if (wc->state == STATE_CONNECTED && wc->frame_status != STATUS_ERR_FRAME_AFTER_EOD) {
                all_eod = false;
            }
        }
        if (all_eod) {
            SET_CTL_MODE_PLAYBACK_STOP(false);
        }
        event_handled = true;
    }

    if (mode.mode == MODE_PLAYBACK && mode.pb_submode == PB_SUBMODE_PLAY && mode.pb_dir == PB_DIR_REV) {
        bool all_bod = true;
        for (i = 0; i < MAX_WEBCAM; i++) {
            webcam_t * wc = &webcam[i];
            if (wc->state == STATE_CONNECTED && wc->frame_status != STATUS_ERR_FRAME_BEFORE_BOD) {
                all_bod = false;
            }
        }
        if (all_bod) {
            SET_CTL_MODE_PLAYBACK_STOP(false);
        }
        event_handled = true;
    }

    // ------------------------------------------------
    // ---- check if display needs to be rendered, ----
    // ---- if not then return                     ----
    // ------------------------------------------------

    // create the data_and_tims_str
    time_t secs;
    if (mode.mode == MODE_LIVE) {
        secs = get_real_time_us() / 1000000;
        time2str(date_and_time_str, secs, CONFIG_LOCALTIME=='N');
    } else if (mode.mode == MODE_PLAYBACK) {
        if (mode.pb_submode == PB_SUBMODE_PLAY) {
            secs = PB_SUBMODE_PLAY_REAL_TIME_US(&mode) / 1000000;
        } else {
            secs = mode.pb_real_time_us / 1000000;
        }
        time2str(date_and_time_str, secs, CONFIG_LOCALTIME=='N');
    } else {
        bzero(date_and_time_str, sizeof(date_and_time_str));
    }

    // the following conditions require display update
    // - event was handled
    // - an image had changed
    // - date_and_time_str has changed and last update > 100 ms ago
    // - keyboard input is in progress and last update > 100 ms ago
    // - server_check is in progress and last update > 100 ms ago
    // - last update > 1 sec ago
    // if none of these conditions exist then return
    static char     last_date_and_time_str[MAX_TIME_STR];
    static uint64_t last_image_change[MAX_WEBCAM];
    static uint64_t last_window_update_us;
    curr_us = microsec_timer();
    do {
        if (event_handled) {
            break;
        }

        for (i = 0; i < MAX_WEBCAM; i++) {
            if (webcam[i].image_change != last_image_change[i]) {
                break;
            }
        }
        if (i < MAX_WEBCAM) {
            break;
        }

        if (strcmp(date_and_time_str, last_date_and_time_str) != 0 &&
            curr_us - last_window_update_us > 100*MS)
        {
            break;
        }

        if (config_keybd_mode != CONFIG_KEYBD_MODE_INACTIVE &&
            curr_us - last_window_update_us > 100*MS) 
        {
            break;
        }

        if (server_check_status == SERVER_CHECK_STATUS_IN_PROGRESS &&
            curr_us - last_window_update_us > 100*MS) 
        {
            break;
        }

        if (curr_us - last_window_update_us > 1000*MS) {
            break;
        }

        return;
    } while (0);
    for (i = 0; i < MAX_WEBCAM; i++) {
        last_image_change[i] = webcam[i].image_change;
    }
    strcpy(last_date_and_time_str, date_and_time_str);
    last_window_update_us = curr_us;

    // --------------------------------------------
    // ---- reinit the list of positions       ----
    // --------------------------------------------

    bzero(event.mouse_event_pos, sizeof(event.mouse_event_pos));

    INIT_POS(ctlpane, 
             win_width-CTL_WIDTH, 0,     // x, y
             CTL_WIDTH, win_height);     // w, h
    INIT_POS(ctlbpane, 
             win_width-CTL_WIDTH, win_height-CTLB_ROWS*font[0].char_height,
             CTL_WIDTH, CTLB_ROWS*font[0].char_height);
    INIT_POS(keybdpane,
             0, 0,
             win_width-CTL_WIDTH, win_height);

    int small_win_count = 0;
    for (i = 0; i < MAX_WEBCAM; i++) {
        int wc_x, wc_y, wc_w, wc_h;

        if (CONFIG_ZOOM >= 'A' && CONFIG_ZOOM <= 'D') {
            int wc_zw = (double)(win_width - CTL_WIDTH) / 1.33;
            if ('A'+i == CONFIG_ZOOM) {
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
        } else {
            wc_w = (win_width - CTL_WIDTH) / 2;
            wc_h = win_height / 2;
            switch (i) {
            case 0: wc_x = 0;    wc_y = 0;    break;
            case 1: wc_x = wc_w; wc_y = 0;    break;
            case 2: wc_x = 0;    wc_y = wc_h; break;
            case 3: wc_x = wc_w; wc_y = wc_h; break;
            }
        }
        INIT_POS(wcpane[i], 
                 wc_x, wc_y, 
                 wc_w, wc_h);
        INIT_POS(wctitlepane[i],
                 wc_x + 1, wc_y + 1,
                 wc_w - 2, font[0].char_height);
        INIT_POS(wcimagepane[i],
                 wc_x + 1, wc_y + 2 + font[0].char_height,
                 wc_w - 2, wc_h - font[0].char_height - 3);
    }

    // ---------------------------------
    // ---- clear the entire window ----
    // ---------------------------------

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer);

    // ------------------------------
    // ---- config mode: ctlpane ----
    // ------------------------------

    if (config_mode) {
        // Ctl Pane ...
        //
        // CONFIGURE
        // 
        // -- SERVER --
        // <username>
        // 
        // <password>
        // 
        // SERVER_CHECK
        // OK: WC_CNT=7  
        // 
        // -- NETWORK --
        // PROXY_DISABLED
        // 
        // -- TIME --
        // LOCALTIME
        // OFF=5000 MS

        // title line
        render_text(&ctlpane, 0, 0, "CONFIGURE", MOUSE_EVENT_NONE);

        // server section
        render_text(&ctlpane, 2, 0, "-- SERVER --", MOUSE_EVENT_NONE);

        if (config_keybd_mode == CONFIG_KEYBD_MODE_USERNAME) {
            bool cursor_blink_on = ((curr_us % 1000000) > 500000);
            len = strlen(config_keybd_str);
            strcpy(str, len < CTL_COLS ? config_keybd_str : config_keybd_str + (len-CTL_COLS+1));
            if (cursor_blink_on) {
                strcat(str, "_");
            }
        } else {
            strcpy(str, CONFIG_USERNAME);
        }
        len = strlen(str);
        render_text(&ctlpane, 3, 0, str, MOUSE_EVENT_CONFIG_USERNAME);

        if (config_keybd_mode == CONFIG_KEYBD_MODE_PASSWORD) {
            bool cursor_blink_on = ((curr_us % 1000000) > 500000);
            len = strlen(config_keybd_str);
            strcpy(str, len < CTL_COLS ? config_keybd_str : config_keybd_str + (len-CTL_COLS+1));
            if (cursor_blink_on) {
                strcat(str, "_");
            }
        } else if (strcmp(CONFIG_PASSWORD, NO_PASSWORD) == 0) {
            strcpy(str, NO_PASSWORD);
        } else {
            strcpy(str, "********");
        }
        render_text(&ctlpane, 5, 0, str, MOUSE_EVENT_CONFIG_PASSWORD);

        render_text(&ctlpane, 7, 0, "SERVER_CHECK", MOUSE_EVENT_CONFIG_SERVER_CHECK);
        render_text(&ctlpane, 8, 0, SERVER_CHECK_STATUS_STR(server_check_status), MOUSE_EVENT_NONE);
        if (server_check_status == SERVER_CHECK_STATUS_OK) {
            sprintf(str, ": WC_CNT=%d", max_webcam_names-1);
            render_text(&ctlpane, 8, 2, str, MOUSE_EVENT_NONE);
        }

        // network section
        render_text(&ctlpane, 10, 0, "-- NETWORK --", MOUSE_EVENT_NONE);
        render_text(&ctlpane, 11, 0, 
                    CONFIG_PROXY == 'N' ? "PROXY DISABLED" : "PROXY ENABLED",
                    MOUSE_EVENT_CONFIG_PROXY);

        // time section
        render_text(&ctlpane, 13, 0, "-- TIME --", MOUSE_EVENT_NONE);
        render_text(&ctlpane, 14, 0, 
                    CONFIG_LOCALTIME == 'N' ? "GMT" : "LOCALTIME",
                    MOUSE_EVENT_CONFIG_TIME);
        sprintf(str, "OFF=%"PRId64" MS", system_clock_offset_us/1000);
        render_text(&ctlpane, 15, 0, str, MOUSE_EVENT_NONE);
    }

    // --------------------------------
    // ---- config mode: keybdpane ----
    // --------------------------------

    if (config_keybd_mode != CONFIG_KEYBD_MODE_INACTIVE) {
        static char * row_chars_unshift[4] = { "1234567890_",
                                               "qwertyuiop",
                                               "asdfghjkl",
                                               "zxcvbnm" };
        static char * row_chars_shift[4]   = { "1234567890_",
                                               "QWERTYUIOP",
                                               "ASDFGHJKL",
                                               "ZXCVBNM" };
        char ** row_chars;
        int     r, c;


        row_chars = (!config_keybd_shift ? row_chars_unshift : row_chars_shift);
        r = PANE_ROWS(&keybdpane,1) / 2 - 4;
        c = (PANE_COLS(&keybdpane,1) - 33) / 2;

        for (i = 0; i < 4; i++) {
            for (j = 0; row_chars[i][j] != '\0'; j++) {
                str[0] = row_chars[i][j];
                str[1] = '\0';
                render_text_ex(&keybdpane, r+2*i, c+3*j, str, str[0], 1, false, 1);
            }
        }

        render_text_ex(&keybdpane, r+8, c+0,  "SHIFT", MOUSE_EVENT_CONFIG_KEY_SHIFT, 0, false, 1);
        render_text_ex(&keybdpane, r+8, c+8,  "ESC",   MOUSE_EVENT_CONFIG_KEY_ESC,   0, false, 1);
        render_text_ex(&keybdpane, r+8, c+14, "BS",    MOUSE_EVENT_CONFIG_KEY_BS,    0, false, 1);
        render_text_ex(&keybdpane, r+8, c+19, "ENTER", MOUSE_EVENT_CONFIG_KEY_ENTER, 0, false, 1);
    }

    // ----------------------------------------------
    // ---- live & playback modes: image panes   ----
    //----- (except when config_mode keybd actv) ----
    // ----------------------------------------------

    if (config_keybd_mode == CONFIG_KEYBD_MODE_INACTIVE) {
        for (i = 0; i < MAX_WEBCAM; i++) {
            webcam_t * wc            = &webcam[i];
            char       win_id_str[2] = { 'A'+i, 0 };

            // acquire wc mutex
            pthread_mutex_lock(&wc->image_mutex);

            // display border 
            if (wc->image_highlight) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, SDL_ALPHA_OPAQUE);
            } else {
                SDL_SetRenderDrawColor(renderer, 0, 0, 255, SDL_ALPHA_OPAQUE);
            }
            SDL_RenderDrawRect(renderer, &wcpane[i]);
            SDL_RenderDrawLine(renderer, 
                               wcpane[i].x, wcpane[i].y+font[0].char_height+1,
                               wcpane[i].x+wcpane[i].w-1, wcpane[i].y+font[0].char_height+1);

            // display text line
            render_text(&wctitlepane[i], 0, 0, win_id_str, MOUSE_EVENT_NONE);
            render_text_ex(&wctitlepane[i], 
                           0, 2, 
                           wc->image_name, 
                           MOUSE_EVENT_WC_NAME+i,
                           PANE_COLS(&wctitlepane[i],0) - 6, 
                           false, 
                           0);
            render_text_ex(&wctitlepane[i], 
                           0, PANE_COLS(&wctitlepane[i],0) - 3,
                           wc->image_res, 
                           mode.mode == MODE_LIVE ? MOUSE_EVENT_WC_RES+i : MOUSE_EVENT_NONE,
                           3,
                           false, 
                           0);

            // display webcam_names
            if (wc->name_select_mode) {
                for (j = 0; j < max_webcam_names; j++) {
                    render_text(&wcimagepane[i], 1+2*j, 0, 
                                webcam_names[j],
                                MOUSE_EVENT_WC_NAME_LIST + 8 * i + j);
                }

            // display the image
            } else if (wc->image_display) {
                // create new texture, if needed
                if (wc->texture == NULL || 
                    wc->texture_w != wc->image_w || 
                    wc->texture_h != wc->image_h) 
                {
                    wc->texture_w = wc->image_w;
                    wc->texture_h = wc->image_h;
                    if (wc->texture != NULL) {
                        SDL_DestroyTexture(wc->texture);
                    }
                    wc->texture = SDL_CreateTexture(renderer, 
                                                    SDL_PIXELFORMAT_YUY2,
                                                    SDL_TEXTUREACCESS_STREAMING,
                                                    wc->texture_w,
                                                    wc->texture_h);
                    if (wc->texture == NULL) {
                        ERROR("SDL_CreateTexture failed\n");
                        exit(1);
                    }
                    DEBUG("created new texture %dx%d\n", wc->texture_w, wc->texture_h);
                }

                // update the texture with the image pixels  ZZZ LOCKING ?
                SDL_UpdateTexture(wc->texture,
                                  NULL,            // update entire texture
                                  wc->image,       // pixels
                                  wc->image_w*2);  // pitch

                // copy the texture to the render target
                SDL_RenderCopy(renderer, wc->texture, NULL, &wcimagepane[i]);

                // register for the zoom event
                event.mouse_event_pos[MOUSE_EVENT_WC_ZOOM+i] = wcimagepane[i];

            // display image notification text lines
            } else {
                int r = PANE_ROWS(&wcimagepane[i],0) / 2;
                render_text_ex(&wcimagepane[i], r-1, 0, wc->image_notification_str1, MOUSE_EVENT_NONE, 0, true, 0);
                render_text_ex(&wcimagepane[i], r,   0, wc->image_notification_str2, MOUSE_EVENT_NONE, 0, true, 0);
                render_text_ex(&wcimagepane[i], r+1, 0, wc->image_notification_str3, MOUSE_EVENT_NONE, 0, true, 0);

                // register for the zoom event
                event.mouse_event_pos[MOUSE_EVENT_WC_ZOOM+i] = wcimagepane[i];
            }

            // relsease wc mutex
            pthread_mutex_unlock(&wc->image_mutex);
        }
    }

    // ---------------------------------
    // ---- live mode: ctlpane      ----
    // ---- (except in config mode) ----
    // ---------------------------------

    if (mode.mode == MODE_LIVE && !config_mode) {
        // Ctl Pane ...
        //
        // 00:  LIVE
        // 01:
        // 02:  06/07/58
        // 03:  11:12:13

        strcpy(str, date_and_time_str);
        str[8] = '\0';
        render_text(&ctlpane, 0, 0, "LIVE", MOUSE_EVENT_MODE_SELECT);
        render_text(&ctlpane, 2, 0, str, MOUSE_EVENT_NONE);
        render_text(&ctlpane, 3, 0, str+9, MOUSE_EVENT_NONE);
    }

    // ---------------------------------
    // ---- playback mode: ctlpane  ----
    // ---- (except in config mode) ----
    // ---------------------------------

    // XXX also add set time
    if (mode.mode == MODE_PLAYBACK && !config_mode) {
        // Ctl Pane ...
        //
        // 00:   PLAYBACK         --- CONTROL
        // 01:
        // 02:   06/07/58
        // 03:   11:12:13
        // 04:
        // 05:   PLAYING         ... STATUS   (STOPPED | PAUSED)
        // 06:   FWD  1X         ... STATUS   (REV  1X)
        // 07:
        // 08:                                  STOPPED       PLAYING      PAUSED
        // 09:   STOP    PAUSE   ... CONTROL  (PLAY PAUSE | STOP PAUSE | STOP PLAY)
        // 10:
        // 11:   FWD     REV
        // 12:
        // 13:   FASTER  SLOWER

        // title
        render_text(&ctlpane, 0, 0, "PLAYBACK", MOUSE_EVENT_MODE_SELECT);

        // status: date and time
        strcpy(str, date_and_time_str);
        str[8] = '\0';
        render_text(&ctlpane, 2, 0, str, MOUSE_EVENT_NONE);
        render_text(&ctlpane, 3, 0, str+9, MOUSE_EVENT_NONE);

        // status: stop|play|pause, speed, and dir
        struct mode_s m = mode;
        render_text(&ctlpane, 5, 0, PB_SUBMODE_STR(m.pb_submode), MOUSE_EVENT_NONE);
        if (m.pb_speed >= 1) {
            sprintf(str, "%s  %.0fX", PB_DIR_STR(m.pb_dir), m.pb_speed);
        } else {
            sprintf(str, "%s  %.2fX", PB_DIR_STR(m.pb_dir), m.pb_speed);
        }
        render_text(&ctlpane, 6, 0, str, MOUSE_EVENT_NONE);

        // control: stop,play,pause
        if (m.pb_submode == PB_SUBMODE_STOP) {
            render_text(&ctlpane, 9, 0, "PLAY", MOUSE_EVENT_PLAYBACK_PLAY);
            render_text(&ctlpane, 9, 8, "PAUSE", MOUSE_EVENT_PLAYBACK_PAUSE);
        } else if (m.pb_submode == PB_SUBMODE_PLAY) {
            render_text(&ctlpane, 9, 0, "STOP", MOUSE_EVENT_PLAYBACK_STOP);
            render_text(&ctlpane, 9, 8, "PAUSE", MOUSE_EVENT_PLAYBACK_PAUSE);
        } else if (m.pb_submode == PB_SUBMODE_PAUSE) {
            render_text(&ctlpane, 9, 0, "STOP", MOUSE_EVENT_PLAYBACK_STOP);
            render_text(&ctlpane, 9, 8, "PLAY", MOUSE_EVENT_PLAYBACK_PLAY);
        } else {
            render_text(&ctlpane, 9, 0, "????", MOUSE_EVENT_NONE);
            render_text(&ctlpane, 9, 8, "????", MOUSE_EVENT_NONE);
        }

        // control: fwd,rev
        render_text(&ctlpane, 11, 0, "FWD", MOUSE_EVENT_PLAYBACK_FORWARD);
        render_text(&ctlpane, 11, 8, "REV", MOUSE_EVENT_PLAYBACK_REVERSE);

        // control: fast,slow
        render_text(&ctlpane, 13, 0, "FASTER", MOUSE_EVENT_PLAYBACK_FASTER);
        render_text(&ctlpane, 13, 8, "SLOWER", MOUSE_EVENT_PLAYBACK_SLOWER);
    }

    // -----------------------------------------------
    // ---- live & playback modes: ctlpane bottom ----
    // -----------------------------------------------

    // Ctl Pane Bottom ...
    // 
    // STATUS_TITLE
    // A <values> 
    // B <values>
    // C <values>.
    // D <values>
    // 
    // CONFIG  QUIT    

    // status display
    // ZZZ some say "not conn" and others don't
    if ((mode.mode == MODE_LIVE) || (mode.mode == MODE_PLAYBACK)) {
        switch (status_select) {

        case 0: {  // FRAMES/SEC
            uint64_t        delta_us;

            static uint64_t last_us;
            static uint64_t last_recvd_frames[MAX_WEBCAM];
            static char     static_str[MAX_WEBCAM][32];
            
            // if greater then 3 second since last values saved then
            //   recompute rates
            //   save last values
            // endif
            curr_us = microsec_timer();
            delta_us = curr_us - last_us;
            if (delta_us > 5000*MS) {
                for (i = 0; i < MAX_WEBCAM; i++) {
                    if (webcam[i].recvd_frames < last_recvd_frames[i]) {
                        last_recvd_frames[i] = webcam[i].recvd_frames;
                    }
                    sprintf(static_str[i], "%c %0.1f", 
                            'A'+i,
                            (double)(webcam[i].recvd_frames - last_recvd_frames[i]) * 1000000 / delta_us);
                    last_recvd_frames[i] = webcam[i].recvd_frames;
                }
                last_us = curr_us;
            }

            // display
            render_text(&ctlbpane, 0, 0, "FRAMES/SEC", MOUSE_EVENT_STATUS_SELECT);
            for (i = 0; i < MAX_WEBCAM; i++) {
                render_text(&ctlbpane, i+1, 0, static_str[i], MOUSE_EVENT_NONE);
            }
            break; }

        case 1: {  // TOTAL MB
            render_text(&ctlbpane, 0, 0, "TOTAL MB", MOUSE_EVENT_STATUS_SELECT);
            for (i = 0; i < MAX_WEBCAM; i++) {
                sprintf(str, "%c %3d", 'A'+i, (int)(webcam[i].recvd_bytes/1000000));
                render_text(&ctlbpane, i+1, 0, str, MOUSE_EVENT_NONE);
            }
            break; }

        case 2: {  // NETWORK
            render_text(&ctlbpane, 0, 0, "NETWORK", MOUSE_EVENT_STATUS_SELECT);
            for (i = 0; i < MAX_WEBCAM; i++) {
                sprintf(str, "%c %d %4d %4d",
                        'A'+i, 
                        webcam[i].p2p_id,
                        webcam[i].status.p2p_resend_cnt,
                        webcam[i].status.p2p_recvdup_cnt);
                render_text(&ctlbpane, i+1, 0, str, MOUSE_EVENT_NONE);
            }
            break; }

        case 3: { // REC DURATION
            render_text(&ctlbpane, 0, 0, "REC DURATION", MOUSE_EVENT_STATUS_SELECT);
            for (i = 0; i < MAX_WEBCAM; i++) {
                uint32_t days, hours, minutes, seconds;

                if (webcam[i].state == STATE_CONNECTED) {
                    CVT_INTERVAL_SECS_TO_DAY_HMS(webcam[i].status.rp_duration_us/1000000,
                                                days, hours, minutes, seconds);
                    sprintf(str, "%c %d:%02d:%02d", 'A'+i, 24*days+hours, minutes, seconds);
                } else {
                    sprintf(str, "%c not conn", 'A'+i);
                }
                render_text(&ctlbpane, i+1, 0, str, MOUSE_EVENT_NONE);
            }
            break; }

        case 4: { // VERSION 1.0
            sprintf(str, "VERSION %d.%d", VERSION_MAJOR, VERSION_MINOR);
            render_text(&ctlbpane, 0, 0, str, MOUSE_EVENT_STATUS_SELECT);
            for (i = 0; i < MAX_WEBCAM; i++) {
                if (webcam[i].state == STATE_CONNECTED) {
                    sprintf(str, "%c %d.%d", 'A'+i, 
                            webcam[i].status.version.major,
                            webcam[i].status.version.minor);
                } else {
                    sprintf(str, "%c not conn", 'A'+i);
                }
                render_text(&ctlbpane, i+1, 0, str, MOUSE_EVENT_NONE);
            }
            break; }

        }
    }

    // Config/Back and Quit
    render_text(&ctlbpane, 6, 0, !config_mode ? "CONFIG" : "BACK", MOUSE_EVENT_CONFIG_SELECT);
    render_text(&ctlbpane, 6, 10, "QUIT", MOUSE_EVENT_QUIT);

    // -----------------
    // ---- present ----
    // -----------------

    SDL_RenderPresent(renderer);
}

void render_text(SDL_Rect * pane, int row, int col, char * str, int mouse_event)
{
    render_text_ex(pane, row, col, str, mouse_event, 0, false, 0);
}

void render_text_ex(SDL_Rect * pane, int row, int col, char * str, int mouse_event, int field_cols, bool center, int font_id)
{
    SDL_Surface    * surface; 
    SDL_Texture    * texture; 
    SDL_Color        fg_color;
    SDL_Rect         pos;
    char             s[MAX_STR];
    int              slen;

    static SDL_Color fg_color_normal = {255,255,255}; 
    static SDL_Color fg_color_event  = {0,255,255}; 
    static SDL_Color bg_color        = {0,0,0}; 

    // if zero length string then nothing to do
    if (str[0] == '\0') {
        return;
    }

    // verify row and col
    if (row < 0 || row >= PANE_ROWS(pane,font_id) || 
        col < 0 || col >= PANE_COLS(pane,font_id)) 
    {
        return;
    }

    // if field_cols not supplied then determine it
    if (field_cols == 0) {
        field_cols = PANE_COLS(pane,font_id) - col;
        if (field_cols <= 0) {
            return;
        }
    }

    // make a copy of the str arg, and shorten if necessary
    strcpy(s, str);
    slen = strlen(s);
    if (slen > field_cols) {
        s[field_cols] = '\0';
        slen = field_cols;
    }

    // if centered then adjust col
    if (center) {
        col += (field_cols - slen) / 2;
    }

    // render the text to a surface0
    fg_color = (mouse_event != MOUSE_EVENT_NONE ? fg_color_event : fg_color_normal); 
    surface = TTF_RenderText_Shaded(font[font_id].font, s, fg_color, bg_color);
    if (surface == NULL) { 
        FATAL("TTF_RenderText_Shaded returned NULL\n");
    } 

    // determine the display location
    pos.x = pane->x + col * font[font_id].char_width;
    pos.y = pane->y + row * font[font_id].char_height;
    pos.w = surface->w;
    pos.h = surface->h;

    // create texture from the surface, and render the texture
    texture = SDL_CreateTextureFromSurface(renderer, surface); 
    SDL_RenderCopy(renderer, texture, NULL, &pos); 

    // clean up
    SDL_FreeSurface(surface); 
    SDL_DestroyTexture(texture); 

    // if there is a mouse_event then save the location for the event handler
    if (mouse_event != MOUSE_EVENT_NONE) {
        event.mouse_event_pos[mouse_event] = pos;
    }
}

// -----------------  WEBCAM THREAD  -------------------------------------

void * webcam_thread(void * cx) 
{
    #define STATE_CHANGE(new_state, s1, s2, s3) \
        do { \
            if ((new_state) != wc->state) { \
                INFO("wc %c: %s -> %s '%s' '%s' %s\n", \
                    id_char, STATE_STR(wc->state), STATE_STR(new_state), s1, s2, s3); \
                wc->state = (new_state); \
                last_state_change_time_us = microsec_timer(); \
                DISPLAY_TEXT(s1,s2,s3); \
            } \
        } while (0)

    #define RESOLUTION_STR(w,h) ((w) == 640 ? "HI" : (w) == 320 ? "MED" : (w) == 160 ? "LOW" : "???")

    #define DISPLAY_IMAGE(_image, _width, _height, _motion) \
        do { \
            pthread_mutex_lock(&wc->image_mutex); \
            if (_motion) { \
                wc->image_highlight = true; \
                last_highlight_enable_time_us = microsec_timer(); \
            } \
            strcpy(wc->image_res, RESOLUTION_STR(_width,_height)); \
            if (wc->image) { \
                free(wc->image); \
            } \
            wc->image = (_image); \
            wc->image_w = (_width); \
            wc->image_h = (_height); \
            wc->image_display = true;  \
            wc->image_change++; \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    #define DISPLAY_TEXT(s1,s2,s3); \
        do { \
            pthread_mutex_lock(&wc->image_mutex); \
            wc->image_highlight = false; \
            strcpy(wc->image_res, ""); \
            strcpy(wc->image_notification_str1, s1); \
            strcpy(wc->image_notification_str2, s2); \
            strcpy(wc->image_notification_str3, s3); \
            wc->image_display = false; \
            wc->image_change++; \
            pthread_mutex_unlock(&wc->image_mutex); \
            if ((s1)[0] != '\0' || (s2)[0] != '\0' || (s3)[0] != '\0') { \
                usleep(500*MS); \
            } \
        } while (0)

    #define DISPLAY_CLEAR_HIGHLIGHT() \
        do { \
            if (wc->image_highlight == false) { \
                break; \
            } \
            pthread_mutex_lock(&wc->image_mutex); \
            wc->image_highlight = false; \
            wc->image_change++; \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    #define DISPLAY_WC_NAME(dn) \
        do { \
            pthread_mutex_lock(&wc->image_mutex); \
            strcpy(wc->image_name, (dn)); \
            wc->image_change++; \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    #define OK_TO_CONNECT \
        (strcmp(CONFIG_WC_NAME(id), NO_WCNAME) != 0 && \
         strcmp(CONFIG_USERNAME, NO_USERNAME) != 0 && \
         strcmp(CONFIG_PASSWORD, NO_PASSWORD) != 0)

    #define DISCONNECT() \
        do { \
            if (handle != INVALID_HANDLE) { \
                p2p_disconnect(handle); \
                handle = INVALID_HANDLE; \
                bzero(&wc->status, sizeof(wc->status)); \
                wc->p2p_id = 0; \
                wc->recvd_bytes = 0; \
                wc->recvd_frames = 0; \
                p2p = NULL; \
            } \
        } while (0)

    #define INVALID_HANDLE (-1)

    int              id      = (int)(long)cx;
    char             id_char = 'A' + id;
    webcam_t       * wc      = &webcam[id];
    p2p_routines_t * p2p     = NULL;
    int              handle  = INVALID_HANDLE;

    uint64_t         last_state_change_time_us = microsec_timer();
    uint64_t         last_status_msg_recv_time_us = microsec_timer();
    uint64_t         last_highlight_enable_time_us = microsec_timer();
    char             last_zoom = '-';

    INFO("THREAD %d STARTING\n", id);

    // init non-zero fields of wc
    pthread_mutex_init(&wc->image_mutex,NULL);
    wc->state = STATE_NOT_CONNECTED;
    wc->change_name_request = -1;
    DISPLAY_WC_NAME(CONFIG_WC_NAME(id));

    // acknowledge that this thread has completed initialization
    __sync_fetch_and_add(&webcam_threads_running_count,1);

    // runtime
    while (true) {
        // handle change protocol
        if (handle != INVALID_HANDLE &&
            wc->p2p_id != (CONFIG_PROXY == 'N' ? 1 : 2)) 
        {
            DISCONNECT();
            STATE_CHANGE(STATE_NOT_CONNECTED, "", "", "");
        }

        // handle change_name_request
        if (wc->change_name_request != -1) {
            DISCONNECT();
            STATE_CHANGE(STATE_NOT_CONNECTED, "", "", "");

            strcpy(CONFIG_WC_NAME(id), webcam_names[wc->change_name_request]);
            CONFIG_WRITE();
            wc->change_name_request = -1;
            DISPLAY_WC_NAME(CONFIG_WC_NAME(id));
        }

        // state processing
        switch (wc->state) {
        case STATE_NOT_CONNECTED:
            if (OK_TO_CONNECT) {
                STATE_CHANGE(STATE_CONNECTING, "", "", "");
                break;
            }
            usleep(100*MS);
            break;

        case STATE_CONNECTING: {
            int h;

            // select the protocol
            p2p = (CONFIG_PROXY == 'N' ? &p2p1 : &p2p2); 
            wc->p2p_id = (p2p == &p2p1 ? 1 : 2);
            DISPLAY_TEXT("CONNECTING",
                         wc->p2p_id == 1 ? "PROXY DISABLED" : "PROXY ENABLED",
                         "");

            // attempt to connect to wc_name
            //ZZZ can this now return a status
            //     - p2p1 can receive a connect_reject packet, and there are other error paths
            //     - p2p2 can get error status back from connect_from_cloud_server
            //     - should use STATUS_T
            h = p2p_connect(CONFIG_USERNAME, CONFIG_PASSWORD, wc->image_name, SERVICE_WEBCAM);
            if (h < 0) {  
                STATE_CHANGE(STATE_CONNECTING_ERROR, "CONNECT ERROR", "", "");
                break;
            }

            // connect succeeded; init variables for the new connection
            handle = h;
            last_status_msg_recv_time_us = microsec_timer();
            last_highlight_enable_time_us = microsec_timer();
            last_zoom = '-';  // invalid zoom value
            bzero(&wc->mode, sizeof(struct mode_s));
            bzero(&wc->status, sizeof(struct status_s));
            wc->frame_status = STATUS_INFO_OK;
            STATE_CHANGE(STATE_CONNECTED, "CONNECTED", "", "");
            break; }

        case STATE_CONNECTED: {
            webcam_msg_t msg;
            int          ret, tmp_zoom;
            uint32_t     data_len, width, height;
            uint8_t      data[RP_MAX_FRAME_DATA_LEN];
            uint8_t    * image;
            uint64_t     curr_us;
            char         int_str[MAX_INT_STR];

            // if mode has changed then send message to webcam
            if (wc->mode.mode_id != mode.mode_id) {
                wc->mode = mode;

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
                           time2str(ts1,wc->mode.pb_mode_entry_real_time_us/1000000,CONFIG_LOCALTIME=='N'),
                           time2str(ts2,wc->mode.pb_real_time_us/1000000,CONFIG_LOCALTIME=='N'));
                }
#endif

                // send the new mode message to webcam 
                bzero(&msg,sizeof(msg));
                msg.msg_type = MSG_TYPE_CMD_SET_MODE;   
                msg.u.mt_cmd_set_mode = wc->mode;
                if ((ret = p2p_send(handle, &msg, sizeof(webcam_msg_t))) < 0) {
                    STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "send mode msg", "");
                    break;
                }
            }

            // if zoom has changed then send message to webcam to adjust the
            // frame webcam's frame xmit interval
            tmp_zoom = CONFIG_ZOOM;
            if (tmp_zoom != last_zoom) {
                uint64_t intvl_us = (tmp_zoom == 'A'+id ? 0 : tmp_zoom == 'N' ? 150*MS : 250*MS);

                INFO("wc %c: send MSG_TYPE_CMD_SET_MIN_SEND_INTVL_US intvl=%"PRId64" us\n", id_char, intvl_us);
                bzero(&msg,sizeof(msg));
                msg.msg_type = MSG_TYPE_CMD_SET_MIN_SEND_INTVL_US;   
                msg.u.mt_cmd_min_send_intvl.us = intvl_us;
                if ((ret = p2p_send(handle, &msg, sizeof(webcam_msg_t))) < 0) {
                    STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "send intvl msg", "");
                    break;
                }
                last_zoom = tmp_zoom;
            }

            // process resolution change request
            if (wc->change_resolution_request) {
                if (wc->mode.mode == MODE_LIVE) {
                    bzero(&msg,sizeof(msg));
                    msg.msg_type = MSG_TYPE_CMD_LIVE_MODE_CHANGE_RES;
                    if ((ret = p2p_send(handle, &msg, sizeof(webcam_msg_t))) < 0) {
                        STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "send res msg", "");
                        break;
                    }
                    DISPLAY_TEXT(status2str(STATUS_INFO_CHANGING_RESOLUTION), "", "");
                }
                wc->change_resolution_request = false;
            }

            // clear highlight if it is currently enabled and the last time it was
            // enabled is greater than HIGHLIGHT_TIME_US
            curr_us = microsec_timer();
            if ((wc->image_highlight) &&
                (curr_us - last_highlight_enable_time_us > HIGHLIGHT_TIME_US) &&
                !(wc->mode.mode == MODE_PLAYBACK && wc->mode.pb_submode == PB_SUBMODE_PAUSE))
            {
                DISPLAY_CLEAR_HIGHLIGHT();
            }

            // if an error condition exists then display the error
            if (curr_us - last_status_msg_recv_time_us > 10000000) {
                DISPLAY_TEXT(status2str(STATUS_ERR_DEAD), "", "");
            } else if (wc->mode.mode == MODE_LIVE && wc->status.cam_status != STATUS_INFO_OK) {
                DISPLAY_TEXT(status2str(wc->status.cam_status), "", "");
            } else if (wc->mode.mode == MODE_PLAYBACK && wc->status.rp_status != STATUS_INFO_OK) {
                DISPLAY_TEXT(status2str(wc->status.rp_status), "", "");
            }

            // receive msg header  
            ret = p2p_recv(handle, &msg, sizeof(msg), RECV_NOWAIT_ALL);
            if (ret != sizeof(msg)) {
                if (ret != RECV_WOULDBLOCK) {
                    STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "recv msg hdr", "");
                }
                usleep(MS);
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
                    if ((ret = p2p_recv(handle, data, data_len, RECV_WAIT_ALL)) != data_len) {
                        STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "recv msg data", "");
                        break;
                    }
                    wc->recvd_bytes += ret;
                }

                // increment recvd_frames count
                wc->recvd_frames++;

                // if mode_id in received msg does not match our mode_id then discard
                if (msg.u.mt_frame.mode_id != wc->mode.mode_id) {
                    WARN("wc %c: discarding frame msg because msg mode_id %"PRId64" is not expected %"PRId64"\n",
                         id_char, msg.u.mt_frame.mode_id, wc->mode.mode_id);
                    break;
                }

                // don't display if the status for the mode we're in is not okay
                if ((wc->mode.mode == MODE_LIVE && wc->status.cam_status != STATUS_INFO_OK) ||
                    (wc->mode.mode == MODE_PLAYBACK && wc->status.rp_status != STATUS_INFO_OK))
                {
                    uint32_t status = (wc->mode.mode == MODE_LIVE ? wc->status.cam_status 
                                                                  : wc->status.rp_status);
                    WARN("wc %c: discarding frame msg because %s status is %s\n",
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
                    wc->frame_status = msg.u.mt_frame.status;
                    DISPLAY_TEXT(status2str(msg.u.mt_frame.status), "", "");
                    break;
                }

                // jpeg decode
                ret = jpeg_decode(id, JPEG_DECODE_MODE_YUY2,
                                  data, data_len,             // jpeg
                                  &image, &width, &height);   // pixels
                if (ret < 0) {
                    ERROR("wc %c: jpeg decode ret=%d\n", id_char, ret);
                    wc->frame_status = STATUS_ERR_JPEG_DECODE;
                    DISPLAY_TEXT(status2str(STATUS_ERR_JPEG_DECODE), "", "");
                    break;
                }

                // display the image
                wc->frame_status = STATUS_INFO_OK;
                DISPLAY_IMAGE(image, width, height, msg.u.mt_frame.motion);
                break;

            case MSG_TYPE_STATUS:
                // save status and last time status recvd
                wc->status = msg.u.mt_status;
                last_status_msg_recv_time_us = microsec_timer();
                break;

            default:
                int2str(int_str, msg.msg_type);
                STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "invld msg type", int_str);
                break;
            }
            break; }

        case STATE_CONNECTING_ERROR:
        case STATE_CONNECTED_ERROR:
            DISPLAY_CLEAR_HIGHLIGHT();  
            DISCONNECT();
            if (microsec_timer() - last_state_change_time_us < RECONNECT_TIME_US) {
                usleep(100*MS);
                break;
            }
            STATE_CHANGE(STATE_NOT_CONNECTED, "", "", "");
            break;

        case STATE_FATAL_ERROR:
            break;

        default: {
            char int_str[MAX_INT_STR];

            int2str(int_str, wc->state);
            STATE_CHANGE(STATE_FATAL_ERROR, "DISABLED", "invalid state", int_str);
            break; }
        }

        // if quit event or in fatal error state then exit this thread
        if (event.quit_event || wc->state == STATE_FATAL_ERROR) {
            break;
        }
    }

    // disconnect
    DISCONNECT();

    // exit thread
    __sync_fetch_and_add(&webcam_threads_running_count,-1);
    INFO("THREAD %d TERMINATING\n", id);
    return NULL;
}

// -----------------  DEBUG THREAD  -------------------------------------------------

#ifndef ANDROID

#include <readline/readline.h>
#include <readline/history.h>

void * debug_thread(void * cx)
{
    #define MAX_GETCL_ARGV 10

    char * line = NULL;
    int    argc;
    char * argv[MAX_GETCL_ARGV];
    p2p_routines_t * p2p = &p2p1;

    // enable this thread to be cancelled
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    // loop until eof on debug input or thread cancelled
    while (true) {
        // print prompt and read cmd/args
        free(line);
        if ((line = readline("DEBUG> ")) == NULL) {
            break;
        }
        if (line[0] != '\0') {
            add_history(line);
        }

        // parse line to argc/argv
        argc = 0;
        while (true) {
            char * token = strtok(argc==0?line:NULL, " \n");
            if (token == NULL || argc == MAX_GETCL_ARGV) {
                break;
            }
            argv[argc++] = token;
        }

        // if blank line then continue
        if (argc == 0) {
            continue;
        }

        // process commands ...

        // cmd: help
        if (strcmp(argv[0], "help") == 0) {
            PRINTF("p2p_debug_con [<handle>]\n");
            PRINTF("p2p_monitor_ctl <handle> <secs>\n");
            continue;
        }

        // cmd: p2p_debug_con 
        if (strcmp(argv[0], "p2p_debug_con") == 0) {
            int handle;

            if (argc == 1) {
                handle = -1;
            } else if (sscanf(argv[1], "%d", &handle) != 1) {
                PRINTF("usage: p2p_debug_con [<handle>]\n");
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
                PRINTF("usage: p2p_monitor_ctl <handle> <secs>\n");
                continue;
            }
            p2p_monitor_ctl(handle, secs);
            continue;
        }

        // invalid command
        PRINTF("invalid command\n");
    }

    // eof on debug input
    event.quit_event = true;
    return NULL;
}

#endif
