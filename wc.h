#ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#endif
#define __STDC_FORMAT_MACROS

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <libgen.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <netdb.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <ifaddrs.h>
#include <termios.h>
#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/queue.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#ifndef ANDROID
#include <sys/statvfs.h>
#endif

#ifdef ANDROID
#define SOCK_CLOEXEC 0
#endif

// -----------------  VERSION  ------------------------------------------------------

#define VERSION_MAJOR 1
#define VERSION_MINOR 3

#define VERSION ( { version_t v = { VERSION_MAJOR, VERSION_MINOR }; v; } );

typedef struct {
    int major;
    int minor;
} version_t;

// -----------------  CONFIG READ/WRITE  --------------------------------------------

#define MAX_CONFIG_VALUE_STR 100

typedef struct {
    const char * name;
    char         value[MAX_CONFIG_VALUE_STR];
} config_t;

int config_read(char * config_path, config_t * config, int config_version);
int config_write(char * config_path, config_t * config, int config_version);

// -----------------  WC SERVER -----------------------------------------------------

#define ADMIN_SERVER_HOSTNAME        "sthaid-rs.dyndns.org"

#define ADMIN_SERVER_PORT             80
#define ADMIN_SERVER_DGRAM_PORT       9001

#define ADMIN_SERVER_MAX_NETTEST_BUFF 0x100000 

#define ADMIN_SERVER_LOGIN_OK         12345678

#define MAX_USER_WC                   32
#define MAX_WC_NAME                   32
#define MAX_WC_MACADDR                32
#define MAX_USER_NAME                 32
#define MAX_PASSWORD                  32
#define MIN_USER_NAME                 4
#define MIN_PASSWORD                  4

#define HTTP_CONNECT_REQ  "CONNECT " ADMIN_SERVER_HOSTNAME ":80 HTTP/1.0\r\n\r\n"
#define HTTP_CONNECT_RESP "HTTP/1.0 200 OK\r\n\r\n"

int connect_to_admin_server(char * user_name, char * password, char * service, int * connect_status);

// -----------------  PEER TO PEER COMMUNICATION  ------------------------------------

#define SERVICE_INVALID       0
#define SERVICE_NETTEST       1
#define SERVICE_SHELL         2
#define SERVICE_WEBCAM        3

#define SERVICE_STR(x) \
    ((x) == SERVICE_INVALID ? "SVC_INVALID" : \
     (x) == SERVICE_NETTEST ? "SVC_NETTEST" : \
     (x) == SERVICE_SHELL   ? "SVC_SHELL"   : \
     (x) == SERVICE_WEBCAM  ? "SVC_WEBCAM"  : \
                              "SVC_????")

#define RECV_WAIT_ANY    0
#define RECV_WAIT_ALL    1
#define RECV_NOWAIT_ANY  2
#define RECV_NOWAIT_ALL  3

#define RECV_EOF        (0)
#define RECV_ERROR      (-1)
#define RECV_WOULDBLOCK (-2)

#define P2P_ANNOUNCE_INTVL_US  10000000  // 10 secs

typedef struct {
    uint64_t sent_bytes;
    uint64_t recvd_bytes;
    uint64_t sent_data_dgrams;
    uint64_t recvd_data_dgrams;
    uint64_t sent_acks;
    uint64_t recvd_acks;
    uint64_t resent_data_dgrams;
    uint64_t recvd_duplicates;
    uint64_t peer_resent_data_dgrams;
    uint64_t peer_recvd_duplicates;
} p2p_stats_t;

#define p2p_init        (*p2p->init)
#define p2p_connect     (*p2p->connect)
#define p2p_accept      (*p2p->accept)
#define p2p_disconnect  (*p2p->disconnect)
#define p2p_send        (*p2p->send)
#define p2p_recv        (*p2p->recv)
#define p2p_get_stats   (*p2p->get_stats)
#define p2p_monitor_ctl (*p2p->monitor_ctl)
#define p2p_debug_con   (*p2p->debug_con)

typedef struct {
    int (*init)(int max_con);
    int (*connect)(char * user_name, char * password, char * wc_name, int service, int * connect_status);
    int (*accept)(char * wc_macaddr, int * service, char * user_name);
    int (*disconnect)(int handle);
    int (*send)(int handle, void * buff, int len);
    int (*recv)(int handle, void * buff, int len, int mode);
    int (*get_stats)(int handle, p2p_stats_t * stats);
    int (*monitor_ctl)(int handle, int secs);
    void (*debug_con)(int handle);
} p2p_routines_t;

