#include "wc.h"

// TBD - doc on setting tuning defines
// TBD - tcpdump, verify no icmp packets, and verify no ip fragmentation, verify MAX_DATA 1472
// TBD - add code to connect_common to validate the mutexes are working, (trylock and release)

// TBD LATER - p2p_send with iov
// TBD LATER - may want to progressively back off until a max resend time is reached, see TIME_RESEND_MS

// 
// defines
//

#define MAX_CON                       8      // must be power of 2
#define MAX_DATA_DGRAM                100    // max is 1023
#define MAX_DATA                      (1472-offsetof(dgram_t,u.p2p_data.data)) 
#define MAX_RECVBUFF                  1000000
#define MAX_ACK                       10
#define MAX_DATA_DGRAM_ON_WFA_LIST    20

#define TIME_DELTA_MS                 50
#define TIME_DGRAM_RECV_TOUT_MS       10000
#define TIME_DISCONNECT_TOUT_MS       10000
#define TIME_SEND_STATS_MS            1000
#define TIME_RESEND_MS                500
#define TIME_ACK_SEND_TOUT_MS         50

#define STATE_CONNECTING        1
#define STATE_CONNECTED         2
#define STATE_ERROR             3
#define STATE_DISCONNECTING     4
#define STATE_DISCONNECTED      5 

#define CON_STATE_STR(x) \
    ((x) == STATE_CONNECTING     ? "STATE_CONNECTING"     : \
     (x) == STATE_CONNECTED      ? "STATE_CONNECTED"      : \
     (x) == STATE_ERROR          ? "STATE_ERROR"          : \
     (x) == STATE_DISCONNECTING  ? "STATE_DISCONNECTING"  : \
     (x) == STATE_DISCONNECTED   ? "STATE_DISCONNECTED"     \
                                 : "STATE_????")

#define ON_FREE_LIST              1
#define ON_SEND_LIST              2
#define ON_WAITING_FOR_ACK_LIST   3

#define RECV_EOD_OFFSET_NOT_SET  -1

#define P2P_DATA_ID_GEN_NEXT(id) ((id) + 1024)
#define P2P_DATA_ID_TO_IDX(id)   (((id) & 1023) - 1)

#define LOCAL_RECVBUFF_FIRST_OFFSET(con) (TAILQ_FIRST(&con->recvbuff_data_valid_list_head)->offset)
#define LOCAL_RECVBUFF_FIRST_LENGTH(con) (TAILQ_FIRST(&con->recvbuff_data_valid_list_head)->length)

#define CON_STATE_CHANGE(con, new_state) \
    do { \
        if ((new_state) != (con)->state) { \
            if ((con)->state == STATE_CONNECTED) { \
                pthread_cond_signal(&(con)->recv_data_avail_cond); \
            } \
            (con)->state = (new_state); \
            INFO("%"PRId64" is now %s\n", (con)->con_id, CON_STATE_STR((con)->state)); \
        } \
    } while (0)

#ifdef DEBUG_DGRAM
  #define DEBUG_PRINT_DGRAM(a,b,c) debug_print_dgram(a,b,c)
#else
  #define DEBUG_PRINT_DGRAM(a,b,c) 
#endif

#define TAILQ_LENGTH(name,head,field) \
    ({ \
        struct name * x; \
        int length = 0;  \
        TAILQ_FOREACH(x, head, field) length++; \
        length; \
    })

#define MILLISEC_TIMER  (microsec_timer() / 1000)

//
// typedefs
//

typedef struct {
    dgram_t dgram;
    char    dgram_data[MAX_DATA];
} dgram_storage_t;

typedef struct data_dgram_entry_s {
    dgram_t  dgram;
    char     dgram_data[MAX_DATA];
    int      send_count;
    uint64_t time_resend_ms;  
    int      on_list;
    TAILQ_ENTRY(data_dgram_entry_s) entries; 
} data_dgram_entry_t;

typedef struct recvbuff_data_valid_entry_s {
    uint64_t offset;
    uint64_t length; 
    TAILQ_ENTRY(recvbuff_data_valid_entry_s) entries;
} recvbuff_data_valid_entry_t;

typedef struct con_s {
    int                  state;
    uint64_t             con_id;
    int                  sfd;
    struct sockaddr_in   peer_addr;

    uint64_t             time_last_dgram_recvd_ms;
    uint64_t             time_last_stats_dgram_sent_ms;

    pthread_t recv_dgram_thread_id; 
    pthread_t poll_thread_id;  
    pthread_t monitor_thread_id;

    bool con_resp_recvd; 
    bool send_in_prog;
    bool recv_in_prog;

    uint64_t send_offset;
    uint64_t remote_recvbuff_offset;

    data_dgram_entry_t data_dgram_array[MAX_DATA_DGRAM];
    TAILQ_HEAD(th1, data_dgram_entry_s) data_dgram_free_list_head; 
    TAILQ_HEAD(th2, data_dgram_entry_s) data_dgram_send_list_head; 
    TAILQ_HEAD(th3, data_dgram_entry_s) data_dgram_waiting_for_ack_list_head;  
    int num_data_dgrams_on_waiting_for_ack_list; 

    char recvbuff[MAX_RECVBUFF];
    TAILQ_HEAD(th4, recvbuff_data_valid_entry_s) recvbuff_data_valid_list_head;  
    pthread_cond_t recv_data_avail_cond;  
    uint64_t recv_eod_offset;  
    int recv_cond_wait_len;

    int      ack[MAX_ACK];  
    int      ack_tail;  
    int      ack_unsent_count; 
    uint64_t ack_oldest_unsent_time_ms;  
    uint64_t last_sent_local_recvbuff_offset;  

    int monitor_secs;

    p2p_stats_t stats;
} con_t;

//
// variables
//

pthread_mutex_t connect_mutex = PTHREAD_MUTEX_INITIALIZER;

con_t         * con_tbl[MAX_CON];
int             con_handle[MAX_CON];
pthread_mutex_t disconnect_mutex[MAX_CON];
pthread_mutex_t send_mutex[MAX_CON];
pthread_mutex_t mutex[MAX_CON];

//
// prototypes
//

int p2p1_connect(char * user_name, char * password, char * wc_name, int service, int * connect_status);
int p2p1_accept(char * wc_macaddr, int * service, char * user_name);
int connect_common(int sfd, struct sockaddr_in * peer_addr, uint64_t con_id, int * status);
int p2p1_disconnect(int handle);
void free_con(int con_tbl_idx);
int p2p1_send(int handle, void * buff, int len);
int p2p1_recv(int handle, void * buff, int len, int mode);
void thread_cleanup_handler(void * cx);
void * recv_dgram_thread(void * cx);
void * poll_thread(void * cx);
int p2p1_get_stats(int handle, p2p_stats_t * stats);
int p2p1_monitor_ctl(int handle, int secs);
void * monitor_thread(void * cx);
void send_data_dgram_list(con_t * con, uint64_t time_ms, char * debug_str); 
void process_acks(con_t * con, int * ack, int max, uint64_t new_remote_recvbuff_offset);
void send_ack(con_t * con, uint64_t time_ms, char * debug_str);
void send_stats(con_t * con, uint64_t time_ms);
int send_dgram(con_t * con, void * dgram, int length);
bool verify_recvd_dgram(dgram_t * dgram,int length,int dgid1,int dgid2,int dgid3,int dgid4,int dgid5,
    uint64_t con_id, char * s, int slen);
int dgram_expected_length(dgram_t * dgram);
void sleep_ms(int ms, pthread_mutex_t * mutex);
int get_local_addr(struct sockaddr_in * local_addr);
void p2p1_debug_con(int handle);
void debug_print_dgram(bool recv, dgram_t * dg, int len);

//
// p2p routines
//

p2p_routines_t p2p1 = { p2p1_connect,
                        p2p1_accept,
                        p2p1_disconnect,
                        p2p1_send,
                        p2p1_recv,
                        p2p1_get_stats,
                        p2p1_monitor_ctl,
                        p2p1_debug_con };

// -----------------  P2P_CONNECT & P2P_ACCEPT  ------------------------

