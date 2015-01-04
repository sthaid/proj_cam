#define _FILE_OFFSET_BITS 64

#include "wc.h"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

//
// defines
//

// tuning
#define MAX_BUFMAP             32  
#define FRAMES_PER_SEC         10
#define FRAME_COMPARE_INTERVAL 5

// webcam base device name
#define WC_VIDEO "/dev/video"

// configuration
#define CONFIG_PATH         ".wc_config"
#define CONFIG_VERSION      1
#define CONFIG_RESOLUTION   (config[0].value[0])
#define CONFIG_WRITE() \
    do { \
        config_write(CONFIG_PATH, config, CONFIG_VERSION); \
    } while (0)

// cam resolution
#define MAX_RESOLUTION    3
#define WIDTH_HIGH_RES    640
#define HEIGHT_HIGH_RES   480
#define WIDTH_MED_RES     320
#define HEIGHT_MED_RES    240
#define WIDTH_LOW_RES     160
#define HEIGHT_LOW_RES    120
#define VALID_WIDTH_AND_HEIGHT(w,h) \
                          (((w) == WIDTH_HIGH_RES && (h) == HEIGHT_HIGH_RES) || \
                           ((w) == WIDTH_MED_RES && (h) == HEIGHT_MED_RES)   || \
                           ((w) == WIDTH_LOW_RES && (h) == HEIGHT_LOW_RES))
#define WIDTH(res)        ((res) == '0' ? WIDTH_LOW_RES  : (res) == '1' ? WIDTH_MED_RES  : WIDTH_HIGH_RES)
#define HEIGHT(res)       ((res) == '0' ? HEIGHT_LOW_RES : (res) == '1' ? HEIGHT_MED_RES : HEIGHT_HIGH_RES)
#define MAX_PIXELS        (WIDTH_HIGH_RES * HEIGHT_HIGH_RES)

// record / playback
#define RP_FILE_NAME              "rp.dat"
#define RP_FILE_MAGIC             0x1122334455667788LL
#define RP_FILE_FRAME_MAGIC       0x123455aa
#define RP_FILE_HDR_0_OFFSET      0
#define RP_FILE_HDR_1_OFFSET      512
#define RP_MIN_FRAME_FILE_OFFSET  1024
#define RP_MAX_FRAME_FILE_OFFSET  (rp_file_size - 1000000)
#define RP_MIN_FILE_FREE_SIZE     2000000
#define RP_MIN_FILE_SIZE          100000000
#define RP_FS_AVAIL_SIZE          100000000

#define RP_MAX_TOC                10000000

#define RP_FILE_LENGTH(frame_off) \
     ({ uint64_t end_off = rp_file_hdr.last_frame_file_offset +  \
                           rp_file_hdr.last_frame_length; \
        uint64_t frm_off = (frame_off); \
        assert(rp_file_hdr.last_frame_file_offset != 0); \
        assert(frm_off != 0); \
        if (end_off <= frm_off) { \
            end_off += rp_file_size; \
        } \
        end_off - frm_off; \
     })

// misc
#define ROUND_UP(v,n) (((uint64_t)(v) + ((n) - 1)) & ~((uint64_t)(n) - 1))
#define MB 0x100000
#define MS 1000

//
// typedefs
//

typedef struct {
    void  * addr;
    int     length;
} bufmap_t;

typedef struct frame_s {
    uint32_t             ref_count;
    bool                 motion;
    uint64_t             time_us;
    TAILQ_ENTRY(frame_s) entries;
    int32_t              buff_len;
    uint8_t              buff[0];
} frame_t;

typedef struct {
    uint64_t magic;
    uint64_t update_counter;
    uint64_t file_size;
    uint64_t last_frame_valid_through_real_time_us;
    uint64_t last_frame_file_offset;
    uint64_t last_frame_length;
    uint64_t reserved[9];
    uint32_t pad;
    uint32_t checksum;
} rp_file_hdr_t;

typedef struct {
    uint32_t magic;
    uint32_t data_len;
    uint64_t real_time_us; 
    uint64_t prior_frame_file_offset;
    bool     motion;
    uint8_t  reserved[3];
    uint32_t checksum;
    uint8_t  data[0];
} rp_frame_hdr_t;

#pragma pack(push,1)
typedef struct {
    uint64_t real_time_us;
    uint32_t file_offset_div_32;
} rp_toc_t;
#pragma pack(pop)

//
// variables
//

// cam
int                      cam_fd = -1;
bufmap_t                 bufmap[MAX_BUFMAP];
TAILQ_HEAD(th1, frame_s) proc_frame_list;
pthread_mutex_t          proc_frame_list_mutex;

// status
uint32_t cam_status;
uint32_t rp_status;

// record / playback
uint64_t          rp_file_size;
int               rp_fd = -1;
rp_file_hdr_t     rp_file_hdr;
uint32_t          rp_file_hdr_last;
rp_toc_t          rp_toc[RP_MAX_TOC];
uint32_t          rp_toc_idx_next;
uint32_t          rp_toc_idx_oldest = -1;

// cam config
config_t          config[] = { { "resolution",  "2" },
                               { "",            ""  } };

// code execution time measurement
TIMING_DECLARE(tmg_rp_read_frame_by_real_time_us,10000000);
TIMING_DECLARE(tmg_rp_read_frame_by_real_time_us_lookup,10000000);
TIMING_DECLARE(tmg_rp_write_file_hdr,10000000);
TIMING_DECLARE(tmg_rp_write_frame,10000000);

//
// prototypes
//

// init
int wc_svc_webcam_init(void);

// client support
void * wc_svc_webcam(void * cx);

// cam support
int cam_init(void);
void cam_init_webcam(char resolution);
void * cam_thread(void * cx);
void compare_gs_image(uint8_t * gs1, uint32_t gs1_w, uint32_t gs1_h, uint8_t * gs2, uint32_t gs2_w, uint32_t gs2_h,
                      bool * motion, bool * brightness);

// record/playback support
int rp_init(void);
int rp_open_file(void);
void rp_close_file(void);
void * rp_toc_init_thread(void * cx);
void * rp_write_file_frame_thread(void * cx);
int rp_read_and_verify_file_hdr(void);
int rp_write_file_hdr(void);
int rp_read_frame_by_file_offset(uint64_t file_offset, rp_frame_hdr_t * rpfh, void ** data, uint32_t * status);
int rp_read_frame_by_real_time_us(uint64_t real_time_us, rp_frame_hdr_t * rpfh, void ** data, 
        uint32_t * status, uint64_t * frame_start_real_time_us, uint64_t * frame_end_real_time_us);
int rp_write_frame(uint64_t real_time_us, void * data, int32_t data_len, bool motion);
uint32_t rp_checksum(void * data, size_t len);

// -----------------  INIT  -------------------------------------------------------------

int wc_svc_webcam_init(void)
{
    int ret;

    // init
    TAILQ_INIT(&proc_frame_list);
    pthread_mutex_init(&proc_frame_list_mutex, NULL);

    // read configuration
    ret = config_read(CONFIG_PATH, config, CONFIG_VERSION);
    if (ret < 0) {
        ERROR("config_read failed, config_path=%s\n", CONFIG_PATH);
    }
    INFO("CONFIG_RESOLUTION = '%c'\n", CONFIG_RESOLUTION);

    // initialize record/playback
    ret = rp_init();
    if (ret < 0) {
        ERROR("rp_init failed\n");
    }

    // initialize camera:
    ret = cam_init();
    if (ret < 0) {
        ERROR("cam_init failed\n");
    }

    // return success
    return 0;
}

// -----------------  SERVICE THREAD  ---------------------------------------------------

