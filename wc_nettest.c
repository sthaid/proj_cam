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
// typedefs
//

typedef struct {
    int  handle;
    int  send_bytes;
    bool send_thread_exit_req;
} nt_vars_t;

//
// variables
//

//
// prototypes
//

void * send_thread(void * cx);

// -----------------  SERVICE NETTEST  --------------------------------------------------

void * wc_svc_nettest(void * cx)
{
    int         handle = (int)(long)cx;
    nt_msg_t    msg_response;
    int         ret, datalen;
    pthread_t   thread = 0;
    nt_vars_t * nt  = NULL;
    nt_msg_t  * msg = NULL;

    // allocate memory for this instance and init
    nt = malloc(sizeof(nt_vars_t));
    if (nt == NULL) {
        goto done;
    }
    nt->handle = handle;
    nt->send_bytes = 0;
    nt->send_thread_exit_req = false;

    // allocate msg buffer
    msg = calloc(1,sizeof(nt_msg_t)+NT_MAX_SEND_OR_RECV_LEN);
    if (msg == NULL) {
        goto done;
    }

    // create the send_thread
    pthread_create(&thread, NULL, send_thread, nt);

    // receive and process msgs
    while (true) {
        // read the msg_t
        ret = p2p_recv(nt->handle, msg, sizeof(nt_msg_t), RECV_WAIT_ALL);
        if (ret != sizeof(nt_msg_t)) {
            ERROR("p2p_recv msg\n");
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
            ret = p2p_recv(nt->handle, msg->data, datalen, RECV_WAIT_ALL);
            if (ret != datalen) {
                ERROR("p2p_recv for data buffer len %d, ret=%d\n", datalen, ret);
                goto done;
            }
            break;
        case NT_MSG_TYPE_REQUEST_SEND_DATA:
            // verify new value for nt.send_bytes is in range
            if (msg->val1 < 0 || msg->val1 > NT_MAX_SEND_OR_RECV_LEN) {
                ERROR("request send_data datalen %d invalid\n", msg->val1);
                goto done;
            }

            // set new value of nt.send_bytes
            nt->send_bytes = msg->val1;
            INFO("nettest send datalen is now %d\n", nt->send_bytes);

            // send response
            msg_response.msg_type = NT_MSG_TYPE_RESPONSE;
            msg_response.val0 = msg->val0;      // echo the request msg id
            msg_response.val1 = 0;              // response data, not used
            msg_response.val2 = msg->msg_type;  // echo the request msg_type
            ret = p2p_send(nt->handle, &msg_response, sizeof(msg_response));
            if (ret != sizeof(msg_response)) {
                ERROR("p2p_send NT_MSG_TYPE_RESPONSE ret %d, expected %d\n", 
                    ret, (int)(sizeof(msg_response)));
                goto done;
            }
            break;
        case NT_MSG_TYPE_REQUEST_EXIT:
            // issue notice
            INFO("nettest exit request received\n");

            // send response
            msg_response.msg_type = NT_MSG_TYPE_RESPONSE;
            msg_response.val0 = msg->val0;      // echo the request msg id
            msg_response.val1 = 0;              // response data, not used
            msg_response.val2 = msg->msg_type;  // echo the request msg_type
            ret = p2p_send(nt->handle, &msg_response, sizeof(msg_response));
            if (ret != sizeof(msg_response)) {
                ERROR("p2p_send NT_MSG_TYPE_RESPONSE ret %d, expected %d\n", 
                    ret, (int)(sizeof(msg_response)));
                goto done;
            }

            // exit
            goto done;
            break;
        default:
            ERROR("recv_thread received invalid msg type %d\n", msg->msg_type);
            goto done;
            break;
        }
    }

done:
    // shutdown send thread
    if (thread) {
        nt->send_thread_exit_req = true;
        pthread_join(thread,NULL);
    }

    // call disconnect
    if (p2p_disconnect(nt->handle) < 0) {
        ERROR("p2p_disconnect\n");
    }

    // free allocations
    if (nt) {
        free(nt);
    }
    if (msg) {
        free(msg);
    }
    
    // exit thread
    INFO("nettest complete\n");
    return NULL;
}

void * send_thread(void * cx)
{
    nt_vars_t * nt = cx;
    nt_msg_t  * msg = NULL;
    int         i, sb, ret;

    // allocate and init msg
    msg = malloc(sizeof(nt_msg_t)+NT_MAX_SEND_OR_RECV_LEN);
    if (msg == NULL) {
        return NULL;
    }
    bzero(msg, sizeof(nt_msg_t));
    for (i = 0; i < NT_MAX_SEND_OR_RECV_LEN; i++) {
        msg->data[i] = i;
    }

    // while not requested to exit
    while (nt->send_thread_exit_req == false) {
        // if not being asked to send data then sleep for 10 ms
        sb = nt->send_bytes;
        if (sb == 0) {
            usleep(100000);
            continue;
        }

        // we are being asked to send data, send the requested length
        msg->msg_type = NT_MSG_TYPE_DATA;
        msg->val0 = sb;
        ret = p2p_send(nt->handle, msg, sizeof(nt_msg_t)+sb);
        if (ret != sizeof(nt_msg_t)+sb) {
            ERROR("p2p_send NT_MSG_TYPE_DATA ret %d, expected %d\n", 
                  ret, (int)(sizeof(nt_msg_t)+sb));
            break;
        }
    }

    // free msg
    free(msg);

    // exit thread
    INFO("send_thread exitted\n");
    return NULL;
}