int p2p1_connect(char * user_name, char * password, char * wc_name, int service, int * connect_status)
{
    struct sockaddr_in admin_server_addr;
    struct sockaddr_in local_addr;
    dgram_t            dgram;
    int                sfd = -1;
    int                ret, i, handle;
    struct sockaddr_in peer_addr;
    uint64_t           con_id;
    char               s[100];
    socklen_t          addrlen = sizeof(struct sockaddr_in);
    socklen_t          fromlen;
    struct sockaddr_in from;
    bool               got_p2p_info = false;
    dgram_uid_t        dgram_uid;
    bool               valid_dgram_recvd;
    uint64_t           start_us;

    // preset connect_status return 
    *connect_status = STATUS_ERR_GENERAL_FAILURE;

    // get address of ADMIN_SERVER
    ret =  getsockaddr(ADMIN_SERVER_HOSTNAME, ADMIN_SERVER_DGRAM_PORT, SOCK_DGRAM, IPPROTO_UDP, &admin_server_addr);
    if (ret < 0) {
        ERROR("failed to get address of %s\n",  ADMIN_SERVER_HOSTNAME);
        *connect_status = STATUS_ERR_GET_SERVER_ADDR;
        goto error_ret;
    }
    INFO("address of %s is %s\n",
         ADMIN_SERVER_HOSTNAME, 
         sock_addr_to_str(s, sizeof(s), (struct sockaddr*)&admin_server_addr));

    // create socket 
    sfd = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_UDP);
    if (sfd == -1) {
        ERROR("socket\n");
        *connect_status = STATUS_ERR_CREATE_SOCKET;
        goto error_ret;
    }

    // bind  XXX still wonder why I need get_local_addr, just use INADDR_ANY instead
    ret = get_local_addr(&local_addr);
    if (ret == -1) {
        ERROR("get_local_addr %s\n", strerror(errno));
        *connect_status = STATUS_ERR_GET_LOCAL_ADDR;
        goto error_ret;
    }
    ret = bind(sfd, (struct sockaddr*)&local_addr, sizeof(local_addr));
    if (ret == -1) {
        ERROR("bind %s\n", strerror(errno));
        *connect_status = STATUS_ERR_BIND_LOCAL_ADDR;
        goto error_ret;
    }

    // repeat tries to receive the peer to peer info from admin_server
    // that is needed to establish the connection to the webcam
    for (i = 0; i < 5; i++) {
        // generate the dgram_uid, which is used to validate the response 
        dgram_uid = dgram_uid_gen();          

        // construct connect request dgram
        bzero(&dgram,sizeof(dgram));
        dgram.id = DGRAM_ID_CONNECT_REQ;
        strcpy(dgram.u.connect_req.user_name, user_name);
        strcpy(dgram.u.connect_req.password, password);
        dgram.u.connect_req.service = service;
        strncpy(dgram.u.connect_req.wc_name, wc_name, MAX_WC_NAME);
        ret = getsockname(sfd, (struct sockaddr *)&dgram.u.connect_req.client_addr_behind_nat, &addrlen);
        if (ret == -1) {
            ERROR("getsockname");
            *connect_status = STATUS_ERR_GETSOCKNAME;
            goto error_ret;
        }
        dgram.u.connect_req.dgram_uid = dgram_uid;

        // debug print the dgram we are about to send
        DEBUG_PRINT_DGRAM(false, &dgram, offsetof(dgram_t,u.connect_req.dgram_end));

        // send the connect request dgram
        ret = sendto(sfd, &dgram, offsetof(dgram_t,u.connect_req.dgram_end), 0,
                     (struct sockaddr *)&admin_server_addr, sizeof(admin_server_addr));
        if (ret != offsetof(dgram_t,u.connect_req.dgram_end)) {
            ERROR("send connect");
            *connect_status = STATUS_ERR_SENDTO;
            goto error_ret;
        }

        // receive the response, give up after 2 seconds
        valid_dgram_recvd = false;
        start_us = microsec_timer();
        while (true) {
            // if times up then break
            if (microsec_timer() - start_us > 2000000) {
                break;
            }

            // recv dgram with 1 sec tout
            set_sock_opts(sfd, -1, -1, -1, 1000000);
            fromlen = sizeof(from);
            ret = recvfrom(sfd, &dgram, sizeof(dgram_t), 0, (struct sockaddr*)&from, &fromlen);
            set_sock_opts(sfd, -1, -1, -1, 0);
            if (ret < 0 && errno == EAGAIN) {
                continue;
            }
            if (ret < 0) {
                ERROR("recv return error %s\n", strerror(errno));
                *connect_status = STATUS_ERR_RECVFROM;
                goto error_ret;
            }

            // debug print the received dgram
            DEBUG_PRINT_DGRAM(true, &dgram, ret);

            // if dgram is not from server then receive again
            if (fromlen != sizeof(struct sockaddr_in) ||
                memcmp(&admin_server_addr, &from, sizeof(from)) != 0) 
            {
                ERROR("ignoring dgram - from unexpected address\n");
                continue;
            }

            // if the dgram is not valid then receive again
            if (!verify_recvd_dgram(&dgram,ret,DGRAM_ID_CONNECT_ACTIVATE,DGRAM_ID_CONNECT_REJECT,0,0,0,0,s,sizeof(s))) {
                ERROR("ignoring dgram - verify_recvd_dgram: %s\n", s);
                continue;
            }

            // if the recvd dgram_uid doesn't match what was sent then receive again
            if ((dgram.id == DGRAM_ID_CONNECT_ACTIVATE && 
                 dgram_uid_equal(&dgram.u.connect_activate.dgram_uid, &dgram_uid) == false) ||
                (dgram.id == DGRAM_ID_CONNECT_REJECT && 
                 dgram_uid_equal(&dgram.u.connect_reject.dgram_uid, &dgram_uid) == false))
            {
                ERROR("ignoring dgram - dgram_uid doesn't match what was sent\n");
                continue;
            }

            // the recvd dgram is valid, so break out of this loop
            valid_dgram_recvd = true;
            break;
        }

        // if we haven't received a response then send the request again
        if (!valid_dgram_recvd) {
            ERROR("did not recive a valid dgram response, try send CONNECT_REQ again\n");
            continue;
        }

        // if response is rejection then return errror
        if (dgram.id == DGRAM_ID_CONNECT_REJECT) {
            if (dgram.u.connect_reject.status == STATUS_ERR_WC_ADDR_NOT_AVAIL) {
                INFO("recvd dgram rejection, status WC_ADDR_NOT_AVAIL, retrying in 1 sec\n");
                sleep(1);
                continue;
            }
            ERROR("recvd dgram rejection, status %s\n", status2str(dgram.u.connect_reject.status));
            *connect_status = dgram.u.connect_reject.status;
            goto error_ret;
        }

        // we got the needed peer_addr and con_id
        peer_addr = dgram.u.connect_activate.wc_addr;
        con_id    = dgram.u.connect_activate.con_id;
        got_p2p_info = true;
        break;
    }

    // if we did not get the info then error
    if (!got_p2p_info) {
        ERROR("failed to receive p2p connect info from server\n");
        *connect_status = STATUS_ERR_NO_RESPONSE_FROM_SERVER;
        goto error_ret;
    }

    // call connect common;  on error sfd is closed by connect_common
    handle = connect_common(sfd, &peer_addr, con_id, connect_status);
    if (handle < 0) {
        ERROR("%"PRId64" connect_common failed\n", con_id);
        sfd = -1;
        goto error_ret;
    }

    // return handle
    *connect_status = STATUS_INFO_OK;
    return handle;

    // error return path
error_ret:
    if (sfd != -1) {
        close(sfd);
        sfd = -1;
    }
    return -1;
}

int p2p1_accept(char * wc_macaddr, int * service, char * user_name)
{
    #define MAX_DGRAM_UID_TBL 30

    struct sockaddr_in   local_addr;
    struct sockaddr_in   admin_server_addr;
    struct sockaddr_in   new_admin_server_addr;
    int                  sfd = -1;
    int                  ret, handle, status, i;
    dgram_t              dgram;
    struct sockaddr_in   peer_addr;
    uint64_t             con_id;
    char                 s[100];
    socklen_t            addrlen = sizeof(struct sockaddr_in);
    socklen_t            fromlen;
    struct sockaddr_in   from;
    uint64_t             time_last_getsockaddr_ms;
    bool                 connect_activate_dgram_recvd;
    uint64_t             start_us;
    bool                 retry = false;

    static dgram_uid_t   dgram_uid_tbl[MAX_DGRAM_UID_TBL];

try_again:
    // close the socket and delay one second when retrying
    if (sfd != -1) {
        close(sfd);
        sfd = -1;
    }
    if (retry) {
        sleep(1);
    }
    retry = true;

    // preset returns
    *service = SERVICE_INVALID;
    user_name[0] = '\0';

    // create socket for to communicate with ADMIN_SERVER_HOSTNAME
    sfd = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_UDP);
    if (sfd == -1) {
        ERROR("socket %s\n", strerror(errno));
        goto try_again;
    }

    // bind to local_addr  
    ret = get_local_addr(&local_addr);
    if (ret == -1) {
        ERROR("get_local_addr %s\n", strerror(errno));
        goto try_again;
    }
    ret = bind(sfd, (struct sockaddr*)&local_addr, sizeof(local_addr));
    if (ret == -1) {
        ERROR("bind %s\n", strerror(errno));
        goto try_again;
    }

    // get address of ADMIN_SERVER_HOSTNAME
    while (true) {
        ret = getsockaddr(ADMIN_SERVER_HOSTNAME, ADMIN_SERVER_DGRAM_PORT, SOCK_DGRAM, IPPROTO_UDP, &admin_server_addr);
        if (ret < 0) {
            ERROR("failed to get address of %s, retry in 10 secs\n", ADMIN_SERVER_HOSTNAME);
            sleep(10);
            continue;
        }
        time_last_getsockaddr_ms = MILLISEC_TIMER;
        INFO("address of %s is %s\n", 
            ADMIN_SERVER_HOSTNAME, 
            sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&admin_server_addr));
        break;
    }

    // loop until we get a peer_addr and conn_id from admin_server
    while (true) {
        // every 10 minutes try to get an updated address for ADMIN_SERVER_HOSTNAME
        if (MILLISEC_TIMER - time_last_getsockaddr_ms > 10*60000) {
            time_last_getsockaddr_ms = MILLISEC_TIMER;
            ret = getsockaddr(ADMIN_SERVER_HOSTNAME, ADMIN_SERVER_DGRAM_PORT, SOCK_DGRAM, IPPROTO_UDP, &new_admin_server_addr);
            if (ret == 0 && memcmp(&new_admin_server_addr, &admin_server_addr, sizeof(struct sockaddr_in)) != 0) {
                admin_server_addr = new_admin_server_addr;
                INFO("address of %s has changed to %s\n", 
                     ADMIN_SERVER_HOSTNAME, 
                     sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&admin_server_addr));
            }
        }

        // construct the wc_announce dgram
        bzero(&dgram, sizeof(dgram));
        dgram.id = DGRAM_ID_WC_ANNOUNCE;
        strncpy(dgram.u.wc_announce.wc_macaddr, wc_macaddr, MAX_WC_MACADDR);
        ret = getsockname(sfd, (struct sockaddr *)&dgram.u.wc_announce.wc_addr_behind_nat, &addrlen);
        if (ret == -1) {
            ERROR("getsockname\n");
            goto try_again;
        }
        dgram.u.wc_announce.version = VERSION;

        // debug print the dgram we are about to send
        DEBUG_PRINT_DGRAM(false, &dgram, offsetof(dgram_t,u.wc_announce.dgram_end));

        // send wc announce
        ret = sendto(sfd, &dgram, offsetof(dgram_t,u.wc_announce.dgram_end), 0,
                     (struct sockaddr *)&admin_server_addr, sizeof(admin_server_addr));
        if (ret != offsetof(dgram_t,u.wc_announce.dgram_end)) {
            ERROR("send announce to %s, %s\n", 
                  sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&admin_server_addr), 
                  strerror(errno));
            goto try_again;
        }

        // receive the response, give up after P2P_ANNOUNCE_INTVL, 
        // so that another ANNOUNCE will be sent
        connect_activate_dgram_recvd = false;
        start_us = microsec_timer();
        while (true) {
            // if it has been more than P2P_ANNOUNCE_INTVL_US then break out of this 
            // loop so that another ANNOUNCE will be sent
            if (microsec_timer() - start_us > P2P_ANNOUNCE_INTVL_US) {
                break;
            }

            // receive on the socket with 1 sec timeout, 
            set_sock_opts(sfd, -1, -1, -1, 1000000);
            fromlen = sizeof(from);
            ret = recvfrom(sfd, &dgram, sizeof(dgram_t) , 0, (struct sockaddr *)&from, &fromlen);
            set_sock_opts(sfd, -1, -1, -1, 0);
            if (ret < 0 && errno == EAGAIN) {
                continue;
            }
            if (ret < 0) {
                ERROR("recvfrom\n");
                goto try_again;
            }

            // debug print the received dgram
            DEBUG_PRINT_DGRAM(true, &dgram, ret);

            // if dgram is not from server then receive again
            if (fromlen != sizeof(struct sockaddr_in) ||
                memcmp(&admin_server_addr, &from, sizeof(from)) != 0) 
            {
                ERROR("ignoring dgram - from unexpected address\n");
                continue;
            }

            // if the dgram is not valid then receive again
            if (!verify_recvd_dgram(&dgram,ret,DGRAM_ID_CONNECT_ACTIVATE,0,0,0,0,0,s,sizeof(s))) {
                ERROR("ignoring dgram - verify_recvd_dgram: %s\n", s);
                continue;
            }

            // if this dgram_uid has been received before then receive again
            for (i = 0; i < MAX_DGRAM_UID_TBL; i++) {
                if (dgram_uid_equal(&dgram.u.connect_activate.dgram_uid, &dgram_uid_tbl[i])) {
                    break;
                }
            }
            if (i < MAX_DGRAM_UID_TBL) {
                ERROR("ignoring dgram - dgram_uid has been recvd before\n");
                continue;
            }

            // add the recvd dgram_uid to the dgram_uid_tbl
            memmove(dgram_uid_tbl+1, dgram_uid_tbl, sizeof(dgram_uid_t)*(MAX_DGRAM_UID_TBL-1));
            dgram_uid_tbl[0] = dgram.u.connect_activate.dgram_uid;

            // the recvd dgram is valid, so break out of this loop
            connect_activate_dgram_recvd = true;
            break;
        }

        // if we haven't received a CONNECT_ACTIVATE then send the ANNOUNCE again
        if (!connect_activate_dgram_recvd) {
            DEBUG("did not recive a valid dgram response, try send ANNOUNCE again\n");
            continue;
        }

        // return the user_name and service to caller
        *service = dgram.u.connect_activate.service;
        strncpy(user_name, dgram.u.connect_activate.user_name, MAX_USER_NAME);
        user_name[MAX_USER_NAME] = '\0';

        // we got the needed peer_addr and con_id
        peer_addr = dgram.u.connect_activate.client_addr;
        con_id    = dgram.u.connect_activate.con_id;
        break;
    }

    // call connect common;  on error sfd is closed by connect_common
    handle = connect_common(sfd, &peer_addr, con_id, &status);
    if (handle < 0) {
        ERROR("%"PRId64" connect_common failed, %s\n", con_id, status2str(status));
        sfd = -1;
        goto try_again;
    }

    // return handle
    return handle;
}

