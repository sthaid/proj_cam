#include "wc.h"
#include "button_sound.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_mixer.h>

// vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv
// TBD - is mutex needed on ctl.mode
// TBD LATER - copy fonts to this directory,  and incorporate in backup
// TBD LATER - debug commands to dump global data structures

// XXX can't resize window
// XXX save config to file, for easy restart, includes username and password
// XXX   - don't use the environment any more for username and password

// #ifdef ANDROID
//     // XXX this may be usefule for the setting file
//     const char * path = SDL_AndroidGetInternalStoragePath();
//     INFO("PATH '%s'\n", path);
//     //sleep(3);
// #endif

// XXX review all ifdef ANDROID
// XXX review all struct fields
// XXX search for YYY

//
// defines 
//

#define MAX_WEBCAM                  4
#define MAX_GETCL_ARGV              10
#define MAX_STR                     100  //YYY delete

#define WIN_WIDTH_INITIAL           1280
#define WIN_HEIGHT_INITIAL          800
#define WIN_WIDTH_MIN               800
#define WIN_HEIGHT_MIN              500
#define CTL_WIDTH                   115

#define STATE_NO_WC_ID_STR          0
#define STATE_CONNECTING            1
#define STATE_CONNECTED             2
#define STATE_CONNECTING_ERROR      3
#define STATE_CONNECTED_ERROR       4
#define STATE_FATAL_ERROR           5

#define MS                          1000
#define HIGHLIGHT_TIME_US           (2000*MS)
#define RECONNECT_TIME_US           (10000*MS)

#define IMAGE_DISPLAY_TEXT          1
#define IMAGE_DISPLAY_IMAGE         2

#define CONFIG_USERNAME             (config[0].value)
#define CONFIG_PASSWORD             (config[1].value)
#define CONFIG_WC_NAME(idx)         (config[2+idx].value)
#define CONFIG_PROTOCOL             (config[6].value[0])    // 1, 2
#define CONFIG_ZOOM                 (config[7].value[0])    // A, B, C, D, N
#define CONFIG_ZULU_TIME            (config[8].value[0])    // N, Y
#define CONFIG_DEBUG                (config[9].value[0])    // N, Y

#ifndef ANDROID
#define FONT_PATH                   "/usr/share/fonts/gnu-free/FreeMonoBold.ttf"
#define SDL_FLAGS                   SDL_WINDOW_RESIZABLE
#else
#define FONT_PATH                   "/system/fonts/Arial.ttf"
#define SDL_FLAGS                   SDL_WINDOW_FULLSCREEN_DESKTOP  //YYY
#endif

#define STATE_STR(state) \
   ((state) == STATE_NO_WC_ID_STR     ? "STATE_NO_WC_ID_STR"      : \
    (state) == STATE_CONNECTING       ? "STATE_CONNECTING"        : \
    (state) == STATE_CONNECTED        ? "STATE_CONNECTED"         : \
    (state) == STATE_CONNECTING_ERROR ? "STATE_CONNECTING_ERROR"  : \
    (state) == STATE_CONNECTED_ERROR  ? "STATE_CONNECTED_ERROR"   : \
    (state) == STATE_FATAL_ERROR      ? "STATE_FATAL_ERROR"        \
                                      : "????")

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

// YYY recheck the sleep time here
#define PLAY_BUTTON_SOUND() \
    do { \
        Mix_PlayChannel(-1, button_sound, 0); \
        usleep(300*MS); \
    } while (0)

#define CONFIG_WRITE() \
    do { \
        config_write(config_file_name, config); \
    } while (0)

//
// typedefs
//

typedef struct {
    int             state;
    int             handle;
    uint64_t        last_state_change_time_us;      // YYY put some of these in the thread
    uint64_t        last_status_msg_recv_time_us;
    uint64_t        last_dead_text_display_time_us;
    uint64_t        last_highlight_enable_time_us;
    uint32_t        last_frame_status;
    int32_t         last_zoom;
    struct mode_s   mode;
    struct status_s status;

    uint64_t        recvd_bytes;

    pthread_mutex_t image_mutex;
    uint64_t        image_change;
    char            image_wc_name[MAX_STR];
    char            image_wc_res[MAX_STR];
    bool            image_highlight;
    uint32_t        image_display;
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

    SDL_Texture * texture;
    int           texture_w;
    int           texture_h;
} webcamdi_t;

typedef struct {
    struct mode_s mode;

    SDL_Rect ctl_pos;
    SDL_Rect mode_pos;
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
    // YYY verify all of these events are supported
    bool quit_event;
    bool con_info_event;

    // mode change event
    bool mode_event;

    // playback events
    bool pb_stop_event;
    bool pb_play_pause_event;
    bool pb_speed_event;
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
} event_t;

//
// variables
//