extern p2p_routines_t * p2p;

// - - - - - - - - -  P2P1 DEFINITIONS   - - - - - - - - - - - - - - - - - - - - - - - 

#define MAX_ACK 10

#define DGRAM_ID_WC_ANNOUNCE        1234000001
#define DGRAM_ID_CONNECT_REQ        1234000011
#define DGRAM_ID_CONNECT_ACTIVATE   1234000012
#define DGRAM_ID_CONNECT_REJECT     1234000013
#define DGRAM_ID_P2P_CON_REQ        1234000021
#define DGRAM_ID_P2P_CON_RESP       1234000022
#define DGRAM_ID_P2P_DATA           1234000023
#define DGRAM_ID_P2P_ACK            1234000024
#define DGRAM_ID_P2P_STATS          1234000025

#define DGRAM_ID_STR(x) \
    ((x) == DGRAM_ID_WC_ANNOUNCE      ? "DGRAM_ID_WC_ANNOUNCE"      : \
     (x) == DGRAM_ID_CONNECT_REQ      ? "DGRAM_ID_CONNECT_REQ"      : \
     (x) == DGRAM_ID_CONNECT_ACTIVATE ? "DGRAM_ID_CONNECT_ACTIVATE" : \
     (x) == DGRAM_ID_CONNECT_REJECT   ? "DGRAM_ID_CONNECT_REJECT"   : \
     (x) == DGRAM_ID_P2P_CON_REQ      ? "DGRAM_ID_P2P_CON_REQ"      : \
     (x) == DGRAM_ID_P2P_CON_RESP     ? "DGRAM_ID_P2P_CON_RESP"     : \
     (x) == DGRAM_ID_P2P_DATA         ? "DGRAM_ID_P2P_DATA"         : \
     (x) == DGRAM_ID_P2P_ACK          ? "DGRAM_ID_P2P_ACK"          : \
     (x) == DGRAM_ID_P2P_STATS        ? "DGRAM_ID_P2P_STATS"        : \
                                        "DGRAM_ID_????")

#pragma pack(push,1)
typedef struct {
    uint32_t v[4];
} dgram_uid_t;

typedef struct {
    int id;
    int pad;
    union {
        struct {
            version_t version;
            char wc_macaddr[MAX_WC_MACADDR+1];
            struct sockaddr_in wc_addr_behind_nat;
            char dgram_end;
        } wc_announce;
        struct {
            char user_name[MAX_USER_NAME+1];
            char password[MAX_PASSWORD+1];
           char wc_name[MAX_WC_NAME+1];
            int service;
            struct sockaddr_in client_addr_behind_nat;
            dgram_uid_t dgram_uid;
            char dgram_end;
        } connect_req;
        struct {
            uint64_t con_id;
            char user_name[MAX_USER_NAME+1];
            int service;
            struct sockaddr_in client_addr;
            struct sockaddr_in wc_addr;
            dgram_uid_t dgram_uid;
            char dgram_end;
        } connect_activate;
        struct {
            int status;
            dgram_uid_t dgram_uid;
            char dgram_end;
        } connect_reject;
        struct {
            uint64_t con_id; 
            char dgram_end;
        } p2p_con_req;
        struct {
            uint64_t con_id;
            char dgram_end;
        } p2p_con_resp;
        struct {
            uint64_t con_id;
            uint64_t recvbuff_offset;
            int ack[MAX_ACK];
            char dgram_end;
        } p2p_ack;
        struct {
            uint64_t con_id;
            uint64_t recvbuff_offset;
            int ack[MAX_ACK];
            int id;
            uint64_t offset;
            uint64_t length;
            char data[0];
        } p2p_data;
        struct {
            uint64_t con_id;
            uint64_t resent_data_dgrams;
            uint64_t recvd_duplicates;
            uint64_t max_data_dgram;
            char dgram_end;
        } p2p_stats;
    } u;
} dgram_t;
#pragma pack(pop)

extern p2p_routines_t p2p1;