int connect_common(int sfd, struct sockaddr_in * peer_addr, uint64_t con_id, int * status)
{
    char                          s[100];
    dgram_t                       dgram;
    int                           i, ms, handle, con_tbl_idx;
    con_t                       * con = NULL;
    recvbuff_data_valid_entry_t * dve;
    bool                          connect_mutex_acquired = false;
    bool                          mutex_acquired = false;

    static int                    handle_upper_val;
    static bool                   first_call = true;

    // initial prints
    INFO("%"PRId64" connecting to peer %s\n",
         con_id, sock_addr_to_str(s,sizeof(s),(struct sockaddr*)peer_addr));
    DEBUG("sock options %s\n", sock_to_options_str(sfd, s, sizeof(s)));

    // verify con_id is not zero
    if (con_id == 0) {
        ERROR("%"PRId64" invalid con_id\n", con->con_id);
        *status = STATUS_ERR_INVALID_CONNECTION_ID;
        goto error_ret;
    }

    // acquire connect_mutex
    pthread_mutex_lock(&connect_mutex);
    connect_mutex_acquired = true;

    // on first call, init the other mutexes
    if (first_call) {
        for (i = 0; i < MAX_CON; i++) {
            pthread_mutex_init(&disconnect_mutex[i], NULL);
            pthread_mutex_init(&send_mutex[i], NULL);
            pthread_mutex_init(&mutex[i], NULL);
        }
        first_call = false;
    }

    // verify con_id is not already inuse 
    for (i = 0; i < MAX_CON; i++) {
        if (con_tbl[i] && con_tbl[i]->con_id == con_id) {
            ERROR("%"PRId64" con_id is already in use\n", con_id);
            *status = STATUS_ERR_DUPLICATE_CONNECTION_ID;
            goto error_ret;
        }
    }

    // find a free entry in the con tbl
    for (con_tbl_idx = 0; con_tbl_idx < MAX_CON; con_tbl_idx++) {
        if (con_tbl[con_tbl_idx] == NULL) {
            break;
        }
    }
    if (con_tbl_idx == MAX_CON) {
        ERROR("%"PRId64" con_tbl has no free slot for new connection\n", con_id);
        *status = STATUS_ERR_TOO_MANY_CONNECTIONS;
        goto error_ret;
    }

    // allocate memory for con
    con = con_tbl[con_tbl_idx] = calloc(1,sizeof(con_t));
    if (con == NULL) {
        ERROR("%"PRId64" con mem alloc failed\n", con_id);
        *status = STATUS_ERR_CONNECTION_MEM_ALLOC;
        goto error_ret;
    }

    // construct the handle;  the lower bits are con_tbl_idx and the upper buts
    // are a unique value used for verification that an old handle value is not being used
    handle = (con_tbl_idx + __sync_add_and_fetch(&handle_upper_val,MAX_CON)) & 0x7fffffff;
    if (handle == 0) {
        handle = (con_tbl_idx + __sync_add_and_fetch(&handle_upper_val,MAX_CON)) & 0x7fffffff;
    }

    // set con_handle
    con_handle[con_tbl_idx] = handle;

    // init the con_tbl entry , just non-zero values are explicitely set
    bzero(con, sizeof(con_t));
    con->con_id    = con_id;
    con->sfd       = sfd;
    con->peer_addr = *peer_addr;

    TAILQ_INIT(&con->data_dgram_free_list_head);
    TAILQ_INIT(&con->data_dgram_send_list_head);
    TAILQ_INIT(&con->data_dgram_waiting_for_ack_list_head);
    for (i = 0; i < MAX_DATA_DGRAM; i++) {
        data_dgram_entry_t * ddge = &con->data_dgram_array[i];
        ddge->dgram.u.p2p_data.id = i+1;
        if (i < MAX_DATA_DGRAM-1) {  // reserve last entry for disconnect
            TAILQ_INSERT_TAIL(&con->data_dgram_free_list_head, ddge, entries);
            ddge->on_list = ON_FREE_LIST;
        }
    };

    dve = malloc(sizeof(data_dgram_entry_t));
    dve->offset = 0;
    dve->length = 0;
    TAILQ_INIT(&con->recvbuff_data_valid_list_head);
    TAILQ_INSERT_TAIL(&con->recvbuff_data_valid_list_head, dve, entries);

    pthread_cond_init(&con->recv_data_avail_cond, NULL);
    con->recv_eod_offset = RECV_EOD_OFFSET_NOT_SET;

    CON_STATE_CHANGE(con, STATE_CONNECTING);

    // release connect mutex
    pthread_mutex_unlock(&connect_mutex);
    connect_mutex_acquired = false;

    // acquire mutex
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    mutex_acquired = true;

    // create recv_dgram_thread
    pthread_create(&con->recv_dgram_thread_id, NULL, recv_dgram_thread, (void*)(long)con_tbl_idx);

    // repeat sending DGRAM_ID_P2P_CON_REQ, until con_resp_recvd
    dgram.id = DGRAM_ID_P2P_CON_REQ;
    dgram.u.p2p_con_req.con_id = con_id;
    for (ms = 0; ms < 5000 && !con->con_resp_recvd; ms += 10) {
        if ((ms % 1000) == 0) {
            send_dgram(con, &dgram, offsetof(dgram_t,u.p2p_con_req.dgram_end));
        }
        sleep_ms(10,&mutex[con_tbl_idx]);
    }

    // check for timeout establishing the connection
    if (!con->con_resp_recvd) {
        ERROR("%"PRId64" connect response not received\n", con->con_id);
        *status = STATUS_ERR_NO_RESPONSE_FROM_PEER;
        goto error_ret;
    }
    
    // init the timers that are used by the poll_thread, and
    // create the poll_thread
    con->time_last_dgram_recvd_ms = MILLISEC_TIMER;
    con->time_last_stats_dgram_sent_ms  = MILLISEC_TIMER;
    pthread_create(&con->poll_thread_id, NULL, poll_thread, (void*)(long)con_tbl_idx);

    // create the monitor_thread
    pthread_create(&con->monitor_thread_id, NULL, monitor_thread, (void*)(long)con_tbl_idx);

    // set STATE_CONNECTED
    CON_STATE_CHANGE(con, STATE_CONNECTED);

    // release mutex
    pthread_mutex_unlock(&mutex[con_tbl_idx]);
    mutex_acquired = false;

    // return handle
    return handle;

error_ret:
    // error return
    if (connect_mutex_acquired) {
        pthread_mutex_unlock(&connect_mutex);
    }
    if (mutex_acquired) {
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
    }
    if (con != NULL) {
        free_con(con_tbl_idx);
        sfd = -1;
    }
    if (sfd > 0) {
        close(sfd);
        sfd = -1;
    }
    return -1;
}

// -----------------  P2P_DISCONNECT  ----------------------------------

int p2p1_disconnect(int handle)
{
    con_t               * con;
    uint64_t             time_start_ms;
    data_dgram_entry_t * ddge;
    int                  initial_state, con_tbl_idx;
    int                  ret = 0;

    // verify handle, acquire mutex, and set ptr to con
    con_tbl_idx = (handle & (MAX_CON-1));
    pthread_mutex_lock(&disconnect_mutex[con_tbl_idx]);
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    if (con_tbl[con_tbl_idx] == NULL || con_handle[con_tbl_idx] != handle) {
        if (con_tbl[con_tbl_idx] == NULL) {
            ERROR("invalid handle %d, con_tbl[%d] is NULL\n", handle, con_tbl_idx);
        } else {
            ERROR("invalid handle %d, con_handle[%d] = %d\n",
                  handle, con_tbl_idx, con_handle[con_tbl_idx]);
        }
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        pthread_mutex_unlock(&disconnect_mutex[con_tbl_idx]);
        return -1;
    }
    con = con_tbl[con_tbl_idx];

    // save the initial state
    initial_state = con->state;

    // set state to STATE_DISCONNECTING and 
    CON_STATE_CHANGE(con, STATE_DISCONNECTING);

    // clear con_handle to provide additional protection
    con_handle[con_tbl_idx] = 0;

    // wait for in progress calls to p2p1_send or p2p1_recv to complete (not normal)
    while (con->send_in_prog || con->recv_in_prog) {
        sleep_ms(10, &mutex[con_tbl_idx]);
    }

    // if initial_state was STATE_ERROR then do quick disconnect
    if (initial_state == STATE_ERROR || con->state == STATE_ERROR) {
        ret = -1;
        goto state_error_quick_disconnect;
    }

    // now must be in STATE_CONNECTED ...

    // if we haven't recvd EOD (which means we are disconnecting first) then  ...
    if (con->recv_eod_offset == RECV_EOD_OFFSET_NOT_SET) {
        // send a data dgram with zero length to signify end of data,
        // note that the connect_common routine reserved the last entry for this purpose
        ddge = &con->data_dgram_array[MAX_DATA_DGRAM-1];

        TAILQ_INSERT_TAIL(&con->data_dgram_send_list_head, ddge, entries);

        ddge->dgram.id = DGRAM_ID_P2P_DATA;
        ddge->dgram.u.p2p_data.con_id = con->con_id;
        ddge->dgram.u.p2p_data.recvbuff_offset = 0;
        bzero(ddge->dgram.u.p2p_data.ack, sizeof(ddge->dgram.u.p2p_data.ack));
        ddge->dgram.u.p2p_data.id = P2P_DATA_ID_GEN_NEXT(ddge->dgram.u.p2p_data.id);
        ddge->dgram.u.p2p_data.offset = con->send_offset;  // this is the EOD offset
        ddge->dgram.u.p2p_data.length = 0;                 // this signifies EOD
        ddge->send_count = 0;
        ddge->time_resend_ms = 0;
        ddge->on_list = ON_SEND_LIST;

        send_data_dgram_list(con, MILLISEC_TIMER,"DISCONNECT");

        // wait for the send list and the waiting for ack list to drain, with timeout
        time_start_ms = MILLISEC_TIMER;
        while ((!TAILQ_EMPTY(&con->data_dgram_send_list_head) ||
                !TAILQ_EMPTY(&con->data_dgram_waiting_for_ack_list_head)) &&
               ((MILLISEC_TIMER) - time_start_ms < TIME_DISCONNECT_TOUT_MS))
        {
            sleep_ms(10, &mutex[con_tbl_idx]);
        }

        // it is an error if the sends failed to drain
        if (!TAILQ_EMPTY(&con->data_dgram_send_list_head) ||
            !TAILQ_EMPTY(&con->data_dgram_waiting_for_ack_list_head))
        {
            ERROR("%"PRId64" timeout draining sends\n", con->con_id);
            ret = -1;
        }
    }

state_error_quick_disconnect:
    // release mutex
    pthread_mutex_unlock(&mutex[con_tbl_idx]);

    // free the con
    free_con(con_tbl_idx); 

    // release disconnect_mutex
    pthread_mutex_unlock(&disconnect_mutex[con_tbl_idx]);

    // return status
    return ret;
}

