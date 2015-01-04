#include "wc.h"

#include <sys/ioctl.h>

// 
// defines
//

#define MAX_HANDLE          200  // android uses a lot of fds
#define MAX_RECV_SAVE_BUFF  0x1000

#define MILLISEC_TIMER      (microsec_timer() / 1000)

//
// typedefs
//

typedef struct {
    uint64_t sent_bytes;
    uint64_t recvd_bytes;
} stats_t;

typedef struct {
    bool            connected;
    bool            failed;
    bool            recv_eof;
    pthread_mutex_t send_mutex;
    void          * recv_save_buff;
    uint32_t        recv_save_buff_len;
    stats_t         stats;
    pthread_t       mon_thread_id;
    bool            mon_thread_cancel_req;
    int             mon_secs;
} con_t;

//
// variables
//

con_t con_tbl[MAX_HANDLE];

//
// prototypes
//

static int p2p2_connect(char * user_name, char * password, char * wc_name, int service, int * connect_status);
static int p2p2_accept(char * wc_macaddr, int * service, char * user_name);
static int p2p2_disconnect(int handle);
static int p2p2_send(int handle, void * buff, int len);
static int p2p2_recv(int handle, void * buff, int len, int mode);
static int p2p2_get_stats(int handle, p2p_stats_t * stats);
static int p2p2_monitor_ctl(int handle, int secs);
static void * monitor_thread(void * cx);
static void p2p2_debug_con(int handle);
static void sleep_ms(int ms);

//
// p2p routines
//

p2p_routines_t p2p2 = { p2p2_connect,
                        p2p2_accept,
                        p2p2_disconnect,
                        p2p2_send,
                        p2p2_recv,
                        p2p2_get_stats,
                        p2p2_monitor_ctl,
                        p2p2_debug_con };

// -------------------------------------------------------------------------------

static int p2p2_connect(char * user_name, char * password, char * wc_name, int service_id, int * connect_status)
{
    char      service[100];
    int       handle;
    void    * recv_save_buff;
    con_t   * con;
    pthread_t thread_id;
    int       tries = 0;

try_again:
    // connect to admin_server, with 'wccon' service
    sprintf(service, "wccon %s %d", wc_name, service_id);
    handle = connect_to_admin_server(user_name, password, service, connect_status);

    // retry on WC_ADDR_NOT_AVAIL error
    if (handle < 0 && *connect_status == STATUS_ERR_WC_ADDR_NOT_AVAIL && ++tries <= 5) {
        sleep(1);
        goto try_again;
    }

    // verify handle is in range
    if (handle < 0 || handle >= MAX_HANDLE) {
        if (handle >= MAX_HANDLE) {
            *connect_status = STATUS_ERR_HANDLE_TOO_BIG;
            close(handle);
        }
        ERROR("unable to connect to admin_server, %s\n", status2str(*connect_status));
        return -1;
    }

    // allocate recv_save_buff
    recv_save_buff = calloc(1,MAX_RECV_SAVE_BUFF);
    if (recv_save_buff == NULL) {
        ERROR("recv_save_buff alloc failed\n");
        *connect_status = STATUS_ERR_CONNECTION_MEM_ALLOC;
        close(handle);
        return -1;
    }

    // init con, non-zero fields only are set
    con = &con_tbl[handle];
    bzero(con, sizeof(con_t));
    con->connected = true;
    pthread_mutex_init(&con->send_mutex,NULL);
    con->recv_save_buff = recv_save_buff;

    // create monitor thread
    pthread_create(&thread_id, NULL, monitor_thread, con);
    con->mon_thread_id = thread_id;

    // return handle
    INFO("connected to '%s', service=%s, handle=%d\n", wc_name, SERVICE_STR(service_id), handle);
    *connect_status = STATUS_INFO_OK;
    return handle;
}

static int p2p2_accept(char * wc_macaddr, int * service, char * user_name)
{
    ERROR("not supported\n");
    return -1;
}