dgram_uid_t dgram_uid_gen(void);
bool dgram_uid_equal(dgram_uid_t * x, dgram_uid_t * y);

// - - - - - - - - -  P2P2 DEFINITIONS   - - - - - - - - - - - - - - - - - - - - - - - 

extern p2p_routines_t p2p2;

// -----------------  SERVICE_NETTEST DEFINITIONS  ------------------------------------

#define NT_MAX_SEND_OR_RECV_LEN         1000000
#define NT_DEFAULT_SEND_OR_RECV_LEN     100000

#define NT_MSG_TYPE_DATA                777001
#define NT_MSG_TYPE_REQUEST_SEND_DATA   777002
#define NT_MSG_TYPE_REQUEST_EXIT        777003
#define NT_MSG_TYPE_RESPONSE            777009

//                                   VAL0              VAL1            VAL2         
// NT_MSG_TYPE_DATA               data-length
// NT_MSG_TYPE_REQUEST_SEND_DATA  req-id            new-send-len
// NT_MSG_TYPE_REQUEST_EXIT       req-id            not-used
// NT_MSG_TYPE_RESPONSE           echo-of-req-id    resp-data      echo-of-req-msg-type

#pragma pack(push,1)
typedef struct {
    int msg_type;
    int val0;
    int val1;
    int val2;
    unsigned char data[0];
} nt_msg_t;
#pragma pack(pop)

// -----------------  SERVICE_WEBCAM DEFINITIONS  ------------------------------------

// msg_type for messages from webcam
#define MSG_TYPE_FRAME                     1 
#define MSG_TYPE_STATUS                    2

// msg_type for messages to webcam
#define MSG_TYPE_CMD_SET_MODE              11
#define MSG_TYPE_CMD_LIVE_MODE_CHANGE_RES  12
#define MSG_TYPE_CMD_SET_MIN_SEND_INTVL_US 13

// max frame data length
#define RP_MAX_FRAME_DATA_LEN   500000

// status values
#define STATUS_INFO_OK                       0
#define STATUS_INFO_STOPPED                  1
#define STATUS_INFO_GAP                      2
#define STATUS_INFO_LOADING_IMAGE            3
#define STATUS_INFO_CHANGING_RESOLUTION      4
#define STATUS_INFO_NOT_RUN                  5
#define STATUS_INFO_IN_PROGRESS              6
#define STATUS_ERR_GENERAL_FAILURE           100
#define STATUS_ERR_FRAME_FILE_OFFSET_INVLD   101
#define STATUS_ERR_FRAME_FILE_EMPTY          102
#define STATUS_ERR_FRAME_BEFORE_BOD          103
#define STATUS_ERR_FRAME_AFTER_EOD           104
#define STATUS_ERR_FRAME_NOT_FOUND_1         105
#define STATUS_ERR_FRAME_NOT_FOUND_2         106
#define STATUS_ERR_FRAME_NOT_FOUND_3         107
#define STATUS_ERR_FRAME_NOT_FOUND_4         108
#define STATUS_ERR_FRAME_HDR_READ            109
#define STATUS_ERR_FRAME_HDR_MAGIC           110
#define STATUS_ERR_FRAME_HDR_CHECKSUM        111
#define STATUS_ERR_FRAME_DATA_MEM_ALLOC      112
#define STATUS_ERR_FRAME_DATA_READ           113
#define STATUS_ERR_FRAME_TIME                114
#define STATUS_ERR_JPEG_DECODE               115
#define STATUS_ERR_DEAD                      116
#define STATUS_ERR_WEBCAM_FAILURE            117
#define STATUS_ERR_SYSTEM_CLOCK_NOT_SET      118
#define STATUS_ERR_NO_USERNAME               119
#define STATUS_ERR_NO_PASSWORD               120
#define STATUS_ERR_HANDLE_TOO_BIG            121
#define STATUS_ERR_GET_SERVER_ADDR           122
#define STATUS_ERR_CREATE_SOCKET             123
#define STATUS_ERR_GET_LOCAL_ADDR            124
#define STATUS_ERR_BIND_LOCAL_ADDR           125
#define STATUS_ERR_GETSOCKNAME               126
#define STATUS_ERR_SENDTO                    127
#define STATUS_ERR_RECVFROM                  128
#define STATUS_ERR_NO_RESPONSE_FROM_SERVER   129
#define STATUS_ERR_INVALID_CONNECTION_ID     130
#define STATUS_ERR_DUPLICATE_CONNECTION_ID   131
#define STATUS_ERR_TOO_MANY_CONNECTIONS      132   // XXX distinguish proxy server vs webcam accept vs locl
#define STATUS_ERR_CONNECTION_MEM_ALLOC      133
#define STATUS_ERR_NO_RESPONSE_FROM_PEER     134
#define STATUS_ERR_FAILED_CONNECT_TO_SERVER  135
#define STATUS_ERR_INVLD_RESP_FROM_SERVER    136
#define STATUS_ERR_USERNAME_LENGTH           137
#define STATUS_ERR_PASSWORD_LENGTH           138
#define STATUS_ERR_USERNAME_CHARS            139
#define STATUS_ERR_PASSWORD_CHARS            140
#define STATUS_ERR_INVALID_USER_OR_PASSWD    141
#define STATUS_ERR_INVALID_SERVICE           142
#define STATUS_ERR_WC_DOES_NOT_EXIST         143
#define STATUS_ERR_WC_NOT_ONLINE             144
#define STATUS_ERR_WC_ADDR_NOT_AVAIL         145
#define STATUS_ERR_USERNAME_ALREADY_EXISTS   146
#define STATUS_ERR_TOO_MANY_USERS            147
#define STATUS_ERR_CREATE_USER_PROFILE       148

