#include "wc.h"

#include <sys/ioctl.h>

// 
// defines
//

#define MAX_HANDLE 200  // android uses a lot of fds

#define MILLISEC_TIMER  (microsec_timer() / 1000)

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
    pthread_mutex_t send_mutex;
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
    con_t   * con;
    pthread_t thread_id;
    int       tries = 0;

try_again:
    // connect to cloud_server, with 'wccon' service
    sprintf(service, "wccon %s %d", wc_name, service_id);
    handle = connect_to_cloud_server(user_name, password, service, connect_status);

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
        ERROR("unable to connect to cloud_server, %s\n", status2str(*connect_status));
        return -1;
    }

    // init con, non-zero fields only are set
    con = &con_tbl[handle];
    bzero(con, sizeof(con_t));
    con->connected = true;
    pthread_mutex_init(&con->send_mutex,NULL);

    // create monitor thread
    pthread_create(&thread_id, NULL, monitor_thread, con);
    con->mon_thread_id = thread_id;

    // return handle
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
        INFO("XXX JOINED WITH MONIOR THREAD\n");
        con->mon_thread_id = 0;
    }

    // close handle
    ret = close(handle);

    // reset fields in con to zero
    bzero(con, sizeof(con_t));

    // return status
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

static int p2p2_recv(int handle, void * buff, int len, int mode)
{
    int     len_recvd;
    con_t * con = NULL;
    int     flags;
    int     count;

    // verify, and set ptr to con
    if (handle < 0 || handle >= MAX_HANDLE || 
        (con = &con_tbl[handle]) == NULL ||
        !con->connected || con->failed)
    {
        ERROR("invalid, handle=%d connected=%d failed=%d\n", 
              handle, con ? con->connected : -1, con ? con->failed : -1);
        return RECV_ERROR;
    }

    // XXX RECV_NOWAIT_ALL is not working because the FIONREAD ioctl does
    // not indicate when the other side has closed its socket; for now 
    // replace with RECV_WAIT_ALL
    if (mode == RECV_NOWAIT_ALL) {
        mode = RECV_WAIT_ALL;
    }

    // if mode is non blocking recv of all data then check that
    // the socket has at least len bytes available
    if (mode == RECV_NOWAIT_ALL) {
        if (ioctl(handle, FIONREAD, &count) < 0) {
            ERROR("FIONREAD failed, %s\n", strerror(errno));
            con->failed = true;
            return RECV_ERROR;
        }
        if (count < len) {
            return RECV_WOULDBLOCK;
        }
    }

    // convert mode arg to flags to be passed to recv
    if (mode == RECV_WAIT_ANY) {
        flags = 0;
    } else if (mode == RECV_WAIT_ALL) {
        flags = MSG_WAITALL;
    } else if (mode == RECV_NOWAIT_ANY  || mode == RECV_NOWAIT_ALL) {
        flags = MSG_DONTWAIT;
    } else {
        ERROR("recv invalid mode %d\n", mode);
        con->failed = true;
        return RECV_ERROR;
    }

    // call recv
    len_recvd = recv(handle, buff, len, flags);
    if (len_recvd < 0) {
        return errno == EWOULDBLOCK ? RECV_WOULDBLOCK : RECV_ERROR;
    }

    // verify len_recvd is valid for the modes which should return the entire len requested
    if ((mode == RECV_WAIT_ALL || mode == RECV_NOWAIT_ALL) && len_recvd != len && len_recvd != RECV_EOF) {
        ERROR("recv len_recvd=%d len=%d mode=%d %s\n", len_recvd, len, mode, strerror(errno));
        con->failed = true;
        return RECV_ERROR;
    }

    // update recvd_bytes stat
    con->stats.recvd_bytes += len_recvd;

    // return len_recvd
    return len_recvd;
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
            INFO("\n");
            INFO("Send  Recv\n");
            INFO("Mb/S  Mb/S\n");
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
        INFO("%5.1f %5.1f\n",
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
    INFO("p2p2_debug_con is not supported\n");
}

static void sleep_ms(int ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