void free_con(int con_tbl_idx)
{
    con_t                       * con = con_tbl[con_tbl_idx];
    recvbuff_data_valid_entry_t * dve;
    int                           sfd;

    // note - must be called without mutex held, so threads can exit

    // shutdown and close the socket0
    sfd = con->sfd;
    con->sfd = -1;
    shutdown(sfd, SHUT_RDWR);
    close(sfd);
    
    // wait for poll_thread and recv_dgram_thread to exit
    if (con->recv_dgram_thread_id) {
        pthread_join(con->recv_dgram_thread_id, NULL);
        con->recv_dgram_thread_id = 0;
    }
    if (con->poll_thread_id) {
        pthread_join(con->poll_thread_id, NULL);
        con->poll_thread_id = 0;
    }
    if (con->monitor_thread_id) {
        pthread_join(con->monitor_thread_id, NULL);
        con->monitor_thread_id = 0;
    }

    // free the recvbuff_data_valid_list
    while (!TAILQ_EMPTY(&con->recvbuff_data_valid_list_head)) {
        dve = TAILQ_FIRST(&con->recvbuff_data_valid_list_head);
        TAILQ_REMOVE(&con->recvbuff_data_valid_list_head, dve, entries);
        free(dve);
    }

    // log state change to STATE_DISCONNECTED
    CON_STATE_CHANGE(con, STATE_DISCONNECTED);

    // free the con, and clear the con_tbl and con_handle entries
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    pthread_mutex_lock(&connect_mutex);
    con_handle[con_tbl_idx] = 0;
    con_tbl[con_tbl_idx] = NULL;
    free(con);
    pthread_mutex_unlock(&connect_mutex);
    pthread_mutex_unlock(&mutex[con_tbl_idx]);
}

// -----------------  P2P_SEND  ----------------------------------------

int p2p1_send(int handle, void * buff, int len)
{
    con_t              * con;
    data_dgram_entry_t * ddge;
    int                  data_len, con_tbl_idx;
    int                  length_sent = 0;

    // verify buff and len
    if (buff == NULL || len == 0) {
        return -1;
    }

    // verify handle, acquire mutex, and set ptr to con
    con_tbl_idx = (handle & (MAX_CON-1));
    pthread_mutex_lock(&send_mutex[con_tbl_idx]);
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    if (con_tbl[con_tbl_idx] == NULL || con_handle[con_tbl_idx] != handle) {
        if (con_tbl[con_tbl_idx] == NULL) {
            ERROR("invalid handle %d, con_tbl[%d] is NULL\n", handle, con_tbl_idx);
        } else {
            ERROR("invalid handle %d, con_handle[%d] = %d\n",
                  handle, con_tbl_idx, con_handle[con_tbl_idx]);
        }
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        pthread_mutex_unlock(&send_mutex[con_tbl_idx]);
        return -1;
    }
    con = con_tbl[con_tbl_idx];

    // if not in STATE_CONNECTED or recv_eod_offset has been set
    if ((con->state != STATE_CONNECTED) ||
        (con->recv_eod_offset != RECV_EOD_OFFSET_NOT_SET)) 
    {
        ERROR("%"PRId64" is not STATE_CONNECTED or recv_eod_offset is set\n", con->con_id);
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        pthread_mutex_unlock(&send_mutex[con_tbl_idx]);
        return -1;
    }

    // set send in progress flag
    con->send_in_prog = true;

    // loop over caller's buffer
    while (len > 0) {
        // if the last entry on the data_dgram_send_list has not been sent and
        // has some room for more data then copy caller's data into it
        if (!TAILQ_EMPTY(&con->data_dgram_send_list_head) &&
            (ddge = TAILQ_LAST(&con->data_dgram_send_list_head, th2)) &&
            ddge->send_count == 0 &&
            ddge->dgram.u.p2p_data.length < MAX_DATA)
        {
            data_len = (len > (MAX_DATA-ddge->dgram.u.p2p_data.length) 
                        ? (MAX_DATA-ddge->dgram.u.p2p_data.length) 
                        : len);

            memcpy(ddge->dgram.u.p2p_data.data+ddge->dgram.u.p2p_data.length, buff, data_len);
            ddge->dgram.u.p2p_data.length += data_len;

            con->send_offset += data_len;
            length_sent += data_len;
            buff += data_len;
            len -= data_len;

            continue;
        }

        // if no dgram_entry on the free list then wait
        while ((TAILQ_EMPTY(&con->data_dgram_free_list_head)) && 
               (con->state == STATE_CONNECTED) &&
               (con->recv_eod_offset == RECV_EOD_OFFSET_NOT_SET))
        {
            send_data_dgram_list(con,MILLISEC_TIMER, "P2P_SEND-WAIT_LOOP");
            sleep_ms(10,&mutex[con_tbl_idx]);
        }

        // if no longer in connected state then return error
        if (con->state != STATE_CONNECTED ||
            con->recv_eod_offset != RECV_EOD_OFFSET_NOT_SET)
        {
            con->send_in_prog = false;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            pthread_mutex_unlock(&send_mutex[con_tbl_idx]);
            return -1;
        }

        // remove ddge from data_dgram_free_list, and put it on the data_dgram_send_list
        ddge = TAILQ_FIRST(&con->data_dgram_free_list_head);
        TAILQ_REMOVE(&con->data_dgram_free_list_head, ddge, entries);
        TAILQ_INSERT_TAIL(&con->data_dgram_send_list_head, ddge, entries);

        // init the ddge; some fields are set to actual values when dgram is sent
        data_len = (len > MAX_DATA ? MAX_DATA : len);

        ddge->dgram.id = DGRAM_ID_P2P_DATA;
        ddge->dgram.u.p2p_data.con_id = con->con_id;
        ddge->dgram.u.p2p_data.recvbuff_offset = 0;
        bzero(ddge->dgram.u.p2p_data.ack, sizeof(ddge->dgram.u.p2p_data.ack));
        ddge->dgram.u.p2p_data.id = P2P_DATA_ID_GEN_NEXT(ddge->dgram.u.p2p_data.id);
        ddge->dgram.u.p2p_data.offset = con->send_offset;
        ddge->dgram.u.p2p_data.length = data_len;
        memcpy(ddge->dgram.u.p2p_data.data, buff, data_len);

        ddge->send_count = 0;
        ddge->time_resend_ms = 0;
        ddge->on_list = ON_SEND_LIST;

        // update send_offset
        con->send_offset += data_len;

        // update length_sent
        length_sent += data_len;

        // update buff & len for next time aroung
        buff += data_len;
        len -= data_len;
    }

    // send some/all of the dgrams,
    send_data_dgram_list(con,MILLISEC_TIMER,"P2P_SEND");

    // update stats
    con->stats.sent_bytes += length_sent;

    // clear send in progress flag
    con->send_in_prog = false;

    // release mutex
    pthread_mutex_unlock(&mutex[con_tbl_idx]);
    pthread_mutex_unlock(&send_mutex[con_tbl_idx]);

    // yield to give other threads, that are waiting for the mutex, a chance to run
    sched_yield();

    // return length_sent
    return length_sent;
}

// -----------------  P2P_RECV  ----------------------------------------

// returns
// -  bytes received, or
// -  0  for eof
// - -1  for error
// - -2  for would-block

int p2p1_recv(int handle, void * buff, int len, int mode)
{
    con_t  * con;
    uint64_t offset;
    uint64_t len_to_end;
    int      xfer_len, con_tbl_idx;
    int      length_recvd = 0;
    int      length_requested = len;

    // verify buff and len
    if (buff == NULL || len == 0) {
        return RECV_ERROR;
    }

    // verify handle, acquire mutex, and set ptr to con
    con_tbl_idx = (handle & (MAX_CON-1));
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    if (con_tbl[con_tbl_idx] == NULL || con_handle[con_tbl_idx] != handle) {
        if (con_tbl[con_tbl_idx] == NULL) {
            ERROR("invalid handle %d, con_tbl[%d] is NULL\n", handle, con_tbl_idx);
        } else {
            ERROR("invalid handle %d, con_handle[%d] = %d\n",
                  handle, con_tbl_idx, con_handle[con_tbl_idx]);
        }
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        return RECV_ERROR;
    }
    con = con_tbl[con_tbl_idx];

    // if not in STATE_CONNECTED or STATE_ERROR then return error
    if (con->state != STATE_CONNECTED) {
        ERROR("%"PRId64" is not STATE_CONNECTED\n", con->con_id);
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        return RECV_ERROR;
    }

    // don't allow concurrent calls to recv
    if (con->recv_in_prog) {
        ERROR("%"PRId64" a receive is in progress\n", con->con_id);
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        return RECV_ERROR;
    }

    // check if enough data to proceed with no-wait modes, 
    // if not then return RECV_WOULDBLOCK
    if (mode == RECV_NOWAIT_ANY || mode == RECV_NOWAIT_ALL) {
        bool okay = false;
        if (mode == RECV_NOWAIT_ANY && (LOCAL_RECVBUFF_FIRST_LENGTH(con) >= 1)) {
            okay = true;
        } else if (mode == RECV_NOWAIT_ALL && (LOCAL_RECVBUFF_FIRST_LENGTH(con) >= len)) {
            okay = true;
        } else if ((con->recv_eod_offset != RECV_EOD_OFFSET_NOT_SET) &&
                   (LOCAL_RECVBUFF_FIRST_OFFSET(con) + LOCAL_RECVBUFF_FIRST_LENGTH(con) == con->recv_eod_offset))
        {
            okay = true;
        }
        if (!okay) {
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            return RECV_WOULDBLOCK;
        }
    } 

    // set recv in progress flag
    con->recv_in_prog = true;

    // loop until done filling the caller's buffer
    while (true) {
        // wait for recv data to be available, or at eod
        while ((LOCAL_RECVBUFF_FIRST_LENGTH(con) == 0) && 
               (LOCAL_RECVBUFF_FIRST_OFFSET(con) != con->recv_eod_offset) &&
               (con->state == STATE_CONNECTED)) 
        {
            assert(mode == RECV_WAIT_ANY || mode == RECV_WAIT_ALL);
            con->recv_cond_wait_len = (mode == RECV_WAIT_ALL ? len : 1);
            pthread_cond_wait(&con->recv_data_avail_cond, &mutex[con_tbl_idx]);
            con->recv_cond_wait_len = 0;
        }

        // if not in STATE_CONNECTED then return error
        if (con->state != STATE_CONNECTED) {
            con->recv_in_prog = false;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            return RECV_ERROR;
        }

        // if at EOD then break
        if (LOCAL_RECVBUFF_FIRST_OFFSET(con) == con->recv_eod_offset) {
            break;
        }

        // copy data to caller buffer
        offset = (LOCAL_RECVBUFF_FIRST_OFFSET(con) % MAX_RECVBUFF);
        len_to_end = MAX_RECVBUFF - offset;
        xfer_len = (LOCAL_RECVBUFF_FIRST_LENGTH(con) > len ? len : LOCAL_RECVBUFF_FIRST_LENGTH(con));
        if (xfer_len <= len_to_end) {
            memcpy(buff, con->recvbuff + offset, xfer_len);
        } else {
            memcpy(buff, con->recvbuff + offset, len_to_end);
            memcpy(buff + len_to_end, con->recvbuff, xfer_len - len_to_end);
        }

        // update length_recvd
        length_recvd += xfer_len;

        // update caller's buff and len for next time aroung
        buff += xfer_len;
        len -= xfer_len;

        // update first entry of the recvbuff_data_valid_list
        LOCAL_RECVBUFF_FIRST_OFFSET(con) += xfer_len;
        LOCAL_RECVBUFF_FIRST_LENGTH(con) -= xfer_len;

        // if the offset of the recvbuff is much larger than the last recvbuff
        // offset sent to the other side then send an ack, this will try to update the
        // other side with the latest recvbuff offset
        if (LOCAL_RECVBUFF_FIRST_OFFSET(con) - con->last_sent_local_recvbuff_offset > MAX_RECVBUFF / 4) {
            send_ack(con, MILLISEC_TIMER, "p2p_recv");
        }

        // check if done, when either
        // - caller's buffer has been filled completely, or
        // - caller has requested return when any data is available
        if (len == 0 || (mode == RECV_WAIT_ANY || mode == RECV_NOWAIT_ANY)) {
            break;
        }
    }

    // update stats
    con->stats.recvd_bytes += length_recvd;

    // if caller has requested all data then we must, at this point, 
    // return either the requested length or EOF
    if ((mode == RECV_WAIT_ALL || mode == RECV_NOWAIT_ALL) && 
        length_recvd != length_requested && 
        length_recvd != RECV_EOF) 
    {
        ERROR("lenght_recvd=%d length_requested=%d mode=%d\n", length_recvd, length_requested, mode);
        con->recv_in_prog = false;
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        return RECV_ERROR;
    }

    // clear recv_in_prog
    con->recv_in_prog = false;

    // release mutex
    pthread_mutex_unlock(&mutex[con_tbl_idx]);

    // return length_recvd
    return length_recvd;
}