SDL_Window     * window;
SDL_Renderer   * renderer;
int              win_width;
int              win_height;

TTF_Font       * font;
int              font_char_width;
int              font_char_height;

Mix_Chunk      * button_sound;

char             webcam_names[MAX_USER_WC][MAX_WC_NAME+1];
int              max_webcam_names;
int              webcam_threads_running_count;

webcam_t         webcam[MAX_WEBCAM];
webcamdi_t       webcamdi[MAX_WEBCAM];
event_t          event;
ctl_t            ctl;

char             config_file_name[MAX_STR];
config_t         config[] = { { "username",  "steve"    },
                              { "password",  "password" },
                              { "wc_name_A", "wc1"      },
                              { "wc_name_B", ""         },
                              { "wc_name_C", ""         },
                              { "wc_name_D", ""         },
                              { "protocol",  "1"        },
                              { "zoom",      "A",       },
                              { "zulu_time", "N"        },
                              { "debug",     "N"        },
                              { NULL,        NULL       } };

//
// prototypes
//

void event_handler(void);
void display_handler(void);
void render_text(char * str, SDL_Rect pos, bool ctl, bool centered);
void * webcam_thread(void * cx);
#ifndef ANDROID
void * debug_thread(void * cx);
bool getcl(int * argc, char ** argv);
#endif

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit  rl;
    const char         * dir;
    int            ret, i, count;
    int            sfd;
    FILE         * fp;
    char           s[MAX_STR];

    // set resource limti to allow core dumps
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    ret = setrlimit(RLIMIT_CORE, &rl);
    if (ret < 0) {
        WARN("setrlimit for core dump, %s\n", strerror(errno));
    }

    // read viewer config
    // YYY different on android ?
#ifndef ANDROID
    dir = getenv("HOME");
    if (dir == NULL) {
        FATAL("env var HOME not set\n");
    }
#else
    dir = SDL_AndroidGetInternalStoragePath();
    if (dir == NULL) {
        FATAL("android internal storage path not set\n");
    }
#endif
    sprintf(config_file_name, "%s/.viewer_config", dir);
    INFO("XXXXXXXXXXXXXXXXXX CONFIG_FILE_NAME %s\n", config_file_name);
    if (config_read(config_file_name, config) < 0) {
        FATAL("config_read failed for %s\n", config_file_name);
    }

    // verify username and password have been obtained from the config
    if (strlen(CONFIG_USERNAME) == 0) {
        FATAL("config does not contain username\n");
    }
    if (strlen(CONFIG_PASSWORD) == 0) {
        FATAL("config does not contain password\n");
    }

    // get list of available webcams 
    sfd = connect_to_cloud_server(CONFIG_USERNAME, CONFIG_PASSWORD, "command");
    if (sfd < 0) {
        FATAL("login to cloud_server failed\n");
    }
    fp = fdopen(sfd, "w+");   // mode: read/write
    if (fp == NULL) {
        FATAL("login to cloud_server failed fdopen\n");
    }
    fputs("ls\n", fp);
    while (fgets(s, sizeof(s), fp) != NULL) {
        strcpy(webcam_names[max_webcam_names++], strtok(s, " "));
    }
    fclose(fp);
    for (i = 0; i < max_webcam_names; i++) {  //YYY temp
        INFO("LIST %s\n", webcam_names[i]);
    }

    // initialize to live mode
    SET_CTL_MODE_LIVE();

    // initialize Simple DirectMedia Layer  (SDL)
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) < 0) {
        FATAL("SDL_Init failed\n");
    }

    // create SDL Window and Renderer
    if (SDL_CreateWindowAndRenderer(WIN_WIDTH_INITIAL, WIN_HEIGHT_INITIAL, SDL_FLAGS, &window, &renderer) != 0) {
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
    font = TTF_OpenFont(FONT_PATH, 20);
    if (font == NULL) {
        FATAL("failed TTF_OpenFont %s\n", FONT_PATH);
    }
    TTF_SizeText(font, "X", &font_char_width, &font_char_height);

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

    // loop: process events and update display
    while (!event.quit_event) {
        event_handler();
        display_handler();
        usleep(10*MS);
    }

    // wait for up to 3 second for the webcam threads to terminate
    // YYY does this code run on android when the pgm exits
    // YYY what is causing the threads to exit?
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

// -----------------  EVENT HANDLER  -------------------------------------

void event_handler(void)
{
    SDL_Event ev;
    int       i;

    #define MOUSE_AT_POS(pos) (ev.button.x >= (pos).x && \
                               ev.button.x < (pos).x + (pos).w && \
                               ev.button.y >= (pos).y && \
                               ev.button.y < (pos).y + (pos).h)

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
        case SDL_MOUSEBUTTONDOWN: 
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

            // webcam events
            for (i = 0; i < MAX_WEBCAM; i++) {
                webcamdi_t * wcdi = &webcamdi[i];

                if (MOUSE_AT_POS(wcdi->image_pos)) {
                    event.zoom_event = true;
                    event.zoom_value = 'A'+i;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(wcdi->res_pos) && ctl.mode.mode == MODE_LIVE) {
                    event.wc[i].res_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
#if 0
                if (MOUSE_AT_POS(wcdi->wc_name_pos)) {
                    event.wc_name_input_in_prog[i] = true;  //YYY
                    PLAY_BUTTON_SOUND();
                    break;
                }
#endif
            }
            if (i < MAX_WEBCAM) {
                break;
            }

#if 0 // YYY later
            // control panel events
            if (MOUSE_AT_POS(ctl.mode_pos)) {
                event.mode_event = true;
                PLAY_BUTTON_SOUND();
                break;
            }
#endif
            if (MOUSE_AT_POS(ctl.con_info_title_pos)) {
                event.con_info_event = true;
                PLAY_BUTTON_SOUND();
                break;
            }
#if 0 // YYY later
            if (ctl.mode.mode == MODE_PLAYBACK) {
                if (MOUSE_AT_POS(ctl.pb_stop_pos)) {
                    event.pb_stop_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_play_pause_pos)) {
                    event.pb_play_pause_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_dir_value_pos)) {
                    event.pb_dir_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_speed_value_pos)) {
                    event.pb_speed_input_in_prog = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_sec_minus_pos)) {
                    event.pb_sec_minus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_sec_plus_pos)) {
                    event.pb_sec_plus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_min_minus_pos)) {
                    event.pb_min_minus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_min_plus_pos)) {
                    event.pb_min_plus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_hour_minus_pos)) {
                    event.pb_hour_minus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_hour_plus_pos)) {
                    event.pb_hour_plus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_day_minus_pos)) {
                    event.pb_day_minus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
                if (MOUSE_AT_POS(ctl.pb_day_plus_pos)) {
                    event.pb_day_plus_event = true;
                    PLAY_BUTTON_SOUND();
                    break;
                }
            }
