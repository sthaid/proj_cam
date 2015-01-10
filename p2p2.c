#include "wc.h"

#include <sys/ioctl.h>

// 
// defines
//

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
    bool            in_use;
    int             handle;
    int             fd;
    int             ref_cnt;  // xxx new
    bool            failed;
    bool            recv_eof;
    bool            disconnecting;  // xxx new
    pthread_mutex_t send_mutex;
    char            recv_save_buff[MAX_RECV_SAVE_BUFF];  // XXX maybe allocate
    uint32_t        recv_save_buff_len;
    stats_t         stats;
    pthread_t       mon_thread_id;
    bool            mon_thread_cancel_req;
    int             mon_secs;
} con_t;

//
// variables
//

static int             max_con;
static con_t         * con_tbl;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

//
// prototypes
//

static int p2p2_init(int max_con_arg);
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

p2p_routines_t p2p2 = { p2p2_init,
                        p2p2_connect,
                        p2p2_accept,
                        p2p2_disconnect,
                        p2p2_send,
                        p2p2_recv,
                        p2p2_get_stats,
                        p2p2_monitor_ctl,
                        p2p2_debug_con };

// -------------------------------------------------------------------------------

static int p2p2_init(int max_con_arg)
{
    // verify max_con is power of 2
#if 0
    if (max_con != 1 && max_con != 2 && max_con != 4 && max_con != 8 && max_con != 16 && 
        max_con != 32 && max_con != 64 && max_con != 128 && max_con != 256 && max_con != 512 && 
        max_con != 1024)
#endif
    if (__builtin_popcount(max_con_arg) != 1 || max_con_arg > 1024 || max_con_arg < 0) {
        ERROR("invalid max_con %d\n", max_con_arg);
        return -1;
    }

    // allocate con_tbl
    INFO("XXX SIZE OF ENT %d\n", (int)sizeof(con_tbl[0]));
    max_con = max_con_arg;
    con_tbl = calloc(max_con, sizeof(con_tbl[0]));
    if (con_tbl == NULL) {
        return -1;
    }

    // return success
    return 0;
}