// -----------------  RECV_DGRAM_THREAD  -------------------------------

void thread_cleanup_handler(void * cx)
{
    pthread_mutex_t ** mutex_locked = cx;

    if (*mutex_locked != NULL) {
        pthread_mutex_unlock(*mutex_locked);
        *mutex_locked = NULL;
    }
}

void * recv_dgram_thread(void * cx)
{
    int                con_tbl_idx = (long)cx;
    con_t            * con = con_tbl[con_tbl_idx];
    socklen_t          fromlen;
    struct sockaddr_in from;
    int                length;
    dgram_storage_t    dgram_storage;
    dgram_t          * dgram = &dgram_storage.dgram;
    bool               call_pthread_cond_signal = false;
    char               s[100];
    uint64_t           time_ms;
    pthread_mutex_t  * mutex_locked = NULL;

    pthread_cleanup_push(thread_cleanup_handler, &mutex_locked);

    while (true) {
        // receive a datagram
        fromlen = sizeof(from);
        length = recvfrom(con->sfd, dgram, sizeof(dgram_storage), 0,
                          (struct sockaddr *)&from, &fromlen);

        // if free_con has shutdown the sfd then exit this thread
        if (con->sfd == -1) {
            break;
        }

        // acquire mutex  (not a cancel point)
        pthread_mutex_lock(&mutex[con_tbl_idx]);
        mutex_locked = &mutex[con_tbl_idx];

        // check return from recvfrom
        if (length < 0) {
            ERROR("%"PRId64" recvfrom return error %s\n", con->con_id, strerror(errno));
            CON_STATE_CHANGE(con, STATE_ERROR);
            mutex_locked = NULL;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            break;
        }

        // debug print the received dgram
        DEBUG_PRINT_DGRAM(true, dgram, length);

        // verify the dgram came from peer
        if (fromlen != sizeof(struct sockaddr_in) ||
            memcmp(&from, &con->peer_addr, sizeof(from)) != 0) 
        {
            DEBUG("%"PRId64" ignoring dgram not from peer\n", con->con_id);
            mutex_locked = NULL;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            continue;
        }

        // verify the received dgram 
        if (!verify_recvd_dgram(dgram, length,
                                DGRAM_ID_P2P_CON_REQ, DGRAM_ID_P2P_CON_RESP,
                                DGRAM_ID_P2P_DATA, DGRAM_ID_P2P_ACK, DGRAM_ID_P2P_STATS,
                                con->con_id, s, sizeof(s)))
        {
            ERROR("%"PRId64" verify_recvd_dgram: %s\n", con->con_id, s);
            CON_STATE_CHANGE(con, STATE_ERROR);
            mutex_locked = NULL;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            break;
        }

        // read timer
        time_ms = MILLISEC_TIMER;

        // dgram is valid, so can now set the time_last_dgram_recvd_ms 
        con->time_last_dgram_recvd_ms = time_ms;

        // if con is in STATE_ERROR then exit this thread
        if (con->state == STATE_ERROR) {
            mutex_locked = NULL;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            break;
        }

        // switch on the dgram id
        switch (dgram->id) {
        case DGRAM_ID_P2P_CON_REQ: {
            dgram_t dgram_con_resp;
            if (con->state == STATE_CONNECTING || con->state == STATE_CONNECTED) {
                dgram_con_resp.id = DGRAM_ID_P2P_CON_RESP;
                dgram_con_resp.u.p2p_con_resp.con_id = con->con_id;
                send_dgram(con,&dgram_con_resp,offsetof(dgram_t,u.p2p_con_resp.dgram_end));
            }
            break; }

        case DGRAM_ID_P2P_CON_RESP:
            con->con_resp_recvd = true;
            break;

        case DGRAM_ID_P2P_ACK:
            // update stats
            con->stats.recvd_acks++;

            // since the con_resp dgram might have been lost, we set the con_resp_recvd
            // flag also when received the p2p1_ack and p2p1_data
            con->con_resp_recvd = true;

            // process the acks received
            process_acks(con, dgram->u.p2p_ack.ack, MAX_ACK, dgram->u.p2p_ack.recvbuff_offset);

            // since we've received some acks, we may now be able to send more data dgrams,
            // call send_data_dgram_list to try to do so
            send_data_dgram_list(con,time_ms,"RECVD P2P_ACK");
            break;

        case DGRAM_ID_P2P_DATA: {
            uint64_t recvbuff_start, recvbuff_end;
            uint64_t data_start, data_end;
            int      off, ltoe, data_length;
            bool     discard_duplicate;
            struct recvbuff_data_valid_entry_s *dve, *dve_next, *dve_new;

            // update stats
            con->stats.recvd_data_dgrams++;

            // since the con_resp dgram might have been lost, we set the con_resp_recvd
            // flag also when received the p2p1_ack and p2p1_data
            con->con_resp_recvd = true;

            // get start and end offsets for the local recvbuff and the dgram data buffer
            recvbuff_start = LOCAL_RECVBUFF_FIRST_OFFSET(con);
            recvbuff_end   = recvbuff_start + MAX_RECVBUFF;
            data_start     = dgram->u.p2p_data.offset;
            data_end       = data_start + dgram->u.p2p_data.length;
            data_length    = dgram->u.p2p_data.length;

            // if the data ends beyond the receive buffer end that is an error,
            // the other side should never have sent this packet due to flow control
            if (data_end > recvbuff_end) {
                ERROR("%"PRId64" data dgram data_end is greater than recvbuff_end\n", con->con_id);
                CON_STATE_CHANGE(con, STATE_ERROR);
                break;
            }

            // call process_acks, to handle ack info that is included in P2P_DATA;
            // call send_data_dgram_list because we may now be able to send more
            process_acks(con, dgram->u.p2p_data.ack, MAX_ACK, dgram->u.p2p_data.recvbuff_offset);
            send_data_dgram_list(con,time_ms,"RECVD_P2P_DATA");

            // queue processing of the ack for the received data;
            // if a number of acks need to be sent or we're at eod then 
            // call send_ack
            con->ack[con->ack_tail] = dgram->u.p2p_data.id;
            if (++con->ack_tail >= MAX_ACK) {
                con->ack_tail = 0;
            }
            con->ack_unsent_count++;
            if (con->ack_unsent_count == 1) {
                con->ack_oldest_unsent_time_ms = time_ms;
            }
            if ((con->ack_unsent_count >= MAX_ACK / 2) ||
                (con->recv_eod_offset != RECV_EOD_OFFSET_NOT_SET)) 
            {
                send_ack(con, time_ms, "recv_dgram_thread");
            }

            // if EOD indication received then 
            // - save the eod offset and p2p1_recv;
            // - flush send and waiting for ack lists, because the other side
            //   is no longer receiving
            // - wake p2p1_recv, because it may now be at EOD
            // - send ack
            if (dgram->u.p2p_data.length == 0) {
                con->recv_eod_offset = dgram->u.p2p_data.offset;
                TAILQ_INIT(&con->data_dgram_send_list_head);
                TAILQ_INIT(&con->data_dgram_waiting_for_ack_list_head);
                pthread_cond_signal(&con->recv_data_avail_cond);
                send_ack(con, time_ms, "recv_dgram_thread EOD");
                break;
            }

            // update receive buffer valid list, 
            // always keep the first entry even though it may be zero length;
            // loop over all recvbuff data valid list entries (dve)
            discard_duplicate = false;
            TAILQ_FOREACH(dve, &con->recvbuff_data_valid_list_head, entries) {
                // get the next entry too (dve_next), and 
                // check if there is not a next entry
                dve_next = TAILQ_NEXT(dve,entries);
                if (dve_next == NULL) {
                    // no next entry ...
                    // if the start of the data is before the end of the entry
                    //    discard duplicate  
                    // else if the start of the data is at the end of the entry
                    //    extend the length of the entry (this is the normal case)
                    // else (the data start is beyond the end of the entry)
                    //    malloc a new entry, init the new entry, and insert it list
                    // endif
                    if (data_start < dve->offset + dve->length) {
                        DEBUG("%"PRId64" duplicate - data_start is before the end of entry\n", con->con_id);
                        discard_duplicate = true;
                        break;
                    } else if (data_start == dve->offset + dve->length) {
                        dve->length += data_length;
                    } else {
                        dve_new = malloc(sizeof(*dve_new));
                        dve_new->offset = data_start;
                        dve_new->length = data_length;
                        TAILQ_INSERT_AFTER(&con->recvbuff_data_valid_list_head, dve, dve_new, entries);
                    }
                    break;
                } else {
                    // there is a next entry ...
                    // if the start of the data is before the end of the entry
                    //    discard duplicate
                    // else if the start of the data is at the end of the entry
                    //    extend the length of the entry;
                    //    if the end of the entry is beyond the begining of the next entry
                    //       error
                    //    else if the end of the entry is at the begining of the next entry
                    //       extent the length of the entry by the length of the next entry;
                    //       remove the next entry from the list and free it
                    //    endif
                    // else if start of data is less than the begining of the next entry
                    //    if the end of data is less than the begining of the next entry
                    //       allocate a new entry, init it, and insert on list
                    //    else if the end of data is at the begining of the next entry
                    //       update the next entry to include the data
                    //    else
                    //       error, because the end of data is extended beyond the start of next entry
                    //    endif
                    // else
                    //     continue the loop, because we haven yet found the appropriate place
                    //      to update the list
                    // endif
                    if (data_start < dve->offset + dve->length) {
                        DEBUG("%"PRId64" duplicate - data_start is before the end of entry\n", con->con_id);
                        discard_duplicate = true;
                        break;
                    } else if (data_start == dve->offset + dve->length) {
                        dve->length += data_length;
                        if (dve->offset + dve->length > dve_next->offset) {
                            ERROR("%"PRId64" extended entry ends beyond the start of next entry\n", con->con_id);
                            CON_STATE_CHANGE(con, STATE_ERROR);
                            break;
                        } else if (dve->offset + dve->length == dve_next->offset) {
                            dve->length += dve_next->length;
                            TAILQ_REMOVE(&con->recvbuff_data_valid_list_head, dve_next, entries);
                            free(dve_next);
                        }
                    } else if (data_start < dve_next->offset) {
                        if (data_end < dve_next->offset) {
                            dve_new = malloc(sizeof(*dve_new));
                            dve_new->offset = data_start;
                            dve_new->length = data_length;
                            TAILQ_INSERT_AFTER(&con->recvbuff_data_valid_list_head, dve, dve_new, entries);
                        } else if (data_end == dve_next->offset) {
                            dve_next->offset = data_start;
                            dve_next->length += data_length;
                        } else {
                            ERROR("%"PRId64" end of data is beyond the start of next entry\n", con->con_id);
                            CON_STATE_CHANGE(con, STATE_ERROR);
                            break;
                        }
                    } else {
                        continue;
                    }
                    break;
                }
            }
            if (con->state == STATE_ERROR) {
                break;
            }
#if 0
            DEBUG("%"PRId64" recvbuff_data_valid_list ...\n", con->con_id);
            TAILQ_FOREACH(dve, &con->recvbuff_data_valid_list_head, entries) {
                DEBUG("%"PRId64"    %10"PRId64"  %10"PRId64"\n", 
                      con->con_id, dve->offset, dve->length);
            }
#endif

            // copy the data to recieve buffer
            if (!discard_duplicate) {
                off  = data_start % MAX_RECVBUFF;
                ltoe = MAX_RECVBUFF - off;
                if (dgram->u.p2p_data.length <= ltoe) {
                    memcpy(con->recvbuff + off,
                        dgram->u.p2p_data.data, 
                        dgram->u.p2p_data.length);
                } else {
                    memcpy(con->recvbuff + off,
                        dgram->u.p2p_data.data, 
                        ltoe);
                    memcpy(con->recvbuff + 0,
                        dgram->u.p2p_data.data + ltoe,
                        dgram->u.p2p_data.length - ltoe);
                }
            }

            // update stats
            if (discard_duplicate) {
                con->stats.recvd_duplicates++;
            }

            // if p2p1_recv is waiting for cond signal and 
            //  (a) there is enough data in the recvbuff to satisfy the wait, or 
            //  (b) eod has been set, or
            //  (c) there is enough LOCAL_RECVBUFF_FIRST_LENGTH so that we shoud set 
            //      cond_signal so that p2p1_recv can copy the recvbuff data to it's 
            //      own buffer and send an ack to inform the peer that space is available
            // then set flag to cause pthread_cond_signal to be called
            if ((con->recv_cond_wait_len != 0) &&
                ((LOCAL_RECVBUFF_FIRST_LENGTH(con) >= con->recv_cond_wait_len)  ||
                 (con->recv_eod_offset != RECV_EOD_OFFSET_NOT_SET) ||
                 (LOCAL_RECVBUFF_FIRST_LENGTH(con) > MAX_RECVBUFF / 4)))
            {
                call_pthread_cond_signal = true;
            }
            break; }

        case DGRAM_ID_P2P_STATS: 
            // save stats from the peer
            con->stats.peer_resent_data_dgrams = dgram->u.p2p_stats.resent_data_dgrams;
            con->stats.peer_recvd_duplicates   = dgram->u.p2p_stats.recvd_duplicates;
            break;

        default:
            break;
        }

        // if the switch statement above set STATE_ERROR then exit this thread
        if (con->state == STATE_ERROR) {
            mutex_locked = NULL;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            break;
        }

        // release mutex
        mutex_locked = NULL;
        pthread_mutex_unlock(&mutex[con_tbl_idx]);

        // it may be more efficient to call pthread_cond_signal outside of mutex, so
        // that is what is being done here  
        if (call_pthread_cond_signal) {
            pthread_cond_signal(&con->recv_data_avail_cond);
            call_pthread_cond_signal = false;
        }
    }

    pthread_cleanup_pop(1);

    return NULL;
}