void * wc_svc_webcam(void * cx)
{
    #define SEND_MSG_FRAME(_data, _data_len, _motion, _real_time_us, _status) \
        ( { \
            int ret = 0; \
            webcam_msg_t msg; \
            msg.msg_type                = MSG_TYPE_FRAME; \
            msg.u.mt_frame.mode_id      = mode.mode_id; \
            msg.u.mt_frame.real_time_us = (_real_time_us); /* use zero in live mode */ \
            msg.u.mt_frame.data_len     = (_data_len); \
            msg.u.mt_frame.status       = (_status); \
            msg.u.mt_frame.motion       = (_motion); \
            if (p2p_send(handle, &msg, sizeof(webcam_msg_t)) < 0) { \
                ERROR("p2p_send frame hdr failed\n"); \
                ret = -1; \
            } else if (((_data_len) > 0) && (p2p_send(handle, (_data), (_data_len)) < 0)) { \
                ERROR("p2p_send frame data failed\n"); \
                ret = -1; \
            } \
            ret; \
        } )

    #define RP_DURATION \
        ((rp_file_hdr.last_frame_valid_through_real_time_us == 0 || rp_toc_idx_oldest == -1) \
         ? 0  \
         : rp_file_hdr.last_frame_valid_through_real_time_us - rp_toc[rp_toc_idx_oldest].real_time_us)

    #define REMOVE_LIVE_FRAME_REFERENCE() \
        do { \
            if (live_frame != NULL) { \
                pthread_mutex_lock(&proc_frame_list_mutex); \
                live_frame->ref_count--; \
                live_frame = NULL; \
                pthread_mutex_unlock(&proc_frame_list_mutex); \
            } \
        } while (0)


    // common variables
    int           handle                        = (long)cx;
    int           ret                           = 0;
    struct mode_s mode                          = {0,0,0,0,0};
    bool          mode_is_new                   = false;
    uint64_t      last_status_msg_send_time_us  = 0;
    uint64_t      min_send_intvl_us             = 0;
    webcam_msg_t  msg;

    // live mode variables
    frame_t     * live_frame                    = NULL;
    uint64_t      live_last_frame_send_time_us  = 0;

    // playback mode variables
    bool          stop_frame_sent               = false;
    bool          pause_frame_sent              = false;
    uint64_t      pause_frame_read_fail_time_us = 0;
    uint64_t      play_frame_start_real_time_us = 0;
    uint64_t      play_frame_end_real_time_us   = 0;
    uint64_t      play_last_frame_send_time_us  = 0;
    uint64_t      play_frame_read_fail_time_us  = 0;

    INFO("starting\n");

    while (true) {
        //
        // sleep 
        //

        usleep(10*MS);

        //
        // perform non-blocking recv to get client command
        //

        ret = p2p_recv(handle, &msg, sizeof(msg), RECV_NOWAIT_ALL);
        if (ret == RECV_ERROR) {
            ERROR("p2p_recv failed\n");
            goto done;
        }

        //
        // if we've received a client command then process the cmd
        //

        if (ret == sizeof(msg)) {
            switch (msg.msg_type) {
            case MSG_TYPE_CMD_LIVE_MODE_CHANGE_RES:
                DEBUG("received MSG_TYPE_CMD_LIVE_MODE_CHANGE_RES\n");
                CONFIG_RESOLUTION = (CONFIG_RESOLUTION == '0' ? '1' :
                                     CONFIG_RESOLUTION == '1' ? '2' 
                                                              : '0');
                CONFIG_WRITE();
                break;

            case MSG_TYPE_CMD_SET_MODE:
#ifdef DEBUG_PRINTS
                if (msg.u.mt_cmd_set_mode.mode == MODE_LIVE) {
                    INFO("received MSG_TYPE_CMD_SET_MODE - mode=%s\n", 
                          MODE_STR(msg.u.mt_cmd_set_mode.mode));
                } else {
                    char ts1[MAX_TIME_STR], ts2[MAX_TIME_STR];
                    INFO("received MSG_TYPE_CMD_SET_MODE - mode=%s %s %s speed=%f mode_entry=%s play=%s\n",
                          MODE_STR(msg.u.mt_cmd_set_mode.mode),
                          PB_SUBMODE_STR(msg.u.mt_cmd_set_mode.pb_submode),
                          PB_DIR_STR(msg.u.mt_cmd_set_mode.pb_dir),
                          msg.u.mt_cmd_set_mode.pb_speed,
                          time2str(ts1,msg.u.mt_cmd_set_mode.pb_mode_entry_real_time_us/1000000,false),
                          time2str(ts2,msg.u.mt_cmd_set_mode.pb_real_time_us/1000000,false));
                }
#endif
                mode = msg.u.mt_cmd_set_mode;
                mode_is_new = true;
                break;

            case MSG_TYPE_CMD_SET_MIN_SEND_INTVL_US:
                DEBUG("received MSG_TYPE_CMD_SET_MIN_SEND_INTVL_US - intvl=%"PRId64"\n", 
                       msg.u.mt_cmd_min_send_intvl.us);
                min_send_intvl_us = msg.u.mt_cmd_min_send_intvl.us;
                if (min_send_intvl_us > 1000000) {
                    min_send_intvl_us = 1000000;
                }
                break;

            default:
                ERROR("invalid msg_type %d\n", msg.msg_type);
                goto done;
            }
        }

        //
        // send status msg once per second
        //

        if (microsec_timer() - last_status_msg_send_time_us > 1000000) {
            p2p_stats_t p2p_stats;

            p2p_get_stats(handle, &p2p_stats);
            msg.msg_type = MSG_TYPE_STATUS;
            msg.u.mt_status.version         = VERSION;
            msg.u.mt_status.cam_status      = cam_status;
            msg.u.mt_status.rp_status       = rp_status;
            msg.u.mt_status.rp_duration_us  = RP_DURATION;
            msg.u.mt_status.p2p_resend_cnt  = p2p_stats.resent_data_dgrams;
            msg.u.mt_status.p2p_recvdup_cnt = p2p_stats.peer_recvd_duplicates;
            if (p2p_send(handle, &msg, sizeof(webcam_msg_t)) < 0) {
                ERROR("p2p_send status failed\n");
                goto done;
            }
            last_status_msg_send_time_us = microsec_timer();
        }

        //
        // MODE_LIVE support 
        //

        if (mode.mode == MODE_LIVE) {
            uint64_t curr_us;

            // if just entered this mode then init
            if (mode_is_new) {
                REMOVE_LIVE_FRAME_REFERENCE();
                live_frame                    = NULL;
                live_last_frame_send_time_us  = 0;
                mode_is_new                   = false;
            }

            // if too soon to send then continue
            curr_us = microsec_timer();
            if (curr_us - live_last_frame_send_time_us < min_send_intvl_us) {
                continue;
            }
            live_last_frame_send_time_us = curr_us;

            // attempt to get new live_frame from proc_frame_list, if none then continue;
            pthread_mutex_lock(&proc_frame_list_mutex);
            if (TAILQ_EMPTY(&proc_frame_list) || live_frame == TAILQ_LAST(&proc_frame_list,th1)) {
                pthread_mutex_unlock(&proc_frame_list_mutex);
                continue;
            } else if (live_frame == NULL) {
                live_frame = TAILQ_LAST(&proc_frame_list,th1);
                live_frame->ref_count++;
            } else {
                live_frame->ref_count--;
                live_frame = TAILQ_NEXT(live_frame,entries);
                live_frame->ref_count++;
            }
            pthread_mutex_unlock(&proc_frame_list_mutex);

            // send the frame
            if (SEND_MSG_FRAME(live_frame->buff, live_frame->buff_len, live_frame->motion, 0, STATUS_INFO_OK) < 0) {
                goto done;
            }

        //
        // MODE_PLAYBACK support 
        //

        } else if (mode.mode == MODE_PLAYBACK) {

            //
            // if just entered this mode then init
            //

            if (mode_is_new) {
                REMOVE_LIVE_FRAME_REFERENCE();
                stop_frame_sent               = false;
                pause_frame_sent              = false;
                pause_frame_read_fail_time_us = 0;
                play_frame_start_real_time_us = 0;
                play_frame_end_real_time_us   = 0;
                play_last_frame_send_time_us  = 0;
                play_frame_read_fail_time_us  = 0;
                mode_is_new                   = false;
            }

            //
            // PB_SUBMODE_STOP
            //

            if (mode.pb_submode == PB_SUBMODE_STOP) {
                // if the stop frame has already been sent then we're done
                if (stop_frame_sent) {
                    continue;
                }

                // send the stop frame
                if (SEND_MSG_FRAME(NULL, 0, false, 0, STATUS_INFO_STOPPED) < 0) {
                    goto done;
                }
                stop_frame_sent = true;

            //
            // PB_SUBMODE_PAUSE
            //

            } else if (mode.pb_submode == PB_SUBMODE_PAUSE) {
                rp_frame_hdr_t   rpfh;
                void           * data;
                bool             motion;
                uint32_t         status;
                int              ret;
                uint64_t         frame_real_time_us;

                // if the pause frame has already been sent then we're done
                if (pause_frame_sent) {
                    continue;
                }

                // if prior attempt to read the pause frame has failed with the past 1 second then continue
                if (pause_frame_read_fail_time_us != 0 &&
                    microsec_timer() - pause_frame_read_fail_time_us < 1000000) 
                {
                    continue;
                }

                // determine the desired frame time
                frame_real_time_us = mode.pb_real_time_us;

                // read the frame at the specified playback time
                ret = rp_read_frame_by_real_time_us(frame_real_time_us, &rpfh, &data, &status, 
                                                    &play_frame_start_real_time_us, &play_frame_end_real_time_us);

                // send the frame
                if (ret < 0) {
                    if (pause_frame_read_fail_time_us == 0) {
                        if (SEND_MSG_FRAME(NULL, 0, false, frame_real_time_us, status) < 0) {
                            goto done;
                        }
                    }
                    pause_frame_read_fail_time_us = microsec_timer();
                } else {
                    motion = (rpfh.motion) && 
                             (frame_real_time_us - play_frame_start_real_time_us < 1000000);
                    if (SEND_MSG_FRAME(data, rpfh.data_len, motion, frame_real_time_us, status) < 0) {
                        free(data);
                        goto done;
                    }
                    free(data);
                }

                // if status is AFTER_EOD then we'll continue to try at second intervals, 
                // otherwise we are done
                if (status != STATUS_ERR_FRAME_AFTER_EOD) {
                    pause_frame_sent = true;
                }

            //
            // PB_SUBMODE_PLAY
            //

            } else if (mode.pb_submode == PB_SUBMODE_PLAY) {
                rp_frame_hdr_t   rpfh;
                void           * data;
                bool             motion;
                uint32_t         status;
                int              ret;
                uint64_t         frame_real_time_us;
                uint64_t         curr_us;

                // determine the desired frame time
                frame_real_time_us = PB_SUBMODE_PLAY_REAL_TIME_US(&mode);

                // if the playback frame that we have last read covers the frame time then contineu
                if (frame_real_time_us >= play_frame_start_real_time_us && 
                    frame_real_time_us <= play_frame_end_real_time_us)
                {
                    continue;
                }

                // if prior attempt to read the frame has failed with the past 1 second then continue
                curr_us = microsec_timer();
                if (play_frame_read_fail_time_us != 0 &&
                    curr_us - play_frame_read_fail_time_us < 1000000) 
                {
                    continue;
                }

                // if it is too soon to send another frame then continue
                if (curr_us - play_last_frame_send_time_us < min_send_intvl_us) {
                    continue;
                }
                play_last_frame_send_time_us = curr_us;

                // read the frame
                ret = rp_read_frame_by_real_time_us(frame_real_time_us, &rpfh, &data, &status,
                                                    &play_frame_start_real_time_us, &play_frame_end_real_time_us);

                // send the frame
                if (ret < 0) {
                    play_frame_read_fail_time_us = microsec_timer();
                    if (SEND_MSG_FRAME(NULL, 0, false, frame_real_time_us, status) < 0) {
                        goto done;
                    }
                } else {
                    play_frame_read_fail_time_us = 0;
                    motion = (rpfh.motion) && 
                             (frame_real_time_us - play_frame_start_real_time_us < 1000000);
                    if (SEND_MSG_FRAME(data, rpfh.data_len, motion, frame_real_time_us, status) < 0) {
                        free(data);
                        goto done;
                    }
                    free(data);
                }

            //
            // PB_SUBMODE is INVALID
            //

            } else {
                ERROR("pb_submode %d is invalid\n", mode.pb_submode);
                goto done;
            }

        //
        // MODE_NONE support (the mode has not been set yet)
        //

        } else if (mode.mode == MODE_NONE) {
            if (mode_is_new) {
                REMOVE_LIVE_FRAME_REFERENCE();
                mode_is_new = false;
            }

        //
        // MODE is INVALID, disconnect
        //

        } else {
            ERROR("mode %d is invalid\n", mode.mode);
            goto done;
        }
    }

done:
    REMOVE_LIVE_FRAME_REFERENCE();
    p2p_disconnect(handle);
    INFO("terminating\n");
    return NULL;
}