static int p2p2_disconnect(int handle)
{
    con_t * con = NULL;
    int     ret;

    // verify, and set ptr to con
    if (handle < 0 || handle >= MAX_HANDLE || 
        (con = &con_tbl[handle]) == NULL ||
        !con->connected)
    {
        ERROR("invalid, handle=%d connected=%d\n", handle, con ? con->connected : -1);
        return -1;
    }

    // cancel monitor thread
    if (con->mon_thread_id != 0) {
        con->mon_thread_cancel_req = true;
        pthread_join(con->mon_thread_id, NULL);
        con->mon_thread_id = 0;
    }

    // close handle
    ret = close(handle);

    // free allocation and reset fields in con to zero
    free(con->recv_save_buff);
    bzero(con, sizeof(con_t));

    // return status
    INFO("disconnected, handle=%d, ret=%d\n", handle, ret);
    return ret;
}

static int p2p2_send(int handle, void * buff, int len)
{
    int     len_sent;
    con_t * con = NULL;

    // verify, and set ptr to con
    if (handle < 0 || handle >= MAX_HANDLE || 
        (con = &con_tbl[handle]) == NULL ||
        !con->connected || con->failed)
    {
        ERROR("invalid, handle=%d connected=%d failed=%d\n", 
              handle, con ? con->connected : -1, con ? con->failed : -1);
        return -1;
    }

    // send the buff
    pthread_mutex_lock(&con->send_mutex);
    len_sent = send(handle, buff, len, 0);
    pthread_mutex_unlock(&con->send_mutex);
    if (len_sent != len) {
        ERROR("send len_sent=%d len=%d\n", len_sent, len);
        con->failed = true;
        return -1;
    }

    // yield to give other threads, that are waiting for the mutex, a chance to run
    sched_yield();

    // update sent_bytes stat
    if (len_sent > 0) {
        con->stats.sent_bytes += len_sent;
    }

    // return len_sent
    return len_sent;
}