// -----------------  CON_MGMT_THREAD ----------------------------------

void * poll_thread(void * cx)
{
    int                  con_tbl_idx = (long)cx;
    con_t              * con = con_tbl[con_tbl_idx];
    data_dgram_entry_t * ddge;
    data_dgram_entry_t * ddge_next;
    uint64_t             time_ms;
    pthread_mutex_t    * mutex_locked = NULL;

    pthread_cleanup_push(thread_cleanup_handler, &mutex_locked);

    while (true) {
        // sleep for TIME_DELTA_MS
        sleep_ms(TIME_DELTA_MS, NULL);

        // if free_con has shutdown the sfd then exit this thread
        if (con->sfd == -1) {
            break;
        }

        // acquire mutex
        pthread_mutex_lock(&mutex[con_tbl_idx]);
        mutex_locked = &mutex[con_tbl_idx];

        // read the timer 
        time_ms = MILLISEC_TIMER;

        // if con is in STATE_ERROR then exit this thread
        if (con->state == STATE_ERROR) {
            mutex_locked = NULL;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            break;
        }

        // if state is connected and we haven't received eod offset and
        // no dgram has been received for TIME_DGRAM_RECV_TOUT_MS then 
        // set STATE_ERROR
        if (con->state == STATE_CONNECTED &&
            con->recv_eod_offset == RECV_EOD_OFFSET_NOT_SET && 
            time_ms - con->time_last_dgram_recvd_ms > TIME_DGRAM_RECV_TOUT_MS) 
        {
            ERROR("%"PRId64" nothing received in %d ms\n", 
                  con->con_id, TIME_DGRAM_RECV_TOUT_MS);
            CON_STATE_CHANGE(con, STATE_ERROR);
            mutex_locked = NULL;
            pthread_mutex_unlock(&mutex[con_tbl_idx]);
            break;
        }

        // loop over list of dgrams waiting for ack; if an entry on the waiting for
        // ack list needs to be resent then take it off the waiting for ack list 
        // and put it on the head of send list 
        for (ddge = TAILQ_FIRST(&con->data_dgram_waiting_for_ack_list_head); ddge; ddge = ddge_next) {
            ddge_next = TAILQ_NEXT(ddge,entries);
            if (time_ms > ddge->time_resend_ms) {
                // move entry from waiting_for_ack_list to send_list
                TAILQ_REMOVE(&con->data_dgram_waiting_for_ack_list_head, ddge, entries);
                con->num_data_dgrams_on_waiting_for_ack_list--;
                TAILQ_INSERT_HEAD(&con->data_dgram_send_list_head, ddge, entries);
                ddge->on_list = ON_SEND_LIST;

                // update stats
                con->stats.resent_data_dgrams++;
                DEBUG("%"PRId64" queue ddge for resend, id =%d send_count=%d\n",
                      con->con_id, ddge->dgram.u.p2p_data.id, ddge->send_count);
            }
        }

        // send data dgrams that are on the send list
        send_data_dgram_list(con,time_ms,"CON_MGMT_THREAD");

        // if there has been an unsent ack for TIME_ACK_SEND_TOUT_MS ms then send an ack
        if ((con->ack_unsent_count > 0) &&
            (time_ms - con->ack_oldest_unsent_time_ms > TIME_ACK_SEND_TOUT_MS))
         {
            send_ack(con, time_ms, "poll_thread-1");
        }

        // if it has been TIME_SEND_STATS_MS since last time stats
        // were sent then send the stats
        if (time_ms - con->time_last_stats_dgram_sent_ms > TIME_SEND_STATS_MS) {
            send_stats(con, time_ms);
        }

        // release mutex
        mutex_locked = NULL;
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
    }

    pthread_cleanup_pop(1);

    return NULL;
}

// -----------------  GET CONNECTION STATS  ---------------------------

int p2p1_get_stats(int handle, p2p_stats_t * stats)
{
    con_t * con;
    int     con_tbl_idx;

    // verify handle, acquire mutex, and set ptr to con
    con_tbl_idx = (handle & (MAX_CON-1));
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    if (con_tbl[con_tbl_idx] == NULL || con_handle[con_tbl_idx] != handle) {
        if (con_tbl[con_tbl_idx] == NULL) {
            ERROR("invalid handle %d, con_tbl[%d] is NULL\n", handle, con_tbl_idx);
        } else {
            ERROR("invalid handle %d, con_handle[%d] = %d\n",
                  handle, con_tbl_idx, con_handle[con_tbl_idx]);
        }
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        return -1;
    }
    con = con_tbl[con_tbl_idx];

    // copy con stats to caller's buffer
    *stats = con->stats;

    // release mutex and return success
    pthread_mutex_unlock(&mutex[con_tbl_idx]);
    return 0;
}

// -----------------  MONITOR CONNECTION STATS  ------------------------

int p2p1_monitor_ctl(int handle, int secs)
{
    con_t * con;
    int     con_tbl_idx;

    // verify handle, acquire mutex, and set ptr to con
    con_tbl_idx = (handle & (MAX_CON-1));
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    if (con_tbl[con_tbl_idx] == NULL || con_handle[con_tbl_idx] != handle) {
        if (con_tbl[con_tbl_idx] == NULL) {
            ERROR("invalid handle %d, con_tbl[%d] is NULL\n", handle, con_tbl_idx);
        } else {
            ERROR("invalid handle %d, con_handle[%d] = %d\n",
                  handle, con_tbl_idx, con_handle[con_tbl_idx]);
        }
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        return -1;
    }
    con = con_tbl[con_tbl_idx];

    // update monitor fields in con
    con->monitor_secs = secs;

    // release mutex and return success
    pthread_mutex_unlock(&mutex[con_tbl_idx]);
    return 0;
}