#define STATUS_STR(status) \
    ((status) == STATUS_INFO_OK                       ? "OK"                      : \
     (status) == STATUS_INFO_STOPPED                  ? "STOPPED"                 : \
     (status) == STATUS_INFO_GAP                      ? "GAP"                     : \
     (status) == STATUS_INFO_LOADING_IMAGE            ? "LOADING_IMAGE"           : \
     (status) == STATUS_INFO_CHANGING_RESOLUTION      ? "CHANGING_RESOLUTION"     : \
     (status) == STATUS_INFO_NOT_RUN                  ? "NOT_RUN"                 : \
     (status) == STATUS_INFO_IN_PROGRESS              ? "IN_PROGRESS"             : \
     (status) == STATUS_ERR_GENERAL_FAILURE           ? "GENERAL_FAILURE"         : \
     (status) == STATUS_ERR_FRAME_FILE_OFFSET_INVLD   ? "FRAME_FILE_OFFSET_INVLD" : \
     (status) == STATUS_ERR_FRAME_FILE_EMPTY          ? "FRAME_FILE_EMPTY"        : \
     (status) == STATUS_ERR_FRAME_BEFORE_BOD          ? "FRAME_BEFORE_BOD"        : \
     (status) == STATUS_ERR_FRAME_AFTER_EOD           ? "FRAME_AFTER_EOD"         : \
     (status) == STATUS_ERR_FRAME_NOT_FOUND_1         ? "FRAME_NOT_FOUND_1"       : \
     (status) == STATUS_ERR_FRAME_NOT_FOUND_2         ? "FRAME_NOT_FOUND_2"       : \
     (status) == STATUS_ERR_FRAME_NOT_FOUND_3         ? "FRAME_NOT_FOUND_3"       : \
     (status) == STATUS_ERR_FRAME_NOT_FOUND_4         ? "FRAME_NOT_FOUND_4"       : \
     (status) == STATUS_ERR_FRAME_HDR_READ            ? "FRAME_HDR_READ"          : \
     (status) == STATUS_ERR_FRAME_HDR_MAGIC           ? "FRAME_HDR_MAGIC"         : \
     (status) == STATUS_ERR_FRAME_HDR_CHECKSUM        ? "FRAME_HDR_CHECKSUM"      : \
     (status) == STATUS_ERR_FRAME_DATA_MEM_ALLOC      ? "FRAME_DATA_MEM_ALLOC"    : \
     (status) == STATUS_ERR_FRAME_DATA_READ           ? "FRAME_DATA_READ"         : \
     (status) == STATUS_ERR_FRAME_TIME                ? "FRAME_TIME"              : \
     (status) == STATUS_ERR_JPEG_DECODE               ? "JPEG_DECODE"             : \
     (status) == STATUS_ERR_DEAD                      ? "DEAD"                    : \
     (status) == STATUS_ERR_WEBCAM_FAILURE            ? "WEBCAM_FAILURE"          : \
     (status) == STATUS_ERR_SYSTEM_CLOCK_NOT_SET      ? "SYSTEM_CLOCK_NOT_SET"    : \
     (status) == STATUS_ERR_NO_USERNAME               ? "NO_USERNAME"             : \
     (status) == STATUS_ERR_NO_PASSWORD               ? "NO_PASSWORD"             : \
     (status) == STATUS_ERR_HANDLE_TOO_BIG            ? "HANDLE_TOO_BIG"          : \
     (status) == STATUS_ERR_GET_SERVER_ADDR           ? "GET_SERVER_ADDR"         : \
     (status) == STATUS_ERR_CREATE_SOCKET             ? "CREATE_SOCKET"           : \
     (status) == STATUS_ERR_GET_LOCAL_ADDR            ? "GET_LOCAL_ADDR"          : \
     (status) == STATUS_ERR_BIND_LOCAL_ADDR           ? "BIND_LOCAL_ADDR"         : \
     (status) == STATUS_ERR_GETSOCKNAME               ? "GETSOCKNAME"             : \
     (status) == STATUS_ERR_SENDTO                    ? "SENDTO"                  : \
     (status) == STATUS_ERR_RECVFROM                  ? "RECVFROM"                : \
     (status) == STATUS_ERR_NO_RESPONSE_FROM_SERVER   ? "NO_RESPONSE_FROM_SERVER" : \
     (status) == STATUS_ERR_INVALID_CONNECTION_ID     ? "INVALID_CONNECTION_ID"   : \
     (status) == STATUS_ERR_DUPLICATE_CONNECTION_ID   ? "DUPLICATE_CONNECTION_ID" : \
     (status) == STATUS_ERR_TOO_MANY_CONNECTIONS      ? "TOO_MANY_CONNECTIONS"    : \
     (status) == STATUS_ERR_CONNECTION_MEM_ALLOC      ? "CONNECTION_MEM_ALLOC"    : \
     (status) == STATUS_ERR_NO_RESPONSE_FROM_PEER     ? "NO_RESPONSE_FROM_PEER"   : \
     (status) == STATUS_ERR_FAILED_CONNECT_TO_SERVER  ? "FAILED_CONNECT_TO_SERVER": \
     (status) == STATUS_ERR_INVLD_RESP_FROM_SERVER    ? "INVLD_RESP_FROM_SERVER"  : \
     (status) == STATUS_ERR_USERNAME_LENGTH           ? "USERNAME_LENGTH"         : \
     (status) == STATUS_ERR_PASSWORD_LENGTH           ? "PASSWORD_LENGTH"         : \
     (status) == STATUS_ERR_USERNAME_CHARS            ? "USERNAME_CHARS"          : \
     (status) == STATUS_ERR_PASSWORD_CHARS            ? "PASSWORD_CHARS"          : \
     (status) == STATUS_ERR_INVALID_USER_OR_PASSWD    ? "ACCESS_DENIED"           : \
     (status) == STATUS_ERR_INVALID_SERVICE           ? "INVALID_SERVICE"         : \
     (status) == STATUS_ERR_WC_DOES_NOT_EXIST         ? "WC_DOES_NOT_EXIST"       : \
     (status) == STATUS_ERR_WC_NOT_ONLINE             ? "WC_NOT_ONLINE"           : \
     (status) == STATUS_ERR_WC_ADDR_NOT_AVAIL         ? "WC_ADDR_NOT_AVAIL"       : \
     (status) == STATUS_ERR_USERNAME_ALREADY_EXISTS   ? "USERNAME_ALREADY_EXISTS" : \
     (status) == STATUS_ERR_TOO_MANY_USERS            ? "TOO_MANY_USERS"          : \
     (status) == STATUS_ERR_CREATE_USER_PROFILE       ? "CREATE_USER_PROFILE"     : \
                                                        "????")

