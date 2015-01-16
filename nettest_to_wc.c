/*
Copyright (c) 2015 Steven Haid

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "wc.h"

//
// defines 
//

//
// variables
//

p2p_routines_t * p2p;
int  handle;
int  send_bytes;
int  msg_response_id;
bool send_thread_exit_req;
bool send_thread_exitted;
bool recv_thread_exitted;

//
// prototypes
//

void nettest_to_webcam(char * user_name, char * password, char * wc_name);
void * send_thread(void * cx);
void * recv_thread(void * cx);
int webcam_request(int msg_type, int val);

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit rl;
    char          opt_char;
    char        * wc_name;
    char        * user_name;
    char        * password;
    int           ret;
    bool          debug_mode = false;
    bool          help_mode = false;

    // set resource limti to allow core dumps
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    ret = setrlimit(RLIMIT_CORE, &rl);
    if (ret < 0) {
        WARN("setrlimit for core dump, %s\n", strerror(errno));
    }

    // get user_name and password from environment
    user_name = getenv("WC_USERNAME");
    password  = getenv("WC_PASSWORD");

    // parse options
    p2p = &p2p1;
    while (true) {
        opt_char = getopt(argc, argv, "Pu:p:hdv");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'P':
            p2p = &p2p2;
            break;
        case 'u':
            user_name = optarg;
            break;
        case 'p':
            password = optarg;
            break;
        case 'h':
            help_mode = true;
            break;
        case 'd':
            debug_mode = true;
            break;
        case 'v':
            PRINTF("version %d.%d\n", VERSION_MAJOR, VERSION_MINOR);
            return 0;
        default:
            exit(1);
        }
    }

    // init logging
    logmsg_init(debug_mode ? "stderr" : "none");
    INFO("STARTING %s\n", argv[0]);

    // verify args
    if (help_mode || (user_name == NULL) || (password == NULL) || (argc-optind != 1)) {
        PRINTF("usage: nettest_to_wc <wc_name>\n");
        PRINTF("  -P: use proxy server\n");
        PRINTF("  -u <user_name>: override WC_USERNAME environment value\n");
        PRINTF("  -p <password>: override WC_PASSWORD environment value\n");
        PRINTF("  -h: display this help text\n");
        PRINTF("  -d: enable debug mode\n");
        PRINTF("  -v: display version and exit\n");
        return 1;
    }
    wc_name = argv[optind];

    // run the nettest
    nettest_to_webcam(user_name, password, wc_name);

    // return success
    INFO("TERMINATING %s\n", argv[0]);
    return 0;
}

// -----------------  NETTEST TO WEBCAM  ---------------------------------

void nettest_to_webcam(char * user_name, char * password, char * wc_name)
{
    char      s[100], cmd[100];
    int       val, cnt, connect_status;
    pthread_t send_thread_id, recv_thread_id;

    // connect to the webcam
    if (p2p_init(1) < 0) {
        PRINTF("p2p_init failed\n");
        exit(1);  
    }
    handle = p2p_connect(user_name, password, wc_name, SERVICE_NETTEST, &connect_status);
    if (handle < 0) {
        PRINTF("connect to %s failed, %s\n", wc_name, status2str(connect_status));
        exit(1);
    }

    // log starting notice
    PRINTF("Starting network speed test, webcam %s\n", wc_name);
    if (p2p == &p2p1) {
        PRINTF("network data is being sent directly to the webcam.\n");
    } else {
        PRINTF("network data is being sent to the webcam via server, port 80.\n");
    }

    // create threads
    // - monitor connection stats
    // - send data
    // - receive data
    p2p_monitor_ctl(handle,10);
    pthread_create(&recv_thread_id, NULL, recv_thread, NULL);
    pthread_create(&send_thread_id, NULL, send_thread, NULL);

    // get and process commands
    // - send [<bytes>]
    // - recv [<bytes>]
    // - stop
    // - exit
    while (true) {
        // read command 
        val = 0;
        cmd[0] = '\0';
        if (fgets(s, sizeof(s), stdin) == NULL) {
            PRINTF("nettest is exitting\n");
            goto normal_completion;
        }
        cnt = sscanf(s, "%s %d", cmd, &val);
        if (cnt <= 0) {
            continue;
        }

        // if send or receive thread had an error then goto error_completion
        if (send_thread_exitted || recv_thread_exitted) {
            goto error_completion;
        }

        // process the command
        if (strcmp(cmd, "send") == 0) {
            if (cnt < 2) {
                val = NT_DEFAULT_SEND_OR_RECV_LEN;
            }
            if (val < 0 || val > NT_MAX_SEND_OR_RECV_LEN) {
                ERROR("max length is %d\n", NT_MAX_SEND_OR_RECV_LEN);
                continue;
            }
            send_bytes = val;
            PRINTF("nettest send datalen is now %d\n", send_bytes);
        } else if (strcmp(cmd, "recv") == 0) {
            if (cnt < 2) {
                val = NT_DEFAULT_SEND_OR_RECV_LEN;
            }
            if (val < 0 || val > NT_MAX_SEND_OR_RECV_LEN) {
                ERROR("max length is %d\n", NT_MAX_SEND_OR_RECV_LEN);
                continue;
            }
            if (webcam_request(NT_MSG_TYPE_REQUEST_SEND_DATA, val) == -1) {
                ERROR("failed to issue webcam send_data request, exitting nettest\n");
                goto error_completion;
            }
            PRINTF("nettest recv datalen is now %d\n", val);
        } else if (strcmp(cmd, "stop") == 0) {
            send_bytes = 0; 
            if (webcam_request(NT_MSG_TYPE_REQUEST_SEND_DATA, 0) == -1) {
                ERROR("failed to issue webcam send_data request, exitting nettest\n");
                goto error_completion;
            }
            PRINTF("nettest send and recv are stopped\n");
        } else if (strcmp(cmd, "exit") == 0) {
            PRINTF("nettest is exitting\n");
            goto normal_completion;
        } else {
            ERROR("invalid cmd '%s'\n", cmd);
        }
    }

normal_completion:
    // stop stats monitor
    p2p_monitor_ctl(handle,0);
    
    // shutdown send thread
    send_thread_exit_req = true;
    pthread_join(send_thread_id,NULL);

    // send request to tell the other side to exit,
    // our receive thread will exit when it sees the response
    if (webcam_request(NT_MSG_TYPE_REQUEST_EXIT, val) == -1) {
        ERROR("failed to issue webcam exit request\n");
    }

    // wait for recv thread to exit
    pthread_join(recv_thread_id,NULL);

error_completion:
    // call disconnect
    if (p2p_disconnect(handle) < 0) {
        ERROR("p2p_disconnect\n");
    }
    PRINTF("nettest complete\n");
}

void * send_thread(void * cx)
{
    nt_msg_t * msg;
    int        i, sb, ret;

    // allocate and init msg
    msg = malloc(sizeof(nt_msg_t)+NT_MAX_SEND_OR_RECV_LEN);
    bzero(msg, sizeof(nt_msg_t));
    for (i = 0; i < NT_MAX_SEND_OR_RECV_LEN; i++) {
        msg->data[i] = i;
    }

    // while not requested to exit
    while (send_thread_exit_req == false) {
        // if not being asked to send data then sleep for 10 ms
        sb = send_bytes;
        if (sb == 0) {
            usleep(100000);
            continue;
        }

        // we are being asked to send data, send the requested length
        msg->msg_type = NT_MSG_TYPE_DATA;
        msg->val0 = sb;
        ret = p2p_send(handle, msg, sizeof(nt_msg_t)+sb);
        if (ret != sizeof(nt_msg_t)+sb) {
            ERROR("p2p_send ret %d, expected %d\n", ret, (int)(sizeof(nt_msg_t)+sb));
            break;
        }
    }

    // free msg
    free(msg);

    // set send_thread_exitted flag
    PRINTF("send_thread exitted\n");
    send_thread_exitted = true;
    return NULL;
}

void * recv_thread(void * cx)
{
    nt_msg_t * msg;
    int     ret, datalen;

    // allocate msg
    msg = calloc(1,sizeof(nt_msg_t)+NT_MAX_SEND_OR_RECV_LEN);

    // receive and process msgs
    while (true) {
        // read the nt_msg_t
        ret = p2p_recv(handle, msg, sizeof(nt_msg_t), RECV_WAIT_ALL);
        if (ret != sizeof(nt_msg_t)) {
            ERROR("p2p_recv for nt_msg_t len %d, ret=%d\n", (int)sizeof(nt_msg_t), ret);
            goto done;
        }

        // process the msg
        switch (msg->msg_type) {
        case NT_MSG_TYPE_DATA:
            // verify datalen 
            datalen = msg->val0;
            if (datalen < 1 || datalen > NT_MAX_SEND_OR_RECV_LEN) {
                ERROR("recvd invalid datalen %d\n", datalen);
                goto done;
            }

            // read data
            ret = p2p_recv(handle, msg->data, datalen, RECV_WAIT_ALL);
            if (ret != datalen) {
                ERROR("p2p_recv for data buffer len %d, ret=%d\n", datalen, ret);
                goto done;
            }
            break;
        case NT_MSG_TYPE_RESPONSE:
            msg_response_id  = msg->val0;
            if (msg->val2 == NT_MSG_TYPE_REQUEST_EXIT) {
                goto done;
            }
            break;
        default:
            ERROR("recv_thread received invalid msg type %d\n", msg->msg_type);
            goto done;
        }
    }

done:
    // free msg
    free(msg);

    // set recv_thread_exitted flag, and exit this thread
    PRINTF("recv_thread exitted\n");
    recv_thread_exitted = true;
    return NULL;
}

int webcam_request(int msg_type, int msg_data)
{
    #define TOUT     30000    // 30 secs
    #define INTERVAL 10       // 10 ms

    nt_msg_t msg;
    int ret, ms;
    static int msg_request_id;

    // construct the msg
    bzero(&msg, sizeof(nt_msg_t));
    msg.msg_type = msg_type;
    msg.val0     = ++msg_request_id;
    msg.val1     = msg_data;

    // clear the msg_response_id
    msg_response_id = 0;

    // send the msg
    ret = p2p_send(handle, &msg, sizeof(nt_msg_t));
    if (ret < 0) {
        ERROR("webcam_request p2p_send failed\n");
        return -1;
    }

    // wait for the recv thread to set the response received flag
    for (ms = 0; msg_response_id != msg_request_id && ms < TOUT; ms += INTERVAL) {
        usleep(INTERVAL*1000);
    }
    if (msg_response_id != msg_request_id) {
        ERROR("webcam_request failed to receive response\n");
        return -1;
    }

    // return success
    return 0;
}