void * monitor_thread(void * cx)
{
    #define DELTA(fld) (stats.fld - stats_last.fld)

    int         con_tbl_idx = (long)cx;
    con_t     * con = con_tbl[con_tbl_idx];
    p2p_stats_t stats, stats_last;
    uint64_t    stats_time_ms, stats_last_time_ms;
    int         interval_ms, i;
    int         print_header_count=0;
    bool        first_loop = true;

    // init to avoid compiler warning
    stats_last = con->stats;
    stats_last_time_ms = MILLISEC_TIMER;

    // loop until thread is must terminate
    while (true) {
        // if sleep time is 0 then 
        //   wait until non zero
        //   reset print_header_count, so the header will print immedeatley
        //   make copy of stats, for last_stats
        // endif
        if (con->monitor_secs == 0 || first_loop) {
            while (con->monitor_secs == 0) {
                sleep_ms(100,NULL);
                if (con->sfd == -1) {
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
            INFO("                                                Peer  Peer  \n");
            INFO("            Sent  Recvd Sent  Recvd Resnd RcvDp Resnd RcvDp \n");
            INFO("Send  Recv  Data  Data  Ack   Ack   Data  Data  Data  Data  \n");
            INFO("Mb/S  Mb/S  Dgram Dgram Dgram Dgram Dgram Dgram Dgram Dgram \n");
        }

        // sleep for caller's desired interval
        for (i = 0; i < con->monitor_secs; i++) {
            sleep_ms(1000, NULL);
            if (con->sfd == -1) {
                goto done;
            }
        }

        // get stats now
        stats = con->stats;
        stats_time_ms = MILLISEC_TIMER;

        // print changes since stats_last
        interval_ms = stats_time_ms - stats_last_time_ms;
        INFO("%5.1f %5.1f %5d %5d %5d %5d %5d %5d %5d %5d\n",
             (double)DELTA(sent_bytes) * 8 / (interval_ms * 1000),
             (double)DELTA(recvd_bytes) * 8 / (interval_ms * 1000),
             (int)DELTA(sent_data_dgrams),
             (int)DELTA(recvd_data_dgrams),
             (int)DELTA(sent_acks),
             (int)DELTA(recvd_acks),
             (int)DELTA(resent_data_dgrams),
             (int)DELTA(recvd_duplicates),
             (int)DELTA(peer_resent_data_dgrams),
             (int)DELTA(peer_recvd_duplicates));

        // save stats in stats_last
        stats_last = stats;
        stats_last_time_ms = stats_time_ms;
    }

done:
    // terminate
    return NULL;
}

// -----------------  SUPPORT ROUTINES  --------------------------------

void send_data_dgram_list(con_t * con, uint64_t time_ms, char * debug_str)
{
    data_dgram_entry_t * ddge;
    int                  i, j, length;
    int                  num_sent = 0;

    while (true) {
        // get ptr to first dgram on the send list
        ddge = TAILQ_FIRST(&con->data_dgram_send_list_head);
        if (ddge == NULL) {
            break;
        }

        // if there are too many dgrams on the waiting for ack list then return
        if (con->num_data_dgrams_on_waiting_for_ack_list >= MAX_DATA_DGRAM_ON_WFA_LIST) {
            break;
        }

        // if the end data in the dgram is beyond the end of the remote recvbuff then return
        if (ddge->dgram.u.p2p_data.offset + ddge->dgram.u.p2p_data.length >
            con->remote_recvbuff_offset + MAX_RECVBUFF)
        {
            break;
        }

        // at this point we will be sending the dgram at the head of the send list ...

        // add local recieve buff offset and acks to the dgram
        ddge->dgram.u.p2p_data.recvbuff_offset = LOCAL_RECVBUFF_FIRST_OFFSET(con);
        for (i = 0, j = con->ack_tail; i < MAX_ACK; i++) {
            if (--j < 0) {
                j = MAX_ACK - 1;
            }
            ddge->dgram.u.p2p_data.ack[i] = con->ack[j];
        }

        // send the dgram,
        length = offsetof(dgram_t, u.p2p_data.data) + ddge->dgram.u.p2p_data.length;
        if (send_dgram(con, &ddge->dgram, length) == -1) {
            break;
        }
        num_sent++;

        // the dgram has been queueed to sockets ...

        // update ack state 
        con->ack_unsent_count = 0;
        con->ack_oldest_unsent_time_ms = 0;
        con->last_sent_local_recvbuff_offset = LOCAL_RECVBUFF_FIRST_OFFSET(con);

        // remove the ddge freom the dgram_send_list, and 
        // put it on the tail of the waiting for ack list
        TAILQ_REMOVE(&con->data_dgram_send_list_head, ddge, entries);
        TAILQ_INSERT_TAIL(&con->data_dgram_waiting_for_ack_list_head, ddge, entries);
        ddge->on_list = ON_WAITING_FOR_ACK_LIST;
        con->num_data_dgrams_on_waiting_for_ack_list++;

        // determine the time at which if this dgram is still on the waiting for ack list
        // it will be resend 
        ddge->send_count++;
        ddge->time_resend_ms = time_ms + TIME_RESEND_MS;
    }

    // update stats
    con->stats.sent_data_dgrams += num_sent;

    // debug print the number sent
    if (num_sent && debug_str) {
        DEBUG("%"PRId64" sent %d data dgrams, CALLER=%s\n", con->con_id, num_sent, debug_str);
    }
}

void process_acks(con_t * con, int * ack, int max, uint64_t new_remote_recvbuff_offset)
{
    int i, idx, id;
    data_dgram_entry_t * ddge;

    // if the other side is reporting a larger recvbuff_offset then save the new 
    // value;  the purpose of the check for larger is to handle the possibility of out
    // of order recv
    if (new_remote_recvbuff_offset > con->remote_recvbuff_offset) {
        con->remote_recvbuff_offset = new_remote_recvbuff_offset;
    }

    // the ack array is ordered with the acks for the most recent received dgrams
    // at the begining;  the array will contain acks that have been previously
    // processed, when a previously processed ack is detected then stop scanning the array;
    // note - rarely it could be a problem to not process the entire array if the acks 
    //        are recvd out of order; 
    for (i = 0; i < max; i++) {
        id = ack[i];
        if (id == 0) {
            return;
        }
        
        idx = P2P_DATA_ID_TO_IDX(id);
        ddge = &con->data_dgram_array[idx];
        if (ddge->on_list != ON_WAITING_FOR_ACK_LIST || ddge->dgram.u.p2p_data.id != id) {
            return;
        }

        // remove dgram from waiting for ack list
        TAILQ_REMOVE(&con->data_dgram_waiting_for_ack_list_head, ddge, entries);
        con->num_data_dgrams_on_waiting_for_ack_list--;

        // put dgram on free list
        TAILQ_INSERT_TAIL(&con->data_dgram_free_list_head, ddge, entries);
        ddge->on_list = ON_FREE_LIST;
    }
}

void send_ack(con_t * con, uint64_t time_ms, char * debug_str)
{
    dgram_t dgram;
    int i, j;

    // init the ack dgram
    dgram.id = DGRAM_ID_P2P_ACK;
    dgram.u.p2p_ack.con_id = con->con_id;
    dgram.u.p2p_ack.recvbuff_offset = LOCAL_RECVBUFF_FIRST_OFFSET(con);
    for (i = 0, j = con->ack_tail; i < MAX_ACK; i++) {
        if (--j < 0) {
            j = MAX_ACK - 1;
        }
        dgram.u.p2p_ack.ack[i] = con->ack[j];
    }

    // send the ack dgram
    if (send_dgram(con, &dgram, offsetof(dgram_t, u.p2p_ack.dgram_end)) == -1) {
        return;
    }

    // update stats
    con->stats.sent_acks++;

    // debug print
    DEBUG("%"PRId64" sent ack: FO=%"PRId64"  ID=%d %d %d %d %d %d %d %d %d %d  CALLER=%s\n",
          con->con_id,
          dgram.u.p2p_ack.recvbuff_offset,
          dgram.u.p2p_ack.ack[0], dgram.u.p2p_ack.ack[1],
          dgram.u.p2p_ack.ack[2], dgram.u.p2p_ack.ack[3],
          dgram.u.p2p_ack.ack[4], dgram.u.p2p_ack.ack[5],
          dgram.u.p2p_ack.ack[6], dgram.u.p2p_ack.ack[7],
          dgram.u.p2p_ack.ack[8], dgram.u.p2p_ack.ack[9],
          debug_str);

    // update ack state
    con->ack_unsent_count = 0;
    con->ack_oldest_unsent_time_ms = 0;
    con->last_sent_local_recvbuff_offset = LOCAL_RECVBUFF_FIRST_OFFSET(con);
}

void send_stats(con_t * con, uint64_t time_ms)
{
    dgram_t dgram;

    // init the stats dgram
    dgram.id = DGRAM_ID_P2P_STATS;
    dgram.u.p2p_stats.con_id             = con->con_id;
    dgram.u.p2p_stats.resent_data_dgrams = con->stats.resent_data_dgrams;
    dgram.u.p2p_stats.recvd_duplicates   = con->stats.recvd_duplicates;

    // send the stats dgram
    if (send_dgram(con, &dgram, offsetof(dgram_t, u.p2p_stats.dgram_end)) == -1) {
        return;
    }

    // save the time the stats dgram was sent
    con->time_last_stats_dgram_sent_ms = time_ms;
}

int send_dgram(con_t * con, void * dgram, int length)
{
    int ret;

    // debug print the dgram we are about to send
    DEBUG_PRINT_DGRAM(false, dgram, length);

    // send the dgram, non-blocking
    ret = sendto(con->sfd, dgram, length, MSG_DONTWAIT,
                 (struct sockaddr *)&con->peer_addr, sizeof(struct sockaddr_in));
    if (ret == -1 && errno == EWOULDBLOCK) {
        DEBUG("%"PRId64" sendto would block\n", con->con_id);
        return -1;
    }

    // verify ret is the send length requested
    if (ret != length) {
        ERROR("%"PRId64" sendto of P2P_DATA, len act=%d exp=%d", con->con_id, ret, length);
        CON_STATE_CHANGE(con, STATE_ERROR);
        return -1;
    }

    // return success
    return 0;
}

bool verify_recvd_dgram(dgram_t * dgram, int length, int dgid1, int dgid2, int dgid3, int dgid4, int dgid5,
        uint64_t con_id, char * s, int slen)
{
    // check for minimum length that is needed for dgram id and con_id
    if (length < 16) {
        snprintf(s, slen, "dgram length %d is too short", length);
        return false;
    }

    // verify dgram.id is one of the dgid1,2,3,4
    if ((dgram->id == 0) ||
        (dgram->id != dgid1 && dgram->id != dgid2 && dgram->id != dgid3 && 
         dgram->id != dgid4 && dgram->id != dgid5))
    {
        snprintf(s, slen, "dgram id=%d, exp=%d,%d,%d,%d,%d", dgram->id, dgid1, dgid2, dgid3, dgid4, dgid5);
        return false;
    }

    // if caller provided con_id then check that
    if (con_id != 0 && con_id != dgram->u.p2p_con_req.con_id) {
        snprintf(s, slen, "dgram con_id=%"PRId64", expected=%"PRId64, dgram->u.p2p_con_req.con_id, con_id);
        return false;
    }

    // verify length
    if (length != dgram_expected_length(dgram)) {
        snprintf(s, slen, "dgram length=%d expected=%d", length, dgram_expected_length(dgram));
        return false;
    }

    // return all checks okay
    s[0] = '\0';
    return true;
}

int dgram_expected_length(dgram_t * dgram)
{
    switch (dgram->id) {
    case DGRAM_ID_WC_ANNOUNCE:
        return offsetof(dgram_t, u.wc_announce.dgram_end);
    case DGRAM_ID_CONNECT_REQ:
        return offsetof(dgram_t, u.connect_req.dgram_end);
    case DGRAM_ID_CONNECT_ACTIVATE:
        return offsetof(dgram_t, u.connect_activate.dgram_end);
    case DGRAM_ID_CONNECT_REJECT:
        return offsetof(dgram_t, u.connect_reject.dgram_end);
    case DGRAM_ID_P2P_CON_REQ:
        return offsetof(dgram_t, u.p2p_con_req.dgram_end);
    case DGRAM_ID_P2P_CON_RESP:
        return offsetof(dgram_t, u.p2p_con_resp.dgram_end);
    case DGRAM_ID_P2P_ACK:
        return offsetof(dgram_t, u.p2p_ack.dgram_end);
    case DGRAM_ID_P2P_DATA:
        return offsetof(dgram_t, u.p2p_data.data) + dgram->u.p2p_data.length;
    case DGRAM_ID_P2P_STATS:
        return offsetof(dgram_t, u.p2p_stats.dgram_end);
    default:
        return -1;
    }
}

void sleep_ms(int ms, pthread_mutex_t * mutex)
{
    struct timespec ts;

    if (mutex) {
        pthread_mutex_unlock(mutex);
    }
    
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);

    if (mutex) {
        pthread_mutex_lock(mutex);
    }
}

int get_local_addr(struct sockaddr_in * local_addr)
{
    struct ifaddrs *ifaddr, *ifa;
    int ret = 0;
#ifdef DEBUG_PRINTS
    char s[100];
#endif

    bzero(local_addr,sizeof(struct sockaddr_in));

    ret = getifaddrs(&ifaddr);
    if (ret < 0) {
        return ret;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if ((ifa->ifa_addr != NULL) &&
            (ifa->ifa_addr->sa_family == AF_INET) &&
            ((ifa->ifa_flags & IFF_LOOPBACK) == 0) &&
            ((ifa->ifa_flags & IFF_POINTOPOINT) == 0))
        {
            *local_addr = *(struct sockaddr_in*)ifa->ifa_addr;
            DEBUG("local interface: %s - %s\n", 
                   ifa->ifa_name,
                   sock_addr_to_str(s, sizeof(s), (struct sockaddr*)local_addr));
            freeifaddrs(ifaddr);
            return 0;
        }
    }

    freeifaddrs(ifaddr);
    errno = ENODEV;
    return -1;
}

// -----------------  DEBUG ROUTINES  ----------------------------------

void p2p1_debug_con(int handle)
{
    // if handle equal -1 then print summary of all connections
    if (handle == -1) {
        char peer_addr_str[100];
        int i;

        PRINTF("    HANDLE   CON_ID        STATE            PEER_ADDR\n");

        pthread_mutex_lock(&connect_mutex);
        for (i = 0; i < MAX_CON; i++) {
            con_t * con = con_tbl[i];
            if (con == NULL) {
                continue;
            }
            PRINTF("%8d %8"PRId64" %20s %15s\n",
                   con_handle[i],
                   con->con_id, 
                   CON_STATE_STR(con->state),
                   sock_addr_to_str(peer_addr_str, sizeof(peer_addr_str), (struct sockaddr *)&con->peer_addr));
        }
        pthread_mutex_unlock(&connect_mutex);
        return;
    }

    // print details for con handle ..

    int                                  con_tbl_idx;
    con_t                              * con;
    struct recvbuff_data_valid_entry_s * dve;
    uint64_t                             time_ms;
    char                                 peer_addr_str[100];

    // verify handle, acquire mutex, and set ptr to con
    con_tbl_idx = (handle & (MAX_CON-1));
    pthread_mutex_lock(&mutex[con_tbl_idx]);
    if (con_tbl[con_tbl_idx] == NULL || con_handle[con_tbl_idx] != handle) {
        ERROR("invalid handle %d\n", handle);
        pthread_mutex_unlock(&mutex[con_tbl_idx]);
        return;
    }
    con = con_tbl[con_tbl_idx];

    // print con
    time_ms = MILLISEC_TIMER;
    PRINTF("handle=%d con_id=%"PRId64" state=%s peer_addr=%s sfd=%d\n",
           handle,
           con->con_id, 
           CON_STATE_STR(con->state),
           sock_addr_to_str(peer_addr_str, sizeof(peer_addr_str), (struct sockaddr *)&con->peer_addr),
           con->sfd);
    PRINTF("time (ms) since:  last_dgram_recvd=%"PRId64" last_stats_dgram_sent=%"PRId64"\n",
           time_ms - con->time_last_dgram_recvd_ms,
           time_ms - con->time_last_stats_dgram_sent_ms);
    PRINTF("con_resp_recvd=%d send_in_prog=%d recv_in_prog=%d\n",
           con->con_resp_recvd,
           con->send_in_prog,
           con->recv_in_prog);
    PRINTF("send_offset=%"PRId64" remote_recvbuff_offset=%"PRId64"\n",
           con->send_offset,
           con->remote_recvbuff_offset);
    PRINTF("list_length:  data_dgram_free_list=%d data_dgram_send_list=%d data_dgram_waiting_for_ack_list=%d\n",
           TAILQ_LENGTH(data_dgram_entry_s, &con->data_dgram_free_list_head, entries),
           TAILQ_LENGTH(data_dgram_entry_s, &con->data_dgram_send_list_head, entries),
           TAILQ_LENGTH(data_dgram_entry_s, &con->data_dgram_waiting_for_ack_list_head, entries));
    PRINTF("num_data_dgrams_on_waiting_for_ack_list=%d\n",
           con->num_data_dgrams_on_waiting_for_ack_list);
    PRINTF("recvbuff_data_valid_list: ...\n");
    TAILQ_FOREACH(dve, &con->recvbuff_data_valid_list_head, entries) {
        PRINTF("    %10"PRId64"  %10"PRId64"\n", 
               dve->offset, dve->length);
    }
    PRINTF("recv_eod_offset=%"PRId64" recv_cond_wait_len=%d\n",
           con->recv_eod_offset,
           con->recv_cond_wait_len);
    #if MAX_ACK != 10
    #error "max_ack"
    #endif
    PRINTF("ack=%d %d %d %d %d %d %d %d %d %d \n",
           con->ack[0], con->ack[1], con->ack[2], con->ack[3], con->ack[4],
           con->ack[5], con->ack[6], con->ack[7], con->ack[8], con->ack[9]);
    PRINTF("ack_tail=%d ack_unsent_count=%d time-since-oldest-unsent-ack=%d ms\n",
           con->ack_tail,
           con->ack_unsent_count,
           (con->ack_oldest_unsent_time_ms == 0 
            ? 99999999
            : (uint32_t)(time_ms - con->ack_oldest_unsent_time_ms)));
    PRINTF("last_sent_local_recvbuff_offset=%"PRId64"\n",
            con->last_sent_local_recvbuff_offset);
    PRINTF("monitor_secs=%d\n",
            con->monitor_secs);
    PRINTF("stats: sent_bytes=%"PRId64" recvd_bytes=%"PRId64
                 " sent_data_dgrams=%"PRId64" recvd_data_dgrams=%"PRId64"\n",
            con->stats.sent_bytes, 
            con->stats.recvd_bytes, 
            con->stats.sent_data_dgrams, 
            con->stats.recvd_data_dgrams);
    PRINTF("stats: sent_acks=%"PRId64" recvd_acks=%"PRId64
                 " resent_data_dgrams=%"PRId64" recvd_duplicates=%"PRId64"\n",
            con->stats.sent_acks, 
            con->stats.recvd_acks, 
            con->stats.resent_data_dgrams, 
            con->stats.recvd_duplicates);
    PRINTF("stats: peer_resent_data_dgrams=%"PRId64" peer_recvd_duplicates=%"PRId64"\n",
            con->stats.peer_resent_data_dgrams, 
            con->stats.peer_recvd_duplicates);

    pthread_mutex_unlock(&mutex[con_tbl_idx]);
}

#ifdef DEBUG_DGRAM
void debug_print_dgram(bool recv, dgram_t * dgram, int length)
{
    char * dir = (recv ? "RECV" : "SEND");
    char   s1[100], s2[100], len_err_str[100];
    int    expected_length;

    // generate string if length of dgram is not the expected length
    expected_length = dgram_expected_length(dgram);
    if (length != expected_length) {
        sprintf(len_err_str, "%"PRId64"  **ERROR-LEN=%d,EXP=%d**", 
                con->con_id, length, expected_length);
    } else {
        len_err_str[0] = '\0';
    }

    // print the datagram
    switch (dgram->id) {
    case DGRAM_ID_WC_ANNOUNCE:
        DEBUG("%s WC_ANNOUNCE%s wc_macaddr=%s\n",
              dir, len_err_str,
              dgram->u.wc_announce.wc_macaddr);
        break;
    case DGRAM_ID_CONNECT_REQ:
        DEBUG("%s CONNECT_REQ%s %s %s %s %s\n",
              dir, len_err_str,
              dgram->u.connect_req.user_name,
              dgram->u.connect_req.password,
              SERVICE_STR(dgram->u.connect_req.service),
              dgram->u.connect_req.wc_name);
        break;
    case DGRAM_ID_CONNECT_ACTIVATE:
        DEBUG("%s CONNECT_ACTIVATE%s con_id=%"PRId64" %s %s client=%s wc=%s\n",
              dir, len_err_str,
              dgram->u.connect_activate.con_id,
              dgram->u.connect_activate.user_name,
              SERVICE_STR(dgram->u.connect_activate.service),
              sock_addr_to_str(s1,sizeof(s1),(struct sockaddr*)&dgram->u.connect_activate.client_addr),
              sock_addr_to_str(s2,sizeof(s2),(struct sockaddr*)&dgram->u.connect_activate.wc_addr));
        break;
    case DGRAM_ID_P2P_CON_REQ:
        DEBUG("%s P2P_CON_REQ%s con_id=%"PRId64"\n",
              dir, len_err_str,
              dgram->u.p2p_con_req.con_id);
        break;
    case DGRAM_ID_P2P_CON_RESP:
        DEBUG("%s P2P_CON_RESP%s con_id=%"PRId64"\n",
              dir, len_err_str,
              dgram->u.p2p_con_resp.con_id);
        break;
    case DGRAM_ID_P2P_ACK:
        DEBUG("%s P2P_ACK%s con_id=%"PRId64" recvbuff_offset=%"PRId64" ack=%d %d %d %d %d %d %d %d %d %d\n",
              dir, len_err_str,
              dgram->u.p2p_ack.con_id,
              dgram->u.p2p_ack.recvbuff_offset,
              dgram->u.p2p_ack.ack[0], dgram->u.p2p_ack.ack[1],
              dgram->u.p2p_ack.ack[2], dgram->u.p2p_ack.ack[3],
              dgram->u.p2p_ack.ack[4], dgram->u.p2p_ack.ack[5],
              dgram->u.p2p_ack.ack[6], dgram->u.p2p_ack.ack[7],
              dgram->u.p2p_ack.ack[8], dgram->u.p2p_ack.ack[9]);
        break;
    case DGRAM_ID_P2P_DATA:
        DEBUG("%s P2P_DATA%s con_id=%"PRId64" recvbuff_offset=%"PRId64" ack=%d %d %d %d %d %d %d %d %d %d\n"
              "        id=%d offset=%"PRId64" length=%"PRId64" data=%d %d %d %d\n",
              dir, len_err_str,
              dgram->u.p2p_data.con_id,
              dgram->u.p2p_data.recvbuff_offset,
              dgram->u.p2p_data.ack[0], dgram->u.p2p_data.ack[1], 
              dgram->u.p2p_data.ack[2], dgram->u.p2p_data.ack[3],
              dgram->u.p2p_data.ack[4], dgram->u.p2p_data.ack[5], 
              dgram->u.p2p_data.ack[6], dgram->u.p2p_data.ack[7],
              dgram->u.p2p_data.ack[8], dgram->u.p2p_data.ack[9],
              dgram->u.p2p_data.id,
              dgram->u.p2p_data.offset,
              dgram->u.p2p_data.length,
              dgram->u.p2p_data.data[0], dgram->u.p2p_data.data[1], 
              dgram->u.p2p_data.data[2], dgram->u.p2p_data.data[3]);
        break;
    case DGRAM_ID_P2P_STATS:
        DEBUG("%s P2P_STATS%s con_id=%"PRId64" resent_data_dgrams="%PRId64" recvd_duplicates="%PRId64"\n",
              dir, len_err_str,
              dgram->u.p2p_stats.con_id,
              dgram->u.p2p_stats.resent_data_dgrams,
              dgram->u.p2p_stats.recvd_duplicates);
        break;
    default:
        DEBUG("%s INVALID DGRAM ID %d dgram_length=%d\n",
              dir, dgram->id, length);
        break;
    }
}
#endif