// mode values
#define MODE_NONE      0
#define MODE_LIVE      1
#define MODE_PLAYBACK  2
#define MODE_STR(mode) \
    ((mode) == MODE_NONE     ? "NONE"     : \
     (mode) == MODE_LIVE     ? "LIVE"     : \
     (mode) == MODE_PLAYBACK ? "PLAYBACK" : \
                               "????")

// pb_submode values
#define PB_SUBMODE_STOP  0
#define PB_SUBMODE_PAUSE 1
#define PB_SUBMODE_PLAY  2
#define PB_SUBMODE_STR(psm) \
    ((psm) == PB_SUBMODE_STOP  ? "STOPPED" : \
     (psm) == PB_SUBMODE_PAUSE ? "PAUSED"  : \
     (psm) == PB_SUBMODE_PLAY  ? "PLAYING" : \
                                 "????")

// pb_dir values
#define PB_DIR_FWD  0
#define PB_DIR_REV  1
#define PB_DIR_STR(dir) \
    ((dir) == PB_DIR_FWD  ? "FWD"  : \
     (dir) == PB_DIR_REV  ? "REV"  : \
                            "????")

// macro to get the real time when in PB_SUBMODE_PLAY
#define PB_SUBMODE_PLAY_REAL_TIME_US(m) \
    ((m)->pb_real_time_us + \
     ((int64_t)(get_real_time_us() - (m)->pb_mode_entry_real_time_us) * \
      ((m)->pb_dir == PB_DIR_FWD ? 1 : -1) * \
      ((m)->pb_speed)))