static int p2p2_connect(char * user_name, char * password, char * wc_name, int service_id, int * connect_status)
{
    char        service[100];
    int         handle;
    con_t     * con;
    pthread_t   thread_id;
    int         fd, con_tbl_idx, tries = 0;

    static int  handle_upper_val;

try_again:
    // connect to admin_server, with 'wccon' service
    sprintf(service, "wccon %s %d", wc_name, service_id);
    fd = connect_to_admin_server(user_name, password, service, connect_status);

    // retry on WC_ADDR_NOT_AVAIL error
    if (fd < 0 && *connect_status == STATUS_ERR_WC_ADDR_NOT_AVAIL && ++tries <= 5) {
        sleep(1);
        goto try_again;
    }
    if (fd < 0) {
        ERROR("failed connect_to_admin_server, %s\n", status2str(*connect_status));
        return -1;
    }

    // allocate and init con; only non-zero fields are set
    pthread_mutex_lock(&mutex);
    for (con_tbl_idx = 0; con_tbl_idx < max_con; con_tbl_idx++) {
        if (con_tbl[con_tbl_idx].in_use == false) {
            break;
        }
    }
    if (con_tbl_idx == max_con) {
        ERROR("no free con_tbl entry\n");
        *connect_status = STATUS_ERR_TOO_MANY_CONNECTIONS;
        pthread_mutex_unlock(&mutex);
        return -1;
    }

    handle = (con_tbl_idx + __sync_add_and_fetch(&handle_upper_val,max_con)) & 0x7fffffff;
    if (handle == 0) {
        handle = (con_tbl_idx + __sync_add_and_fetch(&handle_upper_val,max_con)) & 0x7fffffff;
    }
    INFO("XXX HANDLE %d\n", handle);

    con = &con_tbl[con_tbl_idx];
    bzero(con, sizeof(con_t));
    con->in_use = true;
    con->handle = handle;
    con->fd     = fd;
    pthread_mutex_init(&con->send_mutex,NULL);
    pthread_mutex_unlock(&mutex);

    // create monitor thread
    pthread_create(&thread_id, NULL, monitor_thread, con);
    con->mon_thread_id = thread_id;

    // return handle
    INFO("connected to '%s', service=%s, handle=0x%x\n", wc_name, SERVICE_STR(service_id), handle);
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
    con_t * con;

    // verify, set ptr to con, and set disconnecting flag
    pthread_mutex_lock(&mutex);
    con = &con_tbl[handle & (max_con-1)];
    if (handle != con->handle || !con->in_use || con->disconnecting) {
        ERROR("invalid handle=0x%x con handle=0x%x in_use=%d disconnecting=%d\n",
              handle, con->handle, con->in_use, con->disconnecting);
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    con->disconnecting = true;
    pthread_mutex_unlock(&mutex);

    // cancel monitor thread
    if (con->mon_thread_id != 0) {
        con->mon_thread_cancel_req = true;
        pthread_join(con->mon_thread_id, NULL);
        con->mon_thread_id = 0;
    }

    // shutdown & close socket
    shutdown(con->fd, SHUT_RDWR);
    close(con->fd);

    // wait for con->ref_cnt to become zero
    while (con->ref_cnt != 0) {
        sleep_ms(1);
    }

    // set con to zero
    pthread_mutex_lock(&mutex);
    bzero(con, sizeof(con_t));
    pthread_mutex_unlock(&mutex);

    // return success
    INFO("disconnected, handle=0x%x\n", handle);
    return 0;
}

static int p2p2_send(int handle, void * buff, int len)
{
    int     len_sent;
    con_t * con;

    // verify, set ptr to con, and increment ref_cnt
    pthread_mutex_lock(&mutex);
    con = &con_tbl[handle & (max_con-1)];
    if (handle != con->handle || !con->in_use || con->disconnecting || con->failed) {
        ERROR("invalid handle=0x%x con handle=0x%x in_use=%d disconnecting=%d failed=%d\n",
              handle, con->handle, con->in_use, con->disconnecting, con->failed);
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    con->ref_cnt++;
    pthread_mutex_unlock(&mutex);

    // send the buff
    pthread_mutex_lock(&con->send_mutex);
    len_sent = send(con->fd, buff, len, 0);
    pthread_mutex_unlock(&con->send_mutex);
    if (len_sent != len) {
        ERROR("send len_sent=%d len=%d\n", len_sent, len);
        con->failed = true;
        pthread_mutex_lock(&mutex); 
        con->ref_cnt--; 
        pthread_mutex_unlock(&mutex); 
        return -1;
    }

    // yield to give other threads, that are waiting for the mutex, a chance to run
    sched_yield();

    // update sent_bytes stat
    if (len_sent > 0) {
        con->stats.sent_bytes += len_sent;
    }

    // decrement con->ref_cnt
    pthread_mutex_lock(&mutex); 
    con->ref_cnt--; 
    pthread_mutex_unlock(&mutex); 

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

    con_t * con;
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

    // verify, set ptr to con, and increment ref_cnt
    pthread_mutex_lock(&mutex);
    con = &con_tbl[handle & (max_con-1)];
    if (handle != con->handle || !con->in_use || con->disconnecting || con->failed) {
        ERROR("invalid handle=0x%x con handle=0x%x in_use=%d disconnecting=%d failed=%d\n",
              handle, con->handle, con->in_use, con->disconnecting, con->failed);
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    con->ref_cnt++;
    pthread_mutex_unlock(&mutex);

    // check for eof, if so return RECV_EOF
    if (con->recv_eof) {
        pthread_mutex_lock(&mutex); 
        con->ref_cnt--; 
        pthread_mutex_unlock(&mutex); 
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
        ret_len = recv(con->fd, caller_buff, caller_buff_len, 0);
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
        ret_len = recv(con->fd, caller_buff, caller_buff_len, MSG_DONTWAIT);
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
        tmp_len = recv(con->fd, caller_buff+ret_len, caller_buff_len-ret_len, MSG_WAITALL);
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
        tmp_len = recv(con->fd, 
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
                ERROR("recv_save_buff overflow, handle=0x%x, recv_save_buff_len=%d tmp_len=%d\n",
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

    // decrement reference count
    pthread_mutex_lock(&mutex); 
    con->ref_cnt--; 
    pthread_mutex_unlock(&mutex); 

    // return ret_len
    return ret_len;
}

static int p2p2_get_stats(int handle, p2p_stats_t * stats)
{
    con_t * con;

    // verify, set ptr to con, and increment ref_cnt
    pthread_mutex_lock(&mutex);
    con = &con_tbl[handle & (max_con-1)];
    if (handle != con->handle || !con->in_use || con->disconnecting || con->failed) {
        ERROR("invalid handle=0x%x con handle=0x%x in_use=%d disconnecting=%d failed=%d\n",
              handle, con->handle, con->in_use, con->disconnecting, con->failed);
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    con->ref_cnt++;
    pthread_mutex_unlock(&mutex);

    // set return stats buffer to -1 (means no data), and
    // fill in stats values that p2p2 provides
    memset(stats, -1, sizeof(p2p_stats_t));
    stats->sent_bytes = con->stats.sent_bytes;
    stats->recvd_bytes = con->stats.recvd_bytes;

    // decrement reference count
    pthread_mutex_lock(&mutex); 
    con->ref_cnt--; 
    pthread_mutex_unlock(&mutex); 

    // return success
    return 0;
}

static int p2p2_monitor_ctl(int handle, int secs)
{
    con_t * con;

    // verify, set ptr to con, and increment ref_cnt
    pthread_mutex_lock(&mutex);
    con = &con_tbl[handle & (max_con-1)];
    if (handle != con->handle || !con->in_use || con->disconnecting || con->failed) {
        ERROR("invalid handle=0x%x con handle=0x%x in_use=%d disconnecting=%d failed=%d\n",
              handle, con->handle, con->in_use, con->disconnecting, con->failed);
        pthread_mutex_unlock(&mutex);
        return -1;
    }
    con->ref_cnt++;
    pthread_mutex_unlock(&mutex);

    // update monitor settings
    con->mon_secs = secs;

    // decrement reference count
    pthread_mutex_lock(&mutex); 
    con->ref_cnt--; 
    pthread_mutex_unlock(&mutex); 

    // return success
    return 0;
}

static void * monitor_thread(void * cx)
{
    #define DELTA(fld) (stats.fld - stats_last.fld)

    con_t *  con = cx;
    stats_t  stats, stats_last;
    uint64_t stats_time_ms, stats_last_time_ms;
    int      interval_ms, i;
    int      print_header_count=0;
    bool     first_loop = true;

    // increment ref_count
    pthread_mutex_lock(&mutex);
    con->ref_cnt++;
    pthread_mutex_unlock(&mutex);

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
    // decrement ref_cnt
    pthread_mutex_lock(&mutex); 
    con->ref_cnt--; 
    pthread_mutex_unlock(&mutex); 

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