// -----------------  CAMERA  -----------------------------------------------------------

int cam_init(void)
{
    pthread_t thread;

    // create cam_thread
    pthread_create(&thread, NULL, cam_thread, NULL);

    // return success
    return 0;
}

void cam_init_webcam(char resolution)
{
    struct v4l2_capability     cap;
    struct v4l2_cropcap        cropcap;
    struct v4l2_crop           crop;
    struct v4l2_format         format;
    struct v4l2_streamparm     streamparm;
    struct v4l2_requestbuffers reqbuf;
    struct v4l2_buffer         buffer;
    enum   v4l2_buf_type       buf_type;
    int                        i;
    bool                       first_try = true;

    INFO("starting, resolution=%c\n", resolution);

try_again:
    // if not first try then delay
    if (!first_try) {
        INFO("sleep and retry\n");
        cam_status = STATUS_ERR_WEBCAM_FAILURE;
        usleep(10000*MS);  // 10 secs
    }
    first_try = false;

    // if already initialized, then perform uninitialize
    if (cam_fd > 0) {
        close(cam_fd);
        for (i = 0; i < MAX_BUFMAP; i++) {
            if (bufmap[i].addr != NULL) {
                munmap(bufmap[i].addr, bufmap[i].length);
                bufmap[i].addr = NULL;
                bufmap[i].length = 0;
            }
        }
        cam_fd = -1;
        bzero(bufmap,sizeof(bufmap));
    }

    // open webcam
    for (i = 0; i < 2; i++) {
        char devpath[100];
        sprintf(devpath, "%s%d", WC_VIDEO, i);
        cam_fd = open(devpath, O_RDWR|O_CLOEXEC|O_NONBLOCK);
        if (cam_fd < 0) {
            ERROR("open failed %s %s\n",  devpath, strerror(errno));
        } else {
            INFO("open success %s\n", devpath);
            break;
        }
    }
    if (cam_fd < 0) {
        goto try_again;
    }

    // get and verify capability
    if (ioctl(cam_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ERROR("ioctl VIDIOC_QUERYCAP %s\n", strerror(errno));
        goto try_again;
    }

    // verify capabilities
    if ((cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0) {
        ERROR("no cap V4L2_CAP_VIDEO_CAPTURE\n");
        goto try_again;
    }
    if ((cap.capabilities & V4L2_CAP_STREAMING) == 0) {
        ERROR("no cap V4L2_CAP_STREAMING\n");
        goto try_again;
    }

    // get VIDEO_CAPTURE format type, and
    // set pixel format to (MJPEG,width,height)
    bzero(&format, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_G_FMT, &format) < 0) {
        ERROR("ioctl VIDIOC_G_FMT %s\n", strerror(errno));
        goto try_again;
    }
    INFO("setting resolution to %dx%d\n", WIDTH(resolution), HEIGHT(resolution));
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.width  =  WIDTH(resolution);
    format.fmt.pix.height =  HEIGHT(resolution);
    if (ioctl(cam_fd, VIDIOC_S_FMT, &format) < 0) {
        ERROR("ioctl VIDIOC_S_FMT %s\n", strerror(errno));
        goto try_again;
    }

    // get crop capabilities
    bzero(&cropcap,sizeof(cropcap));
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_CROPCAP, &cropcap) < 0) {
        ERROR("ioctl VIDIOC_CROPCAP, %s\n", strerror(errno));
        goto try_again;
    }

    // set crop to default 
    bzero(&crop, sizeof(crop));
    crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    crop.c = cropcap.defrect;
    if (ioctl(cam_fd, VIDIOC_S_CROP, &crop) < 0) {
        if (errno == EINVAL || errno == ENOTTY) {
            INFO("crop not supported\n");
        } else {
            ERROR("ioctl VIDIOC_S_CROP, %s\n", strerror(errno));
            goto try_again;
        }
    }

    // set frames per sec
    bzero(&streamparm, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    streamparm.parm.capture.timeperframe.numerator   = 1;
    streamparm.parm.capture.timeperframe.denominator = FRAMES_PER_SEC;
    if (ioctl(cam_fd, VIDIOC_S_PARM, &streamparm) < 0) {
        ERROR("ioctl VIDIOC_S_PARM, %s\n", strerror(errno));
        goto try_again;
    }

    // request memory mapped buffers
    bzero(&reqbuf, sizeof(reqbuf));
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = MAX_BUFMAP;
    if (ioctl (cam_fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
        ERROR("ioctl VIDIOC_REQBUFS %s\n", strerror(errno));
        goto try_again;
    }

    // verify we got all the frames requested
    if (reqbuf.count != MAX_BUFMAP) {
        ERROR("got wrong number of frames, requested %d, actual %d\n",
              MAX_BUFMAP, reqbuf.count);
        goto try_again;
    }

    // memory map each of the buffers
    for (i = 0; i < MAX_BUFMAP; i++) {
        bzero(&buffer,sizeof(struct v4l2_buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;
        if (ioctl (cam_fd, VIDIOC_QUERYBUF, &buffer) < 0) {
            ERROR("ioctl VIDIOC_QUERYBUF index=%d %s\n", i, strerror(errno));
            goto try_again;
        }
        bufmap[i].addr = mmap(NULL, buffer.length,
                              PROT_READ | PROT_WRITE,
                              MAP_SHARED,           
                              cam_fd, buffer.m.offset);
        bufmap[i].length = buffer.length;

        if (bufmap[i].addr == MAP_FAILED) {
            ERROR("mmap failed, %s\n", strerror(errno));
            goto try_again;
        }
    }

    // give the buffers to driver
   for (i = 0; i < MAX_BUFMAP; i++) {
        bzero(&buffer,sizeof(struct v4l2_buffer));
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index  = i;
        if (ioctl(cam_fd, VIDIOC_QBUF, &buffer) < 0) {
            ERROR("ioctl VIDIOC_QBUF index=%d %s\n", i, strerror(errno));
            goto try_again;
        }
    }

    // enable capture
    buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMON, &buf_type) < 0) {
        ERROR("ioctl VIDIOC_STREAMON %s\n", strerror(errno));
        goto try_again;
    }

    // return success
    INFO("success\n");
    cam_status = STATUS_INFO_OK;
}

void * cam_thread(void * cx)
{
    char               curr_resolution = 'N';
    bool               cam_init_needed = true;

    struct v4l2_buffer buffer;
    uint64_t           last_frame_check_ok_time_us;
    uint64_t           recvd_frame_count;
    uint64_t           last_proc_frame_count;
    uint64_t           last_motion_frame_count;
    frame_t          * new_frame;
    frame_t          * new_frame_array[FRAME_COMPARE_INTERVAL];
    uint32_t           nfa_idx;
    frame_t          * proc_frame_array[FRAME_COMPARE_INTERVAL];
    uint32_t           pfa_idx;
    uint8_t          * recvd_gs;
    uint32_t           recvd_gs_w;
    uint32_t           recvd_gs_h;
    uint8_t          * curr_gs;
    uint32_t           curr_gs_w;
    uint32_t           curr_gs_h;

    #define INIT_LOCAL_VARS() \
        do { \
            bzero(&buffer, sizeof(buffer)); \
            last_frame_check_ok_time_us = 0; \
            recvd_frame_count           = 0; \
            last_proc_frame_count       = 0; \
            last_motion_frame_count     = 0; \
            new_frame                   = NULL; \
            bzero(new_frame_array, sizeof(new_frame_array)); \
            nfa_idx                     = 0; \
            bzero(proc_frame_array, sizeof(proc_frame_array)); \
            pfa_idx                     = FRAME_COMPARE_INTERVAL; \
            recvd_gs                    = NULL; \
            recvd_gs_w                  = 0; \
            recvd_gs_h                  = 0; \
            curr_gs                     = NULL; \
            curr_gs_w                   = 0; \
            curr_gs_h                   = 0; \
        } while (0)

    #define FREE_LOCAL_VARS() \
        do { \
            int i; \
            if (new_frame) { \
                free(new_frame); \
                new_frame = NULL; \
            } \
            for (i = 0; i < FRAME_COMPARE_INTERVAL; i++) { \
                if (new_frame_array[i]) { \
                    free(new_frame_array[i]); \
                    new_frame_array[i] = NULL; \
                } \
            } \
            for (i = 0; i < FRAME_COMPARE_INTERVAL; i++) { \
                if (proc_frame_array[i]) { \
                    free(proc_frame_array[i]); \
                    proc_frame_array[i] = NULL; \
                } \
            } \
            if (recvd_gs) { \
                free(recvd_gs); \
                recvd_gs = NULL; \
            } \
            if (curr_gs) { \
                free(curr_gs); \
                curr_gs = NULL; \
            } \
        } while (0)

    #define IOCTL_VIDIOC_QBUF() \
        do { \
            buffer.flags = 0; \
            if (ioctl(cam_fd, VIDIOC_QBUF, &buffer) < 0) { \
                ERROR("ioctl VIDIOC_QBUF %s\n", strerror(errno));  \
                cam_status = STATUS_ERR_WEBCAM_FAILURE; \
                goto cam_failure; \
            } \
        } while (0)

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // initialize local vars
    INIT_LOCAL_VARS();

    // loop forever
    while (true) {
        // process settings changes
        if (cam_init_needed || CONFIG_RESOLUTION != curr_resolution) {
            char res = CONFIG_RESOLUTION;

            FREE_LOCAL_VARS();
            INIT_LOCAL_VARS();

            cam_init_webcam(res);

            curr_resolution = res;
            cam_init_needed = false;
        }

        // read a frame
        int count = 0;
        while (true) {
            bzero(&buffer, sizeof(buffer));
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (ioctl(cam_fd, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN && count++ < 50) {
                    usleep(50*MS);
                    continue;
                }
                ERROR("ioctl VIDIOC_DQBUF failed, count=%d, %s\n", count, strerror(errno));
                cam_status = STATUS_ERR_WEBCAM_FAILURE;
                goto cam_failure;
            }
            break;
        }

        // if error flag is set then requeue the buffer and continue
        if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
            WARN("V4L2_BUF_FLAG_ERROR is set, flags=0x%x\n", buffer.flags);
            IOCTL_VIDIOC_QBUF();
            continue;
        }

        // frame check - it is error if:
        // - time has gone backward
        // - time jumps forward more than 10 seconds
        uint64_t frame_time_us = TIMEVAL_TO_US(&buffer.timestamp);
        if ((last_frame_check_ok_time_us > 0) &&
            ((frame_time_us <= last_frame_check_ok_time_us) ||
             (frame_time_us > last_frame_check_ok_time_us + 10000000)))
        {
            ERROR("frame sanity check failed - time out of order, rfc=%"PRId64" curr=%d.%6.6d last=%d.%6.6d\n",
                  recvd_frame_count,
                  (int)(frame_time_us / 1000000),
                  (int)((frame_time_us % 1000000)),
                  (int)(last_frame_check_ok_time_us / 1000000),
                  (int)((last_frame_check_ok_time_us % 1000000)));
            cam_status = STATUS_ERR_FRAME_TIME;
            goto cam_failure;
        }
        last_frame_check_ok_time_us = frame_time_us;

        // allocate and init a new_frame, 
        // add it to the new_frame_array, and
        // increment the count of number frames recvd
        new_frame = malloc(sizeof(frame_t) + buffer.bytesused);
        if (new_frame == NULL) {
            ERROR("malloc frame failed, bytesused=%d\n", buffer.bytesused);
            cam_status = STATUS_ERR_FRAME_DATA_MEM_ALLOC;
            goto cam_failure;
        }
        new_frame->ref_count = 0;
        new_frame->motion    = false;
        new_frame->time_us   = frame_time_us;
        new_frame->buff_len  = buffer.bytesused;
        memcpy(new_frame->buff, bufmap[buffer.index].addr, buffer.bytesused);
        new_frame_array[nfa_idx++] = new_frame;
        new_frame = NULL;
        recvd_frame_count++;

        // requeue the buffer to the driver
        IOCTL_VIDIOC_QBUF();

        // if new_frame_array is full
        if (nfa_idx == FRAME_COMPARE_INTERVAL) {
            bool motion, brightness;
            int  ret, i;

            // assert that the proc_frame_array is empty
            assert(pfa_idx == FRAME_COMPARE_INTERVAL);

            // reset nfa_idx
            nfa_idx = 0;

            // convert the recvd jpeg to recvd_gs
            ret = jpeg_decode(
                    0, JPEG_DECODE_MODE_GS, 
                    new_frame_array[FRAME_COMPARE_INTERVAL-1]->buff, new_frame_array[FRAME_COMPARE_INTERVAL-1]->buff_len,
                    &recvd_gs, &recvd_gs_w, &recvd_gs_h);
            if (ret != 0 || !VALID_WIDTH_AND_HEIGHT(recvd_gs_w,recvd_gs_h)) {
                ERROR("jpeg_decode ret=%d width=%d height=%d\n", ret, recvd_gs_w, recvd_gs_h);
            }

            // compare recvd_gs with curr_gs
            compare_gs_image(recvd_gs, recvd_gs_w, recvd_gs_h,
                             curr_gs, curr_gs_w, curr_gs_h,
                             &motion, &brightness);

            // if motion detected within the past 1 seconds then 
            // add all FRAME_COMPARE_INTERVAL entries in new_frame_array to the proc_frame_array
            if (motion) {
                last_motion_frame_count = recvd_frame_count;
            }
            if (recvd_frame_count - last_motion_frame_count <= 10) {
                for (i = 0; i < FRAME_COMPARE_INTERVAL; i++) {
                    new_frame_array[i]->motion = motion;
                    proc_frame_array[i] = new_frame_array[i];
                    new_frame_array[i] = NULL;
                }
                pfa_idx = 0;

                free(curr_gs);
                curr_gs = recvd_gs;
                curr_gs_w = recvd_gs_w;
                curr_gs_h = recvd_gs_h;

                recvd_gs = NULL;
                recvd_gs_w = 0;
                recvd_gs_h = 0;

                last_proc_frame_count = recvd_frame_count;

            // else if brightness has changed or haven't sent a frame in past 50 recvd 
            //      then add just 1 entry from the new_frame_array to the proc_frame_array, and free others
            } else if (brightness || (recvd_frame_count > last_proc_frame_count + 50)) {
                proc_frame_array[FRAME_COMPARE_INTERVAL-1] = new_frame_array[FRAME_COMPARE_INTERVAL-1];
                new_frame_array[FRAME_COMPARE_INTERVAL-1] = NULL;
                for (i = 0; i < FRAME_COMPARE_INTERVAL-1; i++) {
                    free(new_frame_array[i]);
                    new_frame_array[i] = NULL;
                }
                pfa_idx = FRAME_COMPARE_INTERVAL-1;

                free(curr_gs);
                curr_gs = recvd_gs;
                curr_gs_w = recvd_gs_w;
                curr_gs_h = recvd_gs_h;

                recvd_gs = NULL;
                recvd_gs_w = 0;
                recvd_gs_h = 0;

                last_proc_frame_count = recvd_frame_count;

            // else free all new_frame_array entries
            } else {
                for (i = 0; i < FRAME_COMPARE_INTERVAL; i++) {
                    free(new_frame_array[i]);
                    new_frame_array[i] = NULL;
                }

                free(recvd_gs);
                recvd_gs = NULL;
                recvd_gs_w = 0;
                recvd_gs_h = 0;
            }
        }

        // acquire process_frames_list_mutex
        pthread_mutex_lock(&proc_frame_list_mutex);

        // if there are frames in the proc_frame_array then add the oldest to the proc_frame_list
        if (pfa_idx < FRAME_COMPARE_INTERVAL) {
            TAILQ_INSERT_TAIL(&proc_frame_list, proc_frame_array[pfa_idx], entries);
            proc_frame_array[pfa_idx] = NULL;
            pfa_idx++;
        }

        // remove entries in the process_frames_list that have timestamps older than 2 secs
        // and which are not currently being used
        uint64_t curr_us = microsec_timer();
        frame_t * frame = TAILQ_FIRST(&proc_frame_list);
        frame_t * next;
        while (frame != NULL) {
            if (curr_us - frame->time_us < 2000000) {
                break;
            }
            next = TAILQ_NEXT(frame, entries);
            if (frame->ref_count == 0) {
                TAILQ_REMOVE(&proc_frame_list, frame, entries);
                free(frame);
            }
            frame = next;
        }

        // release process_frames_list_mutex
        pthread_mutex_unlock(&proc_frame_list_mutex);

        // continue
        continue;

        // jump here when a webcam error occurs
cam_failure:
        cam_init_needed = true;
    }

    // thread exit
    return NULL;
}