#endif
            break;

#ifndef ANDROID  //YYY do I need this ifdef
       case SDL_WINDOWEVENT: 
            // INFO("YYY GOT WINDOWEVENT %d\n", ev.window.event);
            switch (ev.window.event)  {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                // INFO("YYY  -- SIZE CHANGE %d %d\n",
                       // ev.window.data1, ev.window.data2);
                event.resize_w = ev.window.data1;
                event.resize_h = ev.window.data2;
                event.resize_event = true;
                PLAY_BUTTON_SOUND();
                break;
            default:
                break;
            }
            break;
#endif

        case SDL_QUIT:
            event.quit_event = true;
            PLAY_BUTTON_SOUND();
            break;
        }

#if 0
//YYY maybe these should not be done here

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
        // YYY 
        if (event.pb_speed_event) {
            double speed;
            if (sscanf(event.pb_speed_event_value_str, "%lf", &speed) == 1 &&
                speed >= PB_SPEED_MIN && speed <= PB_SPEED_MAX) 
            {
                SET_CTL_MODE_PLAYBACK_SPEED(speed);
            } else {
                WARN("speed '%s' is invalid\n", event.pb_speed_event_value_str);
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
            delta_us = -1 * 1000000LL;
            event.pb_sec_minus_event = false;
        }
        if (event.pb_sec_plus_event) {
            delta_us = 1 * 1000000LL;
            event.pb_sec_plus_event = false;
        }
        if (event.pb_min_minus_event) {
            delta_us = -60 * 1000000LL;
            event.pb_min_minus_event = false;
        }
        if (event.pb_min_plus_event) {
            delta_us = 60 * 1000000LL;
            event.pb_min_plus_event = false;
        }
        if (event.pb_hour_minus_event) {
            delta_us = -3600 * 1000000LL;
            event.pb_hour_minus_event = false;
        }
        if (event.pb_hour_plus_event) {
            delta_us = 3600 * 1000000LL;
            event.pb_hour_plus_event = false;
        }
        if (event.pb_day_minus_event) {
            delta_us = -86400 * 1000000LL;
            event.pb_day_minus_event = false;
        }
        if (event.pb_day_plus_event) {
            delta_us = 86400 * 1000000LL;
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
#endif
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
                     ctl_x + font_char_width/2 + (c) * font_char_width, \
                     ctl_y + (r) * font_char_height + 1,  \
                     (len) * font_char_width,  \
                     font_char_height); \
        } while (0)

    #define RENDER_CLEAR_ALL() \
        do { \
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE); \
            SDL_RenderClear(renderer); \
        } while (0)

    #define RENDER_CLEAR_RECT(pos) \
        do { \
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE); \
            SDL_RenderFillRects(renderer, &(pos), 1); \
        } while (0)

    #define RENDER_BORDER(pos, highlight) \
        do { \
            if (highlight) { \
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, SDL_ALPHA_OPAQUE); \
            } else { \
                SDL_SetRenderDrawColor(renderer, 0, 0, 255, SDL_ALPHA_OPAQUE); \
            } \
            SDL_RenderDrawRect(renderer, &(pos)); \
            SDL_RenderDrawLine(renderer,  \
                               (pos).x, (pos).y+font_char_height+1, \
                               (pos).x+(pos).w-1, (pos).y+font_char_height+1); \
        } while (0)

    #define RENDER_TEXT(str, pos, ctl, centered) \
        do { \
            render_text(str, pos, ctl, centered); \
        } while (0)

    #define RENDER_PRESENT() \
        do { \
            SDL_RenderPresent(renderer); \
        } while (0)

    bool            event_handled = false;
    int             i;
    char            date_and_time_str[MAX_TIME_STR];
    time_t          secs;

    static int      con_info_select;

    static uint64_t last_mode_id;
    static uint64_t last_image_change[MAX_WEBCAM];
    static uint64_t last_window_update_us;
    static char     last_date_and_time_str[MAX_TIME_STR];

    //
    // process events
    // YYY process these in the event_handler
    //

    // zoom event 
    if (event.zoom_event) {
        CONFIG_ZOOM = (CONFIG_ZOOM == event.zoom_value ? 'N' : event.zoom_value);
        CONFIG_WRITE();
        event.zoom_event = false;
        event_handled = true;
    }

    // connection info event
    if (event.con_info_event) {
        event.con_info_event = false;
        con_info_select = (con_info_select + 1) % 3;  // YYY this should be the update condition
        event_handled = true;
    }