#pragma pack(push,1)
typedef struct {
    uint32_t msg_type;
    union {
        // messages from webcam
        struct {
            uint64_t mode_id;
            uint64_t real_time_us;
            uint32_t data_len;
            uint32_t status;
            bool     motion;
        } mt_frame;
        struct status_s {
            version_t version;
            uint32_t cam_status;
            uint32_t rp_status;
            uint64_t rp_duration_us;
            uint32_t p2p_resend_cnt;
            uint32_t p2p_recvdup_cnt;
        } mt_status;

        // messages to webcam
        struct mode_s {
            uint32_t mode;
            uint64_t mode_id;
            uint32_t pb_submode;
            uint64_t pb_mode_entry_real_time_us;
            uint64_t pb_real_time_us;
            uint32_t pb_dir;
            double   pb_speed;
        } mt_cmd_set_mode;
        struct {
            uint32_t empty;
        } mt_cmd_change_res;
        struct {
            uint64_t us;
        } mt_cmd_min_send_intvl;
    } u;
} webcam_msg_t;
#pragma pack(pop)

// -----------------  SERVICE_SHELL DEFINITIONS  -------------------------------------

// define shell msgs sent from loginwc.c client sent to wc_login.c server

#define SHELL_MSGID_INIT     0x55667701
#define SHELL_MSGID_WINSIZE  0x55667702
#define SHELL_MSGID_DATA     0x55667703

#define MAX_SHELL_MSGID_DATA_DATALEN 10000

typedef struct {
    uint32_t msgid;
    uint32_t datalen;
} shell_msg_hdr_t;

typedef struct {
    uint32_t rows;
    uint32_t cols;
    char     term[200];
} shell_msg_init_t;

typedef struct {
    uint32_t rows;
    uint32_t cols;
} shell_msg_winsize_t;

// -----------------  TIMING CODE PATH EXECUTION  ------------------------------------

#ifdef DEBUG_TIMING

typedef struct {
    const char * name;
    uint64_t     min;
    uint64_t     max;
    uint64_t     sum;
    uint64_t     count;
    uint64_t     begin_us;
    uint64_t     last_print_us;
    uint64_t     print_intvl_us;
} timing_t;