void compare_gs_image(uint8_t * gs1, uint32_t gs1_w, uint32_t gs1_h, uint8_t * gs2, uint32_t gs2_w, uint32_t gs2_h,
                      bool * motion, bool * brightness)
{
    #define MAX_BOX_X  8
    #define MAX_BOX_Y  6

    uint32_t width, height;
    uint32_t box[MAX_BOX_Y][MAX_BOX_X];
    uint32_t x, y, box_x, box_y, box_w, box_h, gs1_pixel_sum, gs2_pixel_sum, idx;
    uint32_t box10cnt, box5cnt, box3cnt;
    double   brightness_diff, gs1_pixel_avg, gs2_pixel_avg;

    // preset returns
    *motion = false;
    *brightness = false;

    // if either gs1 or gs2 don't exist or the dimensions are different then 
    // return motion change flag
    if (gs1 == NULL || gs2 == NULL || gs1_w != gs2_w || gs1_h != gs2_h) {
        *motion = true;
        return;
    }

    // init local vars
    bzero(box, sizeof(box));
    width         = gs1_w;
    height        = gs1_h;
    box_w         = width / MAX_BOX_X;
    box_h         = height / MAX_BOX_Y;
    gs1_pixel_sum = 0;
    gs2_pixel_sum = 0;
    box10cnt      = 0;
    box5cnt       = 0;
    box3cnt       = 0;

    // compare gs1 with gs2, detect change due to motion or brightness ...

    // loop serves two purposes
    // a) sum the intensity of all pixels, and compute pixel_avg
    // b) compute the number of pixels different in each box in the
    //    MAX_BOX_X x MAX_BOX_Y grid which overlays the image
    idx = 0;
    for (y = 0; y < height; y++) {
        box_y = y / box_h;
        for (x = 0; x < width; x++) {
            gs1_pixel_sum += gs1[idx];
            gs2_pixel_sum += gs2[idx];
            if (abs(gs2[idx] - gs1[idx]) > 30) {
                box[box_y][x/box_w]++;
            }
            idx++;
        }
    }
    gs1_pixel_avg = (double)gs1_pixel_sum / (width*height);
    gs2_pixel_avg = (double)gs2_pixel_sum / (width*height);

    // convert box to percent pixel diff
    for (box_y = 0; box_y < MAX_BOX_Y; box_y++) {
        for (box_x = 0; box_x < MAX_BOX_X; box_x++) {
            box[box_y][box_x] = box[box_y][box_x] * 100 / (box_w * box_h);
        }
    }

    // count number of boxes with greater than 10%, 5%, and 3% pixels different
    for (box_y = 0; box_y < MAX_BOX_Y; box_y++) {
        for (box_x = 0; box_x < MAX_BOX_X; box_x++) {
            if (box[box_y][box_x] >= 10) {
                box10cnt++;
            } 
            if (box[box_y][box_x] >= 5) {
                box5cnt++;
            } 
            if (box[box_y][box_x] >= 3) {
                box3cnt++;
            } 
        }
    }

    // set motion, will be true if ...
    // - at least one box has 10% pixels different
    // - at least two boxex have 5% pixels different
    // - at least three boxex have 3% pixels different
    *motion = (box10cnt >= 1 || box5cnt >= 2 || box3cnt >= 3);

#if 0  
    // enable this to simulate continuous motion
    *motion = true;
#endif

    // determine pixel average brightness difference;
    // set brightness_change_detected if  pixel average has changed by 5
    brightness_diff = gs2_pixel_avg - gs1_pixel_avg;
    if (brightness_diff < 0.0) {
        brightness_diff = -brightness_diff;
    }
    *brightness = (brightness_diff >= 5);

#if 0
    // print the box, ...
    if (*motion) {
        INFO("-------- motion %d  brightness %d --------\n", *motion, *brightness);
        for (box_y = 0; box_y < MAX_BOX_Y; box_y++) {
            INFO("%4d %4d %4d %4d %4d %4d %4d %4d \n",
                   box[box_y][0], box[box_y][1], box[box_y][2], box[box_y][3],
                   box[box_y][4], box[box_y][5], box[box_y][6], box[box_y][7]);
        }
        INFO("width=%d height=%d box_w=%d box_h=%d\n",
               width, height, box_w, box_h);
        INFO("MOTION_DETECTED - box10cnt=%d box5cnt=%d box3cnt=%d\n", 
               box10cnt, box5cnt, box3cnt);
    }
    if (*brightness) {
        INFO("BRIGHTNESS_CHANGE_DETECTED: brightness_diff=%.1f\n",
               brightness_diff);
    }
#endif
}