#ifndef ANDROID  //YYY maybe don't need this ifdef
    // resize event
    if (event.resize_event) {
        win_width = event.resize_w;
        win_height = event.resize_h;
        if (win_width < WIN_WIDTH_MIN || win_height < WIN_HEIGHT_MIN) {
            if (win_width < WIN_WIDTH_MIN) {
                win_width = WIN_WIDTH_MIN;
            }
            if (win_height < WIN_HEIGHT_MIN) {
                win_height = WIN_HEIGHT_MIN;
            }
            INFO("YYY CALLING SDL_SetWindowSize   %d %d\n",  win_width, win_height);
            SDL_SetWindowSize(window, win_width, win_height);
        }
        event.resize_event = false;
        event_handled = true;
    }
#endif

    //
    // create the data_and_tims_str
    //

    if (ctl.mode.mode == MODE_LIVE) {
        secs = time(NULL);
    } else {
        if (ctl.mode.pb_submode == PB_SUBMODE_PLAY) {
            secs = PB_SUBMODE_PLAY_REAL_TIME_US(&ctl.mode) / 1000000;
        } else {
            secs = ctl.mode.pb_real_time_us / 1000000;
        }
    }
    time2str(date_and_time_str, secs, CONFIG_ZULU_TIME=='Y');
    if (CONFIG_ZULU_TIME=='Y') {
        strcpy(date_and_time_str+17, " Z");
    }

    // YYY review number of calls to microsec_timer

    //
    // check if display needs to be rendered, if not then return
    //
    // the following conditions require display update
    // - mode has changed
    // - an image has changed (either pane, name, or resolution)
    // - an event is handled 
    // - control pane date_time change and last update > 250ms ago
    // - last update was greater than 1 second ago
    //

    do {
        if (ctl.mode.mode_id != last_mode_id) {
            // INFO("YYY update - ctl mode change\n");
            break;
        }

        for (i = 0; i < MAX_WEBCAM; i++) {
            if (webcam[i].image_change != last_image_change[i]) {
                break;
            }
        }
        if (i < MAX_WEBCAM) {
            static int YYY_COUNT;
            INFO("YYY update - image %d change   COUNT %d\n", i, YYY_COUNT++);
            break;
        }

        if (event_handled) {
            // INFO("YYY update - event change\n");
            break;
        }

        if (strcmp(date_and_time_str, last_date_and_time_str) != 0 &&
            microsec_timer() - last_window_update_us > 250*MS)
        {
            // INFO("YYY update - date/time change\n");
            break;
        }

        if (microsec_timer() - last_window_update_us > 1000*MS) {
            // INFO("YYY update - one sec change\n");
            break;
        }

        return;
    } while (0);

    //
    //  save the 'last' values used in the code block above
    //

    last_mode_id= ctl.mode.mode_id;
    for (i = 0; i < MAX_WEBCAM; i++) {
        last_image_change[i] = webcam[i].image_change;
    }
    strcpy(last_date_and_time_str, date_and_time_str);
    last_window_update_us = microsec_timer();

    //
    // if the display layout has changed then
    // recompute positions of the components
    //

    // YYY if (display layout has changed) {
    if (true) {
        int ctl_x, ctl_y, ctl_w, ctl_h;
        int small_win_count = 0;

        // compute new ctl screen element positions
        ctl_x = win_width-CTL_WIDTH; 
        ctl_y = 0; 
        ctl_w = CTL_WIDTH; 
        ctl_h = win_height;
        INIT_POS(ctl.ctl_pos, ctl_x, ctl_y, ctl_w, ctl_h);
        INIT_CTL_POS(mode_pos,                0,  0,  8);
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
            if (CONFIG_ZOOM == 'N') {
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
                if (i == CONFIG_ZOOM - 'A') {
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
                     wc_x + font_char_width / 2,
                     wc_y + 1, 
                     2 * font_char_width, 
                     font_char_height);
            INIT_POS(wcdi->wc_name_pos, 
                     wc_x + 2 * font_char_width + font_char_width / 2,
                     wc_y + 1, 
                     (wc_w / font_char_width - 7) * font_char_width, 
                     font_char_height);
            INIT_POS(wcdi->res_pos,    
                     wc_x + wc_w - 3 * font_char_width - font_char_width / 2, 
                     wc_y + 1, 
                     3 * font_char_width, 
                     font_char_height);
            INIT_POS(wcdi->image_pos,
                     wc_x + 1,
                     wc_y + 1 + font_char_height + 1,
                     wc_w - 2 * 1,
                     wc_h - font_char_height - 3 * 1);
            INIT_POS(wcdi->image_str1_pos,
                     wcdi->image_pos.x,
                     wcdi->image_pos.y + wcdi->image_pos.h / 2 - font_char_height,
                     wcdi->image_pos.w,
                     font_char_height);
            INIT_POS(wcdi->image_str2_pos,
                     wcdi->image_pos.x,
                     wcdi->image_pos.y + wcdi->image_pos.h / 2,
                     wcdi->image_pos.w,
                     font_char_height);
            INIT_POS(wcdi->image_str3_pos,
                     wcdi->image_pos.x,
                     wcdi->image_pos.y + wcdi->image_pos.h / 2 + font_char_height,
                     wcdi->image_pos.w,
                     font_char_height);
        }
    }

    //
    // render clear the entire window
    //

    RENDER_CLEAR_ALL();

    //
    // update the display for each webcam
    //

    for (i = 0; i < MAX_WEBCAM; i++) {
        webcam_t   * wc   = &webcam[i];
        webcamdi_t * wcdi = &webcamdi[i];
        char         win_id_str[2];

        // acquire wc mutex
        pthread_mutex_lock(&wc->image_mutex);

        // display border
        RENDER_BORDER(wcdi->wc_pos, wc->image_highlight);

        // display text line
        sprintf(win_id_str, "%c", 'A'+i);
        RENDER_TEXT(win_id_str, wcdi->win_id_pos, false, false);
        RENDER_TEXT(wc->image_wc_name, wcdi->wc_name_pos, true, false);
        RENDER_TEXT(wc->image_wc_res, wcdi->res_pos, ctl.mode.mode == MODE_LIVE, false);

        // display image pane
        if (wc->image_display == IMAGE_DISPLAY_IMAGE) {
            // create new texture, if needed
            if (wcdi->texture == NULL || wcdi->texture_w != wc->image_w || wcdi->texture_h != wc->image_h) {
                wcdi->texture_w = wc->image_w;
                wcdi->texture_h = wc->image_h;
                if (wcdi->texture != NULL) {
                    SDL_DestroyTexture(wcdi->texture);
                }
                wcdi->texture = SDL_CreateTexture(renderer, 
                                                   SDL_PIXELFORMAT_YUY2,
                                                   SDL_TEXTUREACCESS_STREAMING,  // YYY locking ?
                                                   wcdi->texture_w,
                                                   wcdi->texture_h);
                if (wcdi->texture == NULL) {
                    ERROR("SDL_CreateTexture failed\n");
                    exit(1);
                }
                DEBUG("created new texture %dx%d\n", wcdi->texture_w, wcdi->texture_h);
            }

            // update the texture with the image pixels  YYY LOCKING ?
            SDL_UpdateTexture(wcdi->texture,
                              NULL,            // update entire texture
                              wc->image,       // pixels
                              wc->image_w*2);  // pitch

            // copy the texture to the render target
            SDL_RenderCopy(renderer, wcdi->texture, NULL, &wcdi->image_pos);
        } else {
            // YYY more lines
            RENDER_TEXT(wc->image_str1, wcdi->image_str1_pos, false, true);
            RENDER_TEXT(wc->image_str2, wcdi->image_str2_pos, false, true);
            RENDER_TEXT(wc->image_str3, wcdi->image_str3_pos, false, true);
        }

        // relsease wc mutex
        pthread_mutex_unlock(&wc->image_mutex);
    }

    //
    // display control/status pane
    //

    if (ctl.mode.mode == MODE_LIVE) {
        // LIVE MODE ...
        //
        //       123456789 1
        //       -----------
        // 00:   LIVE      
        // 01:
        // 02:   06/07/58
        // 03:   11:12:13

        RENDER_TEXT(MODE_STR(ctl.mode.mode), ctl.mode_pos, true, false);
        date_and_time_str[8] = '\0';
        RENDER_TEXT(date_and_time_str, ctl.date_pos, false, false);
        RENDER_TEXT(date_and_time_str+9, ctl.time_pos, false, false);
    } else {  // ctl.mode.mode == MODE_PLAYBACK
        // PLAYBACK MODE ...
        //
        // YYY tbd
    }

    // CONNECTION INFO ...
    //
    //       123456789 1
    //       -----------
    // 23:   RATE Mb/Sec    (and other modes)
    // 24:   A 3.123
    // 25:   B 3.123
    // 26:   C 3.123
    // 27:   D 3.123