static int p2p2_recv(int handle, void * caller_buff, int caller_buff_len, int mode)
{
    #define MIN(a,b) ((a) < (b) ? (a) : (b))
        
    #define COPY_SAVE_BUFF_TO_CALLER_BUFF(len) \
        do { \
            memcpy(caller_buff, \
                   con->recv_save_buff, \
                   (len)); \
            memmove(con->recv_save_buff,  \
                    con->recv_save_buff + (len),  \
                    con->recv_save_buff_len - (len)); \
            con->recv_save_buff_len -= (len); \
        } while (0)

    con_t * con = NULL;
    int     ret_len, tmp_len;

    // This routine is a bit involved. The reason for the complexity is to
    // support mode RECV_NOWAIT_ALL. This mode will not block; if not enough
    // data is available then RECV_WOULDBLOCK is returned, otherwise the 
    // amount of data returned is the requested caller_buff_len.
    //
    // My first try at implementing this was to combine the MSG_WAITALL and 
    // MSG_DONTWAIT flags. This did not provide the desired functionality.
    //
    // Second attempt was to use ioctl FIONREAD, which returns the number of
    // bytes in the read socket. If there was not enough then RECV_WOULDBLOCK 
    // was returned, otherwise recv would be called for the desired length.
    // This worked well, except that when the other side of the connection
    // disconnected, the FIONREAD does not return an error, causing this side
    // not to disconnect.
    //
    // Third attempt is what we have below. A recv_save_buff is used to internally
    // buffer received data when using the RECV_NOWAIT_ALL mode and the amount of
    // data returned by nonblocking recv call is insufficient.

    // verify, and set ptr to con
    if (handle < 0 || handle >= MAX_HANDLE || 
        (con = &con_tbl[handle]) == NULL ||
        !con->connected || con->failed)
    {
        ERROR("invalid, handle=%d connected=%d failed=%d\n", 
              handle, con ? con->connected : -1, con ? con->failed : -1);
        return RECV_ERROR;
    }

    // check for eof, if so return RECV_EOF
    if (con->recv_eof) {
        return RECV_EOF;
    }

    switch (mode) {
    case RECV_WAIT_ANY:
        // if data is in the recv_save_buffer then
        //   copy the recv_save_buffer to caller_buffer    
        //   return
        // endif
        if (con->recv_save_buff_len) {
            ret_len = MIN(con->recv_save_buff_len, caller_buff_len);
            COPY_SAVE_BUFF_TO_CALLER_BUFF(ret_len);
            break;
        }

        // call recv to wait for any data
        ret_len = recv(handle, caller_buff, caller_buff_len, 0);
        if (ret_len == 0) {
            con->recv_eof = true;
            ret_len = RECV_EOF;
        } else if (ret_len < 0) {
            con->failed = true;
            ret_len = RECV_ERROR;
        }
        break;

    case RECV_NOWAIT_ANY:
        // if data is in the recv_save_buffer then
        //   copy the recv_save_buffer to caller_buffer    
        //   return
        // endif
        if (con->recv_save_buff_len) {
            ret_len = MIN(con->recv_save_buff_len, caller_buff_len);
            COPY_SAVE_BUFF_TO_CALLER_BUFF(ret_len);
            break;
        }

        // call recv, non blocking
        ret_len = recv(handle, caller_buff, caller_buff_len, MSG_DONTWAIT);
        if (ret_len == 0) {
            con->recv_eof = true;
            ret_len = RECV_EOF;
        } else if (ret_len < 0) {
            ret_len = (errno == EWOULDBLOCK ? RECV_WOULDBLOCK : RECV_ERROR);
            if (ret_len == RECV_ERROR) {
                con->failed = true;
            }
        }
        break;

    case RECV_WAIT_ALL:
        // if data is in the recv_save_buffer then
        //   copy the recv_save_buffer to caller_buffer    
        //   if caller_buffer is full then 
        //     return
        //   endif
        // endif
        ret_len = 0;
        if (con->recv_save_buff_len) {
            ret_len = MIN(con->recv_save_buff_len, caller_buff_len);
            COPY_SAVE_BUFF_TO_CALLER_BUFF(ret_len);
            if (ret_len == caller_buff_len) {
                break;
            }
        }

        // call recv, with wait-all, to read the remainder of
        // data to fill the caller_buffer
        tmp_len = recv(handle, caller_buff+ret_len, caller_buff_len-ret_len, MSG_WAITALL);
        if (tmp_len == 0) {
            if (ret_len == 0) {
                con->recv_eof = true;
                ret_len = RECV_EOF;
            } else {
                con->failed = true;
                ret_len = RECV_ERROR;
            }
        } else if (tmp_len != caller_buff_len-ret_len) {
            con->failed = true;
            ret_len = RECV_ERROR;
        } else {
            ret_len = caller_buff_len;
        }
        break;

    case RECV_NOWAIT_ALL:
        // if enough data is in the recv_save_buffer to satisfy the request
        //   copy the recv_save_buffer to caller_buffer    
        //   return
        // endif
        if (con->recv_save_buff_len >= caller_buff_len) {
            ret_len = caller_buff_len;
            COPY_SAVE_BUFF_TO_CALLER_BUFF(ret_len);
            break;
        }

        // call recv, non blocking; recv into the caller_buffer
        tmp_len = recv(handle, 
                       caller_buff+con->recv_save_buff_len, caller_buff_len-con->recv_save_buff_len, 
                       MSG_DONTWAIT);

        // if recv returned eof then
        //   return eof or error 
        // else if recv returned error then
        //   return error
        // else if this recv completes the request then
        //   copy the recv_save_buffer to caller_buffer, and return success
        // else
        //   copy data recvd in caller_buffer to the recv_save_buffer, and return RECV_WOULDBLOCK
        // endif
        if (tmp_len == 0) {
            if (con->recv_save_buff_len == 0) {
                con->recv_eof = true;
                ret_len = RECV_EOF;
            } else {
                con->failed = true;
                ret_len = RECV_ERROR;
            }
        } else if (tmp_len < 0) {
            ret_len = (errno == EWOULDBLOCK ? RECV_WOULDBLOCK : RECV_ERROR);
            if (ret_len == RECV_ERROR) {
                con->failed = true;
            }
        } else if (tmp_len ==  caller_buff_len - con->recv_save_buff_len) {
            COPY_SAVE_BUFF_TO_CALLER_BUFF(con->recv_save_buff_len);
            ret_len = caller_buff_len;
        } else {
            if (con->recv_save_buff_len + tmp_len > MAX_RECV_SAVE_BUFF) {
                ERROR("recv_save_buff overflow, handle=%d, recv_save_buff_len=%d tmp_len=%d\n",
                      handle, con->recv_save_buff_len, tmp_len);
                con->failed = true;
                ret_len = RECV_ERROR;
                break;
            }
            memcpy(con->recv_save_buff+con->recv_save_buff_len,
                   caller_buff+con->recv_save_buff_len,
                   tmp_len);
            con->recv_save_buff_len += tmp_len;
            ret_len = RECV_WOULDBLOCK;
        }
        break;
            
    default: 
        ERROR("invalid mode %d\n", mode);
        con->failed = true;
        ret_len = RECV_ERROR;
        break;
    }    

    // update recvd_bytes stat
    if (ret_len > 0) {
        con->stats.recvd_bytes += ret_len;
    }

    // return ret_len
    return ret_len;
}