// -----------------  RECORD / PLAYBACK -------------------------------------------------

// - - - - - - - - -  RP INIT / OPEN / CLOSE   - - - - - - - - - - - - - - 

int rp_init(void)
{
    pthread_t thread;

    // debug print size of key types and structs
    DEBUG("SIZEOF OFF_T          %d\n", (int)sizeof(off_t));
    DEBUG("SIZE OF ST_SIZE       %d\n", (int)sizeof(((struct stat *)0)->st_size));
    DEBUG("SIZE OF RP_FILE_HDR   %d\n", (int)sizeof(rp_file_hdr_t));
    DEBUG("SIZE OF RP_FRAME_HDR  %d\n", (int)sizeof(rp_frame_hdr_t));
    DEBUG("SIZE OF RP_TOC        %d MB\n", (int)(sizeof(rp_toc) / 0x100000));  

    // asserts
    assert(sizeof(off_t) == 8);
    assert(sizeof(((struct stat *)0)->st_size) == 8);
    assert(sizeof(rp_file_hdr_t) == 128);
    assert(sizeof(rp_frame_hdr_t) == 32);

    // open (or create) the record/playback file
    if (rp_open_file() < 0) {
        ERROR("rp_open_file failed\n");
        return -1;
    }

    // register exit handler to close file when program exits
    atexit(rp_close_file);

    // create threads:
    // - rp_toc_init_thread
    // - rp_write_file_frame_thread
    pthread_create(&thread, NULL, rp_toc_init_thread, 
                   (void*)(uintptr_t)(rp_file_hdr.last_frame_file_offset/32));
    pthread_create(&thread, NULL, rp_write_file_frame_thread, NULL);

    // return success
    return 0;
}