//YYY add FPS
    switch (con_info_select) {
    case 0: {
        char str[32];

        RENDER_TEXT("TOTAL MB", ctl.con_info_title_pos, true, false);
        for (i = 0; i < MAX_WEBCAM; i++) {
            sprintf(str, "%c %5d", 'A'+i, (int)(webcam[i].recvd_bytes/1000000));
            RENDER_TEXT(str, ctl.con_info_pos[i], false, false);
        }
        break; }
    case 1: {
        uint64_t        delta_us;
        uint64_t        curr_us = microsec_timer();

        static uint64_t last_us;
        static uint64_t last_recvd_bytes[MAX_WEBCAM];
        static char     str[MAX_WEBCAM][32];
        
        // if greater then 1 second since last values saved then
        //   if less then 5 secs then
        //     recompute rates
        //   endif
        //   save last values
        // endif
        delta_us = curr_us - last_us;
        if (delta_us > 1000*MS) {
            if (delta_us < 5000*MS) {
                for (i = 0; i < MAX_WEBCAM; i++) {
                    sprintf(str[i], "%c %5.3f", 
                            'A'+i,
                            8.0 * (webcam[i].recvd_bytes - last_recvd_bytes[i]) / delta_us);
                }
            } else {
                for (i = 0; i < MAX_WEBCAM; i++) {
                    sprintf(str[i], "%c ---", 'A'+i);
                }
            }
            for (i = 0; i < MAX_WEBCAM; i++) {
                last_recvd_bytes[i] = webcam[i].recvd_bytes;
            }
            last_us = curr_us;
        }

        // display
        RENDER_TEXT("RATE Mb/S", ctl.con_info_title_pos, true, false);
        for (i = 0; i < MAX_WEBCAM; i++) {
            RENDER_TEXT(str[i], ctl.con_info_pos[i], false, false);
        }
        break; }
    case 2: {
        char str[32];

        RENDER_TEXT("P2P SND DUP", ctl.con_info_title_pos, true, false);
        for (i = 0; i < MAX_WEBCAM; i++) {
            sprintf(str, "%c %4d %4d",
                    'A'+i, 
                    webcam[i].status.p2p_resend_cnt,  //YYY where are these from
                    webcam[i].status.p2p_recvdup_cnt);
            RENDER_TEXT(str, ctl.con_info_pos[i], false, false);
        }
        break; }
    }

    //
    // render present
    //

    RENDER_PRESENT();
}