#define TIMING_DECLARE(tmg, pr_intvl) \
    static timing_t tmg = { #tmg, UINT64_MAX, 0, 0, 0, 0, 0, pr_intvl }

#define TIMING_BEGIN(tmg) \
    do {\
        (tmg)->begin_us = microsec_timer(); \
    } while (0)

#define TIMING_END(tmg) \
    do { \
        uint64_t curr_us = microsec_timer(); \
        uint64_t dur_us = curr_us - (tmg)->begin_us; \
        if (dur_us < (tmg)->min) { \
            (tmg)->min = dur_us; \
        } \
        if (dur_us > (tmg)->max) { \
            (tmg)->max = dur_us; \
        } \
        (tmg)->sum += dur_us; \
        (tmg)->count++; \
        if ((tmg)->last_print_us == 0) { \
            (tmg)->last_print_us = curr_us; \
        } else if (curr_us - (tmg)->last_print_us > (tmg)->print_intvl_us) { \
            NOTICE("TIMING %s avg=%d.%3.3d min=%d.%3.3d max=%d.%3.3d count=%d\n", \
                   (tmg)->name, \
                   (int)(((tmg)->sum/(tmg)->count) / 1000000), \
                   (int)(((tmg)->sum/(tmg)->count) % 1000000) / 1000, \
                   (int)((tmg)->min / 1000000), \
                   (int)((tmg)->min % 1000000) / 1000, \
                   (int)((tmg)->max / 1000000), \
                   (int)((tmg)->max % 1000000) / 1000, \
                   (int)((tmg)->count)); \
            (tmg)->min           = UINT64_MAX; \
            (tmg)->max           = 0; \
            (tmg)->sum           = 0; \
            (tmg)->count         = 0; \
            (tmg)->begin_us      = 0; \
            (tmg)->last_print_us = curr_us; \
        } \
    } while (0)

#else

#define TIMING_DECLARE(tmg, pr_intvl)

#define TIMING_BEGIN(tmg) \
    do { \
    } while (0)

#define TIMING_END(tmg) \
    do { \
    } while (0)

#endif

// -----------------  JPEG_DECODE  ---------------------------------------------------

#define JPEG_DECODE_MODE_GS    1
#define JPEG_DECODE_MODE_YUY2  2

int jpeg_decode(uint32_t cxid, uint32_t jpeg_decode_mode, uint8_t * jpeg, uint32_t jpeg_size,
                uint8_t ** out_buf, uint32_t * width, uint32_t * height);

// -----------------  UTILS  ---------------------------------------------------------

#define INFO(fmt, args...) \
    do { \
        logmsg("INFO", __func__, fmt, ## args); \
    } while (0)
#define WARN(fmt, args...) \
    do { \
        logmsg("WARN", __func__, fmt, ## args); \
    } while (0)
#define ERROR(fmt, args...) \
    do { \
        logmsg("ERROR", __func__, fmt, ## args); \
    } while (0)
#define FATAL(fmt, args...) \
    do { \
        logmsg("FATAL", __func__, fmt, ## args); \
        exit(1); \
    } while (0)
#ifdef DEBUG_PRINTS
  #define DEBUG(fmt, args...) \
      do { \
          logmsg("DEBUG", __func__, fmt, ## args); \
      } while (0)
#else
  #define DEBUG(fmt, args...) 
#endif

#define PRINTF(fmt, args...) \
    do { \
        printmsg(fmt, ## args); \
    } while (0)

#define MAX_LOGMSG_FILE_SIZE 0x100000

#define TIMESPEC_TO_US(ts) ((uint64_t)(ts)->tv_sec * 1000000 + (ts)->tv_nsec / 1000)
#define TIMEVAL_TO_US(tv)  ((uint64_t)(tv)->tv_sec * 1000000 + (tv)->tv_usec)

#define MAX_MACADDR_STR 32
#define MAX_TIME_STR    32
#define MAX_INT_STR     32

extern int64_t system_clock_offset_us;

int getsockaddr(char * node, int port, int socktype, int protcol, struct sockaddr_in * ret_addr);
void set_sock_opts(int sfd, int reuseaddr, int sndbuf, int rcvbuf, int rcvto);
char * sock_to_options_str(int sfd, char * s, int slen);
char * sock_addr_to_str(char * s, int slen, struct sockaddr * addr);
int getmacaddr(char * macaddr_str);
void logmsg_init(char * logmsg_file);
void logmsg(char * lvl, const char * func, char * fmt, ...) __attribute__ ((format (printf, 3, 4)));
void printmsg(char *fmt, ...);
void convert_yuy2_to_rgb(uint8_t * yuy2, uint8_t * rgb, int pixels);
void convert_yuy2_to_gs(uint8_t * yuy2, uint8_t * gs, int pixels);
uint64_t microsec_timer(void);
uint64_t get_real_time_us(void);
char * time2str(char * str, time_t time, bool gmt);
bool ntp_synced(void);
void real_time_init(void);
uint64_t fs_avail_bytes(char * path);
char * status2str(uint32_t status);
char * int2str(char * str, int64_t n);