int rp_open_file(void) 
{
    int ret;
    uint64_t avail_bytes;
    char dirpath[100];

    // open file for read/write, and verify file header;
    // note that rp_read_and_verify_file_hdr sets rp_file_size
    rp_fd = open(RP_FILE_NAME, O_RDWR|O_CLOEXEC);
    if (rp_fd > 0) {
        ret = rp_read_and_verify_file_hdr();
        if (ret == 0) {
            INFO("using existing file %s, size=%"PRId64"\n", RP_FILE_NAME, rp_file_size);
            return 0;
        }
        close(rp_fd);
        rp_fd = -1;
        unlink(RP_FILE_NAME);
        ERROR("invalid file hdr, new file will be created\n");
    }

    // create and init file ...

    // determine rp_file_size;
    // - leave RP_FS_AVAIL_SIZE available after file is created
    // - minimum size of file is RP_MIN_FILE_SIZE
    // - size is rounded to multible of 1000000
    strcpy(dirpath, RP_FILE_NAME);
    avail_bytes = fs_avail_bytes(dirname(dirpath));
    if (avail_bytes < RP_FS_AVAIL_SIZE + RP_MIN_FILE_SIZE) {
        ERROR("insufficent space available to create %s, avail=%"PRId64" MB\n",
              RP_FILE_NAME, avail_bytes/MB);
        return -1;
    }
    rp_file_size = (avail_bytes - RP_FS_AVAIL_SIZE);
    rp_file_size -= (rp_file_size % 1000000);

    // create empty file
    rp_fd = open(RP_FILE_NAME, O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC, 0644);
    if (rp_fd < 0) {
        ERROR("failed to create %s, %s\n", RP_FILE_NAME, strerror(errno));
        return -1;
    }

    // call fallocate to preallocate space to the file
    ret = fallocate(rp_fd, 0, 0, rp_file_size);
    if (ret < 0) {
        ERROR("failed to allocate size %"PRId64", %s\n", rp_file_size, strerror(errno));
        close(rp_fd);
        rp_fd = -1;
        return -1;
    }

    // init non zero fields of rp_file_hdr
    rp_file_hdr.magic = RP_FILE_MAGIC;
    rp_file_hdr.file_size = rp_file_size;

    // write the file hdr
    ret = rp_write_file_hdr();
    if (ret < 0) {
        ERROR("failed to write file hdr %s, %s\n", RP_FILE_NAME, strerror(errno));
        close(rp_fd);
        rp_fd = -1;
        return -1;
    }

    // return success
    INFO("created new file %s, size=%"PRId64"\n", RP_FILE_NAME, rp_file_size);
    return 0;
}

void rp_close_file(void)
{
    // sync and close file
    if (rp_fd > 0) {
        INFO("syncing and closing\n");
        fsync(rp_fd);
        close(rp_fd);
    }
}

// - - - - - - - - -  RP THREADS  - - - - - - - - - - - - - - - - - - - - 

void * rp_toc_init_thread(void * cx)
{
    uint64_t       frame_file_offset;
    uint64_t       last_frame_real_time_us;
    uint32_t       idx;
    uint32_t       toc_init_count;
    uint64_t       run_start_time_us;
    uint32_t       status;
    rp_frame_hdr_t rpfh;
    char           ts1[MAX_TIME_STR], ts2[MAX_TIME_STR];

    // starting notice
    INFO("starting\n");

    frame_file_offset       = (uint64_t)(uintptr_t)cx * 32;
    last_frame_real_time_us = -1;
    idx                     = RP_MAX_TOC-1;
    toc_init_count          = 0;
    run_start_time_us       = microsec_timer();

    while (true) {
        // if frame_file_offset is 0 then we've progressed back to beyond the 1st frame
        if (frame_file_offset == 0) {
            INFO("terminating because no more frames\n");
            break;
        }

        // read and verify frame hdr
        if (rp_read_frame_by_file_offset(frame_file_offset, &rpfh, NULL, &status) < 0) {
            INFO("terminating becasue read frame failed\n");
            break;
        }

        // if frame time is not less than our last frame then done
        if (rpfh.real_time_us >= last_frame_real_time_us) {
            INFO("terminating becasue frame real time has increased, last=%s.%3.3d curr=%s.%3.3d\n",
                   time2str(ts1, last_frame_real_time_us/1000000, false),
                   (int)((last_frame_real_time_us%1000000)/1000),
                   time2str(ts2, rpfh.real_time_us/1000000, false),
                   (int)((rpfh.real_time_us%1000000)/1000));
            break;
        }
        last_frame_real_time_us = rpfh.real_time_us;

        // if getting too close to the new toc entries being added by the
        // rp_write_file_frame_thread then we're done
        if (idx <= rp_toc_idx_next + 100) {
            INFO("terminating becasue idx too close %d %d\n", idx, rp_toc_idx_next);
            break;
        }

        // if getting too close to the new file data being added by the
        // rp_write_file_frame_thread then we're done
        if (rp_file_size - RP_FILE_LENGTH(frame_file_offset) < RP_MIN_FILE_FREE_SIZE) {
            INFO("terminating becasue close to file wrap, file_len=%"PRId64"\n",
                   RP_FILE_LENGTH(frame_file_offset));
            break;
        }

        // init the toc entry
        rp_toc[idx].real_time_us       = rpfh.real_time_us;
        rp_toc[idx].file_offset_div_32 = frame_file_offset / 32;
        rp_toc_idx_oldest = idx;
        idx--;
        toc_init_count++;

        // update frame_file_offset to the prior frame
        frame_file_offset = rpfh.prior_frame_file_offset;

        // sleep periodically to give the other threads a chance to run
        if (microsec_timer() - run_start_time_us > 50*MS) {
            usleep(50*MS);
            run_start_time_us = microsec_timer();
        }
    }

    // print notice
    INFO("initialized %d toc entries\n", toc_init_count);
    if (toc_init_count > 0) {
        INFO("    %s  ->  %s\n",
             time2str(ts1, rp_toc[RP_MAX_TOC-toc_init_count].real_time_us/1000000, false),
             time2str(ts2, rp_toc[RP_MAX_TOC-1].real_time_us/1000000, false));
    }

    // thread exit
    INFO("terminating\n");
    return NULL;
}

void * rp_write_file_frame_thread(void * cx)
{
    uint64_t  last_file_hdr_write_time_us;
    uint64_t  last_frame_written_real_time_us;
    bool      last_frame_is_gap;
    uint64_t  real_minus_monotonic_time_us;

    uint64_t  this_frame_real_time_us;
    char      ts1[MAX_TIME_STR], ts2[MAX_TIME_STR];
    int       ret, loop_count;

    frame_t * record_frame = NULL;

    // starting notice
    INFO("starting\n");

    // init
    pthread_detach(pthread_self());
    last_file_hdr_write_time_us     = 0;
    last_frame_written_real_time_us = rp_file_hdr.last_frame_valid_through_real_time_us;
    last_frame_is_gap               = false;

    // wait for system clock to be set
    rp_status = STATUS_ERR_SYSTEM_CLOCK_NOT_SET;
    loop_count = 0;
    while (true) {
        if (ntp_synced()) { 
            break;
        }
        sleep(loop_count++ < 60 ? 1 : 60);
    }
    rp_status = STATUS_INFO_OK;

    // get delta value between realtime and monotonic clocks
    real_minus_monotonic_time_us = get_real_time_us() - microsec_timer();

    while (true) {
        // sleep
        usleep(10*MS);

        // if there is a prior frame and 
        //    it has been more than 10 second since last frame and
        //    the last frame is not a gap frame
        // then
        //    write gap frame
        // endif
        if (rp_file_hdr.last_frame_valid_through_real_time_us != 0 &&
            get_real_time_us() > rp_file_hdr.last_frame_valid_through_real_time_us + 10000000 && 
            !last_frame_is_gap) 
        {
            this_frame_real_time_us = rp_file_hdr.last_frame_valid_through_real_time_us+1;
            if (this_frame_real_time_us > last_frame_written_real_time_us) {
                INFO("writing gap frame  time=%s.%6.6d\n",
                     time2str(ts1, this_frame_real_time_us / 1000000, false),
                     (int)(this_frame_real_time_us % 1000000));
                ret = rp_write_frame(this_frame_real_time_us, NULL, 0, false);
                if (ret < 0) {
                    ERROR("failed to write gap frame\n");
                    goto error;
                }
                last_frame_is_gap = true;
                last_frame_written_real_time_us = this_frame_real_time_us;
                rp_file_hdr.last_frame_valid_through_real_time_us = this_frame_real_time_us;
            } else {
                ERROR("gap frame time %s.%6.6d is less than last %s.%6.6d\n",
                      time2str(ts1, this_frame_real_time_us / 1000000, false),
                      (int)(this_frame_real_time_us % 1000000),
                      time2str(ts2, last_frame_written_real_time_us / 1000000, false),
                      (int)(last_frame_written_real_time_us % 1000000));
            }
        } 

        // if the last frame written is a gap frame, then update the time
        // the gap frame is valid through
        if (last_frame_is_gap) {
            uint64_t rt_us = get_real_time_us();
            if (rt_us > rp_file_hdr.last_frame_valid_through_real_time_us) {
                rp_file_hdr.last_frame_valid_through_real_time_us = rt_us;
            }
        }

        // copy up to 10 frames to the file
        for (loop_count = 0; loop_count < 10; loop_count++) {

            // attempt to get next record_frame from proc_frame_list, if none then break out of the while loop
            pthread_mutex_lock(&proc_frame_list_mutex);
            if (TAILQ_EMPTY(&proc_frame_list) || record_frame == TAILQ_LAST(&proc_frame_list,th1)) {
                pthread_mutex_unlock(&proc_frame_list_mutex);
                break;
            } else if (record_frame == NULL) {
                record_frame = TAILQ_LAST(&proc_frame_list,th1);
                record_frame->ref_count++;
            } else {
                record_frame->ref_count--;
                record_frame = TAILQ_NEXT(record_frame,entries);
                record_frame->ref_count++;
            }
            pthread_mutex_unlock(&proc_frame_list_mutex);

            // if frame time is less than last written then skip
            this_frame_real_time_us = record_frame->time_us + real_minus_monotonic_time_us;
            if (this_frame_real_time_us <= last_frame_written_real_time_us) {
                ERROR("frame time %s.%6.6d lte last_written %s.%6.6d\n",
                      time2str(ts1, this_frame_real_time_us / 1000000, false),
                      (int)(this_frame_real_time_us % 1000000),
                      time2str(ts2, last_frame_written_real_time_us / 1000000, false),
                      (int)(last_frame_written_real_time_us % 1000000));
                continue;
            }

            // write frame to file
            ret = rp_write_frame(this_frame_real_time_us, 
                                 record_frame->buff, 
                                 record_frame->buff_len,
                                 record_frame->motion);
            if (ret < 0) {
                ERROR("failed to write frame\n");
                goto error;
            }
            last_frame_is_gap = false;
            last_frame_written_real_time_us = this_frame_real_time_us;

            // save last_frame_valid_through_real_time_us 
            rp_file_hdr.last_frame_valid_through_real_time_us = this_frame_real_time_us;
        }

        // every 10 seconds sync data to disk and update the file hdr on disk 
        if (microsec_timer() - last_file_hdr_write_time_us > 10000000) {
            ret = rp_write_file_hdr();
            if (ret < 0) {
                ERROR("failed to write file hdr\n");
                goto error;
            }
            last_file_hdr_write_time_us = microsec_timer();
        }
    }

error:
    // thread exit
    INFO("terminating\n");
    return NULL;
}