void render_text(char * str, SDL_Rect pos, bool ctl, bool centered)
{
    SDL_Surface    * surface; 
    SDL_Texture    * texture; 

    static SDL_Color fg_color_normal = {255,255,255}; 
    static SDL_Color fg_color_ctl    = {0,255,255}; 
    static SDL_Color bg_color        = {0,0,0}; 

    // YYY tbd reduce strlen if too long

    surface = TTF_RenderText_Shaded(font, str, ctl ? fg_color_ctl : fg_color_normal, bg_color); 
    if (surface == NULL) { 
        return;
    } 

    texture = SDL_CreateTextureFromSurface(renderer, surface); 

    if (!centered || surface->w >= pos.w) { 
        pos.w = surface->w; 
    } else { 
        pos.x += (pos.w - surface->w) / 2; 
        pos.w = surface->w; 
    } 

    SDL_RenderCopy(renderer, texture, NULL, &pos); 
    SDL_FreeSurface(surface); 
    SDL_DestroyTexture(texture); 
}

#if 0
YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY
YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY
YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY

// YYY redo this pane
// YYY android needs quit button for pgm
    //
    //     123456789 1
    //     -----------
    // 00: PLAYBACK  
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
    }

    // determine if there has been a seconds time tick
    time_sec_now  = time(NULL);
    time_sec_tick = (time_sec_now != time_sec_last);
    time_sec_last = time_sec_now;

    // readthe microsec_timer
    curr_time_us = microsec_timer();

    // update mode
    if (display_all || mode_change) {
        RENDER_TEXT(MODE_STR(ctl.mode.mode), ctl.mode_pos, true, false);
    }

    // update date and time
    if (display_all || mode_change || time_sec_tick || (ctl.mode.mode == MODE_PLAYBACK && ctl.mode.pb_speed > 1)) {
        date_and_time_str[8] = '\0';
        RENDER_TEXT(date_and_time_str, ctl.date_pos, false, false);
        RENDER_TEXT(date_and_time_str+9, ctl.time_pos, false, false);
    }

    // update playback time
    // YYY don't need this
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

        RENDER_TEXT(playback_time_str, ctl.pb_time_pos, false, false);
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
            strcpy(speed_str, event.text_input_str); //YYY 
            if ((curr_time_us % 1000000) < 500000) {
                strcat(speed_str, "_");
            } else {
                strcat(speed_str, " ");
            }
            int field_width    = ctl.pb_speed_value_pos.w / font_char_width;
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
        RENDER_TEXT(state_str,      ctl.pb_state_pos,       false, false);
        RENDER_TEXT("STOP",         ctl.pb_stop_pos,        true,  false);
        RENDER_TEXT(play_pause_str, ctl.pb_play_pause_pos,  true,  false);
        RENDER_TEXT("DIR",          ctl.pb_dir_label_pos,   false, false);
        RENDER_TEXT(dir_str,        ctl.pb_dir_value_pos,   true,  false);
        RENDER_TEXT("SPEED",        ctl.pb_speed_label_pos, false, false);
        RENDER_TEXT(speed_str,      ctl.pb_speed_value_pos, true,  false);
    }

    // update playback time control
    // YYY these are too close togethor
    if ((ctl.mode.mode == MODE_PLAYBACK) &&
        (display_all || mode_change)) 
    {
        RENDER_TEXT("SEC",   ctl.pb_sec_pos,       false, false);
        RENDER_TEXT("-",     ctl.pb_sec_minus_pos, true,  false);
        RENDER_TEXT("+",     ctl.pb_sec_plus_pos,  true,  false);
        RENDER_TEXT("MIN",   ctl.pb_min_pos,       false, false);
        RENDER_TEXT("-",     ctl.pb_min_minus_pos, true,  false);
        RENDER_TEXT("+",     ctl.pb_min_plus_pos,  true,  false);
        RENDER_TEXT("HOUR",  ctl.pb_hour_pos,      false, false);
        RENDER_TEXT("-",     ctl.pb_hour_minus_pos,true,  false);
        RENDER_TEXT("+",     ctl.pb_hour_plus_pos, true,  false);
        RENDER_TEXT("DAY",   ctl.pb_day_pos,       false, false);
        RENDER_TEXT("-",     ctl.pb_day_minus_pos, true,  false);
        RENDER_TEXT("+",     ctl.pb_day_plus_pos,  true,  false);
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

            RENDER_TEXT(record_dur_str, ctl.pb_record_dur_pos[i], false, false);
        }
    }