static int p2p2_get_stats(int handle, p2p_stats_t * stats)
{
    con_t * con = NULL;

    // verify, and set ptr to con
    if (handle < 0 || handle >= MAX_HANDLE || 
        (con = &con_tbl[handle]) == NULL ||
        !con->connected || con->failed)
    {
        ERROR("invalid, handle=%d connected=%d failed=%d\n", 
              handle, con ? con->connected : -1, con ? con->failed : -1);
        return -1;
    }

    // set return stats buffer to -1 (means no data), and
    // fill in stats values that p2p2 provides
    memset(stats, -1, sizeof(p2p_stats_t));
    stats->sent_bytes = con->stats.sent_bytes;
    stats->recvd_bytes = con->stats.recvd_bytes;

    // return success
    return 0;
}

static int p2p2_monitor_ctl(int handle, int secs)
{
    con_t * con = NULL;

    // verify, and set ptr to con
    if (handle < 0 || handle >= MAX_HANDLE || 
        (con = &con_tbl[handle]) == NULL ||
        !con->connected || con->failed)
    {
        ERROR("invalid, handle=%d connected=%d failed=%d\n", 
              handle, con ? con->connected : -1, con ? con->failed : -1);
        return -1;
    }

    // update monitor settings
    con->mon_secs = secs;

    // return success
    return 0;
}

static void * monitor_thread(void * cx)
{
    con_t * con = cx;

    #define DELTA(fld) (stats.fld - stats_last.fld)

    stats_t  stats, stats_last;
    uint64_t stats_time_ms, stats_last_time_ms;
    int      interval_ms, i;
    int      print_header_count=0;
    bool     first_loop = true;

    // init to avoid compiler warning
    stats_last = con->stats;
    stats_last_time_ms = MILLISEC_TIMER;

    // loop until thread is cancelled
    while (true) {
        // if sleep time is 0 then 
        //   wait until non zero
        //   reset print_header_count, so the header will print immedeatley
        //   make copy of stats, for last_stats
        // endif
        if (con->mon_secs == 0 || first_loop) {
            while (con->mon_secs == 0) {
                sleep_ms(100);
                if (con->mon_thread_cancel_req) {
                    goto done;
                }
            }
            print_header_count = 0;
            stats_last = con->stats;
            stats_last_time_ms = MILLISEC_TIMER;
            first_loop = false;
        }

        // print header
        if ((print_header_count++ % 10) == 0) {
            PRINTF("\n");
            PRINTF("Send  Recv\n");
            PRINTF("Mb/S  Mb/S\n");
        }

        // sleep for caller's desired interval
        for (i = 0; i < con->mon_secs; i++) {
            sleep_ms(1000);
            if (con->mon_thread_cancel_req) {
                goto done;
            }
        }

        // get stats now
        stats = con->stats;
        stats_time_ms = MILLISEC_TIMER;

        // print changes since stats_last
        interval_ms = stats_time_ms - stats_last_time_ms;
        PRINTF("%5.1f %5.1f\n",
               (double)DELTA(sent_bytes) * 8 / (interval_ms * 1000),
               (double)DELTA(recvd_bytes) * 8 / (interval_ms * 1000));

        // save stats in stats_last
        stats_last = stats;
        stats_last_time_ms = stats_time_ms;
    }

done:
    // terminate
    return NULL;
}

static void p2p2_debug_con(int handle)
{
    PRINTF("p2p2_debug_con is not supported\n");
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