// - - - - - - - - -  RP SUPPORT  - - - - - - - - - - - - - - - - - - - - 

int rp_read_and_verify_file_hdr(void)
{
    int           ret, i;
    struct stat   buf;
    rp_file_hdr_t hdr[2];
    bool          hdr_valid[2] = {false, false};
    uint32_t      cksum;

    // set rp_file_size to the size of the file
    ret = fstat(rp_fd, &buf);
    if (ret < 0) {
        ERROR("fstat failed, %s\n", strerror(errno));
        return -1;
    }
    rp_file_size = buf.st_size;

    // sanity check rp_file_size
    if (rp_file_size == 0 || 
        rp_file_size < RP_MIN_FILE_SIZE ||
        rp_file_size % 1000000)
    {
        ERROR("invalid rp_file_size %"PRId64"\n", rp_file_size);
        return -1;
    }

    // loop over hdrs
    for (i = 0; i < 2; i++) {
        // read hdr
        ret = pread(rp_fd, &hdr[i], sizeof(rp_file_hdr_t), 
                    i == 0 ? RP_FILE_HDR_0_OFFSET : RP_FILE_HDR_1_OFFSET);
        if (ret != sizeof(rp_file_hdr_t)) {
            ERROR("hdr_%d read failed, %s\n", i, strerror(errno));
            return -1;
        }
            
        // verify hdr
        if (hdr[i].magic != RP_FILE_MAGIC) {
            ERROR("hdr_%d invalid magic 0x%"PRIx64"\n", i, hdr[i].magic);
            continue;
        }
        if (hdr[i].file_size != rp_file_size) {
            ERROR("hdr_%d invalid file_size %"PRId64", expected=%"PRId64"\n", 
                  i, hdr[i].file_size, rp_file_size);
            continue;
        }
        if ((cksum = rp_checksum(&hdr[i],sizeof(rp_file_hdr_t))) != 0) {
            ERROR("hdr_%d invalid checksum 0x%x, expected 0\n", i, cksum);
            continue;
        }

        // set hdr_valid flag
        hdr_valid[i] = true;
    }

    // if hdr_0 and hdr_1 are both valid then choose the one with higher update_counter
    if (hdr_valid[0] && hdr_valid[1]) {
        INFO("both valid, update_counter = %"PRId64" %"PRId64"\n", hdr[0].update_counter, hdr[1].update_counter);
        if (hdr[0].update_counter >= hdr[1].update_counter) {
            INFO("choose hdr 0\n");
            rp_file_hdr = hdr[0];
            rp_file_hdr_last = 0;
        } else {
            INFO("choose hdr 1\n");
            rp_file_hdr = hdr[1];
            rp_file_hdr_last = 1;
        }
        return 0;
    }

    // if hdr_0 is valid or hdr_1 is valid then return the valid hdr
    if (hdr_valid[0]) {
        INFO("one hdr valid, choose 0\n");
        rp_file_hdr = hdr[0];
        rp_file_hdr_last = 0;
        return 0;
    }
    if (hdr_valid[1]) {
        INFO("one hdr valid, choose 1\n");
        rp_file_hdr = hdr[1];
        rp_file_hdr_last = 1;
        return 0;
    }

    // return error
    ERROR("neither file hdr is valid\n");
    return -1;
}

int rp_write_file_hdr(void)
{
    int ret;

    TIMING_BEGIN(&tmg_rp_write_file_hdr);

    // increment the update_counter, and
    // update the checksum
    rp_file_hdr.update_counter++;
    rp_file_hdr.checksum = -rp_checksum(&rp_file_hdr, sizeof(rp_file_hdr_t)-4);

    // sync data 
    fdatasync(rp_fd);

    // write hdr
    ret = pwrite(rp_fd, 
                 &rp_file_hdr, 
                 sizeof(rp_file_hdr_t), 
                 rp_file_hdr_last == 0 ? RP_FILE_HDR_1_OFFSET : RP_FILE_HDR_0_OFFSET);
    if (ret != sizeof(rp_file_hdr_t)) {
        return -1;
    }

    // sync hdr
    fdatasync(rp_fd);

    // update rp_file_hdr_last
    rp_file_hdr_last = (rp_file_hdr_last == 0 ? 1 : 0);

    TIMING_END(&tmg_rp_write_file_hdr);

    // return success
    return 0;
}

int rp_read_frame_by_file_offset(uint64_t frame_file_offset, rp_frame_hdr_t * rpfh, void ** data, uint32_t * status)
{
    uint32_t cksum;
    int      ret;

    // preset returns
    *status = STATUS_ERR_GENERAL_FAILURE;
    if (data != NULL) {
        *data = NULL;
    }

    // validate frame_file_offset
    if (frame_file_offset < RP_MIN_FRAME_FILE_OFFSET || frame_file_offset >= RP_MAX_FRAME_FILE_OFFSET) {
        ERROR("invalid frame_file_offset %"PRId64"\n", frame_file_offset);
        *status = STATUS_ERR_FRAME_FILE_OFFSET_INVLD;
        return -1;
    }

    // read the frame hdr
    ret = pread(rp_fd, rpfh, sizeof(rp_frame_hdr_t), frame_file_offset);
    if (ret != sizeof(rp_frame_hdr_t)) {
        ERROR("read frame hdr failed, offset=%"PRId64", %s\n", 
              frame_file_offset, strerror(errno));
        *status = STATUS_ERR_FRAME_HDR_READ;
        return -1;
    }

    // verify the frame hdr
    if (rpfh->magic != RP_FILE_FRAME_MAGIC) {
        ERROR("frame hdr invalid magic 0x%x\n", rpfh->magic);
        *status = STATUS_ERR_FRAME_HDR_MAGIC;
        return -1;
    }
    if ((cksum = rp_checksum(rpfh, sizeof(rp_frame_hdr_t))) != 0) {
        ERROR("frame hdr invalid checksum 0x%x, expected 0\n", cksum);
        *status = STATUS_ERR_FRAME_HDR_CHECKSUM;
        return -1;
    }

    // if data read not requested or data_len is zero then we're done
    if (data == NULL || rpfh->data_len == 0) {
        *status = STATUS_INFO_OK;
        return 0;
    }

    // allocate buffer and read the data
    *data = malloc(rpfh->data_len); 
    if (*data == NULL) {
        ERROR("failed malloc data_len %d\n", rpfh->data_len);
        *status = STATUS_ERR_FRAME_DATA_MEM_ALLOC;
        return -1;
    }
    ret = pread(rp_fd, *data, rpfh->data_len, frame_file_offset+sizeof(rp_frame_hdr_t));
    if (ret != rpfh->data_len) {
        ERROR("read frame data failed, offset=%"PRId64", %s\n", 
              frame_file_offset+sizeof(rp_frame_hdr_t), strerror(errno));
        *status = STATUS_ERR_FRAME_DATA_READ;
        return -1;
    }

    // return succes
    *status = STATUS_INFO_OK;
    return 0;
}