#endif

// -----------------  WEBCAM THREAD  -------------------------------------

void * webcam_thread(void * cx) 
{
    #define INVALID_HANDLE (-1)

    #define STATE_CHANGE(new_state, s1, s2, s3) \
        do { \
            INFO("wc %c: %s -> %s '%s' '%s' %s\n", \
                 id_char, STATE_STR(wc->state), STATE_STR(new_state), s1, s2, s3); \
            wc->state = (new_state); \
            wc->last_state_change_time_us = microsec_timer(); \
            DISPLAY_TEXT(s1,s2,s3); \
        } while (0)

    #define RESOLUTION_STR(w,h) ((w) == 640 ? "HI" : (w) == 320 ? "MED" : (w) == 160 ? "LOW" : "???")

    #define DISPLAY_IMAGE(_image, _width, _height, _motion) \
        do { \
            pthread_mutex_lock(&wc->image_mutex); \
            if (_motion) { \
                wc->image_highlight = true; \
                wc->last_highlight_enable_time_us = microsec_timer(); \
            } \
            strcpy(wc->image_wc_res, RESOLUTION_STR(_width,_height)); \
            if (wc->image) { \
                free(wc->image); \
            } \
            wc->image = (_image); \
            wc->image_w = (_width); \
            wc->image_h = (_height); \
            wc->image_display = IMAGE_DISPLAY_IMAGE;  \
            wc->image_change++; \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    #define DISPLAY_TEXT(s1,s2,s3); \
        do { \
            DEBUG("wc %c: DISPLAY_TEXT: %s - %s - %s\n", id_char, s1, s2, s3); \
            pthread_mutex_lock(&wc->image_mutex); \
            wc->image_highlight = false; \
            strcpy(wc->image_wc_res, ""); \
            strcpy(wc->image_str1, s1); \
            strcpy(wc->image_str2, s2); \
            strcpy(wc->image_str3, s3); \
            wc->image_display = IMAGE_DISPLAY_TEXT; \
            wc->image_change++; \
            pthread_mutex_unlock(&wc->image_mutex); \
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
            strcpy(wc->image_wc_name, (dn)); \
            wc->image_change++; \
            pthread_mutex_unlock(&wc->image_mutex); \
        } while (0)

    int                 id                        = (int)(long)cx;
    char                id_char                   = 'A' + id;
    webcam_t          * wc                        = &webcam[id];
    struct wc_event_s * wcev                      = &event.wc[id];
    p2p_routines_t    * p2p;

    DEBUG("THREAD %d STARTING\n", id);

    // YYY needs a dropdown for selecting the webcam

    pthread_mutex_init(&wc->image_mutex,NULL);

    p2p = (CONFIG_PROTOCOL == '1' ? &p2p1 : &p2p2);   // YYY change when protocol has changed


    wc->handle = INVALID_HANDLE;

    DISPLAY_WC_NAME(CONFIG_WC_NAME(id));
    if (wc->image_wc_name[0] != '\0') {
        STATE_CHANGE(STATE_CONNECTING, "CONNECTING", "", "");
    } else {
        STATE_CHANGE(STATE_NO_WC_ID_STR, "NO SELECTION", "", "");
    }

    __sync_fetch_and_add(&webcam_threads_running_count,1);

    while (true) {
        // wc_name event processing
        if (wcev->wc_name_event) {
            if (wc->handle != INVALID_HANDLE) {
                p2p_disconnect(wc->handle);
                wc->handle = INVALID_HANDLE;
            }
            DISPLAY_WC_NAME(wcev->wc_name);  //YYY config update wehn this is changed
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
            usleep(100*MS);
            break;

        case STATE_CONNECTING: {
            int h;

            // attempt to connect to wc_name
            DEBUG("wc %c: STATE_CONNECTING connected to %s\n", id_char, wc->image_wc_name);
            h = p2p_connect(CONFIG_USERNAME, CONFIG_PASSWORD, wc->image_wc_name, SERVICE_WEBCAM);
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
            wc->last_zoom = 'X';  // invalid zoom value
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
            // YYY comment
            tmp_zoom = CONFIG_ZOOM;
            if (tmp_zoom != wc->last_zoom) {
                uint64_t intvl_us = (tmp_zoom == id ? 0 : tmp_zoom == 'N' ? 150*MS : 250*MS);

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
                    usleep(500*MS);  // YYY does this work?
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
            // YYY check for quit
            ret = p2p_recv(wc->handle, &msg, sizeof(msg), RECV_NOWAIT_ALL);
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
                    if ((ret = p2p_recv(wc->handle, data, data_len, RECV_WAIT_ALL)) != data_len) {
                        STATE_CHANGE(STATE_CONNECTED_ERROR, "ERROR", "recv msg data", "");
                        break;
                    }
                    wc->recvd_bytes += ret;
                }

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
                DISPLAY_IMAGE(image, width, height, msg.u.mt_frame.motion);
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
            if (wc->handle != INVALID_HANDLE) {
                p2p_disconnect(wc->handle);
                wc->handle = INVALID_HANDLE;
            }
            if (microsec_timer() - wc->last_state_change_time_us < RECONNECT_TIME_US) {
                usleep(100*MS);
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
    if (wc->handle != INVALID_HANDLE) {
        p2p_disconnect(wc->handle);
        wc->handle = INVALID_HANDLE;
    }

    // exit thread
    __sync_fetch_and_add(&webcam_threads_running_count,-1);
    return NULL;
}

// -----------------  DEBUG THREAD  -------------------------------------------------

#ifndef ANDROID
void * debug_thread(void * cx)
{
    int    argc;
    char * argv[MAX_GETCL_ARGV];

    // enable this thread to be cancelled
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

    // loop until eof on debug input or thread cancelled
    while (true) {
        // print prompt and read cmd/args
        PRINTF("DEBUG> ");
        if (getcl(&argc,argv) == false) {
            break;
        }

        // if blank line then continue
        if (argc == 0) {
            continue;
        }

        // cmd: help
        if (strcmp(argv[0], "help") == 0) {
            PRINTF("p2p_debug_con [<handle>]\n");
            PRINTF("p2p_monitor_ctl <handle> <secs>\n");
            continue;
        }

#if 0 // YYY
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
#endif
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
#endif