int rp_read_frame_by_real_time_us(uint64_t real_time_us, rp_frame_hdr_t * rpfh, void ** data, 
        uint32_t * status, uint64_t * frame_start_real_time_us, uint64_t * frame_end_real_time_us)
{
    int32_t  first, last, middle, count;
    uint64_t start_us, end_us;
    uint64_t te_middle_real_time_us, te_middle_plus_one_real_time_us;
    rp_toc_t found_toc_entry;
#ifdef DEBUG_PRINTS
    char ts1[MAX_TIME_STR], ts2[MAX_TIME_STR];
#endif

    #define RP_TOC(i) (rp_toc[(i)%RP_MAX_TOC])

    TIMING_BEGIN(&tmg_rp_read_frame_by_real_time_us);
    TIMING_BEGIN(&tmg_rp_read_frame_by_real_time_us_lookup);

    // init
    *status = STATUS_ERR_GENERAL_FAILURE;
    *frame_start_real_time_us = 0;
    *frame_end_real_time_us = 0;
    start_us = 0;
    end_us = 0;
    if (data) *data = NULL;
    count = 0;
    first = rp_toc_idx_oldest;
    last  = rp_toc_idx_next - 1;
    if (last < first) {
        last += RP_MAX_TOC;
    }

    // if nothing in toc then goto not_found
    if (first == -1) {
        *status = STATUS_ERR_FRAME_FILE_EMPTY;
        goto not_found;
    }

    // check if requested time is before the first or after last frame
    if (real_time_us < RP_TOC(first).real_time_us) {
        *status = STATUS_ERR_FRAME_BEFORE_BOD;
        goto not_found;
    }
    if (real_time_us > rp_file_hdr.last_frame_valid_through_real_time_us) {
        *status = STATUS_ERR_FRAME_AFTER_EOD;
        goto not_found;
    }

    // check if requested time is in the last frame
    if (real_time_us >= RP_TOC(last).real_time_us &&
        real_time_us <= rp_file_hdr.last_frame_valid_through_real_time_us) 
    {
        found_toc_entry = RP_TOC(last);
        start_us = RP_TOC(last).real_time_us;
        end_us   = rp_file_hdr.last_frame_valid_through_real_time_us;
        goto found;
    }

    // if just one entry in the toc then goto not_found 
    if (first == last) {
        *status = STATUS_ERR_FRAME_NOT_FOUND_1;
        goto not_found;
    }

    // use binary search
    while (true) {
        middle = (first + last) / 2;

        assert(middle != last);

        te_middle_real_time_us          = RP_TOC(middle).real_time_us;
        te_middle_plus_one_real_time_us = RP_TOC(middle+1).real_time_us;
        if (te_middle_plus_one_real_time_us <= te_middle_real_time_us) {
            *status = STATUS_ERR_FRAME_NOT_FOUND_2;
            goto not_found;
        }

        if (real_time_us >= te_middle_real_time_us &&
            real_time_us < te_middle_plus_one_real_time_us)
        {
            found_toc_entry = RP_TOC(middle);
            start_us = te_middle_real_time_us;
            end_us   = te_middle_plus_one_real_time_us - 1;
            goto found;
        }

        if (first + 1 == last) {
            *status = STATUS_ERR_FRAME_NOT_FOUND_3;
            goto not_found;
        }

        if (real_time_us > te_middle_real_time_us) {
            first = middle;
        } else {
            last = middle;
        }

        if (++count > 50) {
            *status = STATUS_ERR_FRAME_NOT_FOUND_4;
            goto not_found;
        }
    }

found:
    TIMING_END(&tmg_rp_read_frame_by_real_time_us_lookup);
    // read the frame
    if (rp_read_frame_by_file_offset((uint64_t)found_toc_entry.file_offset_div_32*32, rpfh, data, status) != 0) {
        goto not_found;
    }

    // verify the time of the frame read agrees with the toc
    if (rpfh->real_time_us != found_toc_entry.real_time_us) {
        *status = STATUS_ERR_FRAME_TIME;
        goto not_found;
    }

    // a valid frame has been read, set status to either STATUS_INFO_GAP or STATUS_INFO_OK
    *status = (rpfh->data_len == 0 ? STATUS_INFO_GAP : STATUS_INFO_OK);

    // return success
    *frame_start_real_time_us = start_us;
    *frame_end_real_time_us = end_us;
    DEBUG("success %s - request=%s.%3.3d start=%s.%3.3d duration=%d.%3.3d\n",
           status2str(*status),
           time2str(ts1, real_time_us/1000000, false),
           (int)((real_time_us%1000000)/1000),
           time2str(ts2, start_us/1000000, false),
           (int)((start_us%1000000)/1000),
           (int)((end_us - start_us) / 1000000),
           (int)(((end_us - start_us) % 1000000) / 1000));
    TIMING_END(&tmg_rp_read_frame_by_real_time_us);
    return 0;

not_found:
    // return error
    DEBUG("failure %s - request=%s.%3.3d\n",
           status2str(*status),
           time2str(ts1, real_time_us/1000000, false),
           (int)((real_time_us%1000000)/1000));
    if (data) {
        free(*data);
    }
    return -1;
}

int rp_write_frame(uint64_t real_time_us, void * data, int32_t data_len, bool motion)
{
    rp_frame_hdr_t  rpfh;
    struct iovec    iov[2];
    int             ret;
    uint64_t        frame_file_offset;
    uint32_t        ti_last, ti_oldest;

    static uint64_t last_real_time_us;

    TIMING_BEGIN(&tmg_rp_write_frame);

    // validate real_time_us, log error and drop frame if out of order
    if (last_real_time_us == 0) {
        last_real_time_us = rp_file_hdr.last_frame_valid_through_real_time_us;
    }
    if (real_time_us <= last_real_time_us) {
        ERROR("frame time out of order, curr=%"PRId64" last=%"PRId64"\n", real_time_us, last_real_time_us);
        return 0;
    }
    last_real_time_us = real_time_us;

    // determine the frame_file_offset
    if (rp_file_hdr.last_frame_file_offset == 0) {
        frame_file_offset = RP_MIN_FRAME_FILE_OFFSET;
    } else {
        frame_file_offset = 
            ROUND_UP(rp_file_hdr.last_frame_file_offset+rp_file_hdr.last_frame_length,32);
        if (frame_file_offset >= RP_MAX_FRAME_FILE_OFFSET) {
            frame_file_offset = RP_MIN_FRAME_FILE_OFFSET;
        }
    }

    // make the frame header
    bzero(&rpfh,sizeof(rpfh));
    rpfh.magic                   = RP_FILE_FRAME_MAGIC;
    rpfh.data_len                = data_len;
    rpfh.real_time_us            = real_time_us;
    rpfh.prior_frame_file_offset = rp_file_hdr.last_frame_file_offset;
    rpfh.motion                  = motion;
    rpfh.checksum                = -rp_checksum(&rpfh, sizeof(rpfh)-4);

    // write frame header and image to file
    iov[0].iov_base = &rpfh;
    iov[0].iov_len  = sizeof(rp_frame_hdr_t);
    iov[1].iov_base = data;
    iov[1].iov_len  = data_len;
    ret = pwritev(rp_fd, iov, 2, frame_file_offset);
    if (ret != iov[0].iov_len + iov[1].iov_len) {
        ERROR("write failed, %s\n", strerror(errno));
        return -1;
    }

    // update the file header with the offset and length of the last frame
    rp_file_hdr.last_frame_file_offset = frame_file_offset;
    rp_file_hdr.last_frame_length      = sizeof(rp_frame_hdr_t) + data_len;

    // add entry to rp_toc
    ti_last = rp_toc_idx_next;
    rp_toc[rp_toc_idx_next].real_time_us       = real_time_us;
    rp_toc[rp_toc_idx_next].file_offset_div_32 = frame_file_offset / 32;
    rp_toc_idx_next = (rp_toc_idx_next + 1) % RP_MAX_TOC;

    // update rp_toc_idx_oldest if needed:
    if (rp_toc_idx_oldest == -1) {
        rp_toc_idx_oldest = ti_last;
    } else {
        ti_oldest = rp_toc_idx_oldest;

        if (ti_oldest < ti_last) {
            ti_oldest += RP_MAX_TOC;
        }
        if (ti_oldest - ti_last < 100) {
            ti_oldest = ti_last + 100;
        }
        ti_oldest = (ti_oldest % RP_MAX_TOC);

        while (rp_file_size - RP_FILE_LENGTH((uint64_t)rp_toc[ti_oldest].file_offset_div_32*32) < RP_MIN_FILE_FREE_SIZE) {
            ti_oldest = ((ti_oldest + 1) % RP_MAX_TOC);
        }

        rp_toc_idx_oldest = ti_oldest;
    }            

    TIMING_END(&tmg_rp_write_frame);

    // return success
    return 0;
}

uint32_t rp_checksum(void * data, size_t len)
{
    uint32_t i, sum = 0;

    assert((len % 4) == 0);
    for (i = 0; i < len/4; i++) {
        sum += ((uint32_t *)data)[i];
    }
    return sum;
}
