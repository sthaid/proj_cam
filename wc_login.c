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
// notes
// - to allow bin/login to login as root, add lines for 'pts/0', 'pts/1', ...
//   to /etc/securetty.  Refer to man page for securetty.  Also refer to
//   /etc/security/access.conf and man page for pam.
//

//
// defines
//

//
// typedefs
//

typedef struct {
    int handle;
    int fdm;
} thread_cx_t;

//
// variables
//

//
// prototypes
//

void * read_from_shell_and_write_to_client(void * cx);
int recv_msg(int handle, shell_msg_hdr_t * msg_hdr, void * msg_data, uint32_t max_msg_data, bool * eof);

// -----------------  SERVICE SHELL  ----------------------------------------------------

void * wc_svc_shell(void * cx)
{
    int                  handle;
    int                  fdm, rc;
    pthread_t            thread;
    bool                 thread_created;
    char               * slavename;
    thread_cx_t        * thread_cx;
    pid_t                pid;
    shell_msg_hdr_t      msg_hdr;
    shell_msg_init_t     msg_init;
    shell_msg_winsize_t  msg_winsize;
    char                 msg_buf[MAX_SHELL_MSGID_DATA_DATALEN];
    struct winsize       ws;
    bool                 eof;

    // init locals
    handle         = (int)(long)cx;
    fdm            = -1;
    thread_created = false;
    pid            = 0;
    thread_cx      = NULL;

    // init ptm - set fdm and slavename
    if ((fdm = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) {
        ERROR("open /dev/ptmx, %s\n", strerror(errno));
        goto done;
    }
    grantpt(fdm);
    unlockpt(fdm);
    slavename = ptsname(fdm);

    // recv the init msg
    rc = recv_msg(handle, &msg_hdr, &msg_init, sizeof(msg_init), &eof);
    if (rc < 0 || msg_hdr.msgid != SHELL_MSGID_INIT || msg_hdr.datalen != sizeof(shell_msg_init_t)) {
        ERROR("recv of msg_init, rc=%d msgid=0x%x datalen=%d\n", rc, msg_hdr.msgid, msg_hdr.datalen);
        goto done;
    }

    // set the initial window size
    bzero(&ws, sizeof(ws));
    ws.ws_row = msg_init.rows;
    ws.ws_col = msg_init.cols;
    ioctl(fdm, TIOCSWINSZ, &ws);

    // fork and execute child 
    if ((pid = fork()) == 0) {
        char * envp[2];
        char   env_term[300];

        // close parents stdin, stdout, stderr
        close(0);
        close(1);
        close(2);

        // creates a session and sets the process group ID
        setsid();

        // open child stdin,stdout,stderr
        open(slavename, O_RDWR);
        open(slavename, O_RDWR);
        open(slavename, O_RDWR);

        // login to webcam
        sprintf(env_term, "TERM=%s", msg_init.term);
        envp[0] = env_term;
        envp[1] = NULL;
        execle("/usr/bin/sudo", "/usr/bin/sudo", "/bin/login", (char*)NULL, envp);

        // not reached
        exit(1);
    }

    // parent executes here ...

    // create thread to read from shell and send to client
    thread_cx = malloc(sizeof(thread_cx_t));
    thread_cx->handle = handle;
    thread_cx->fdm    = fdm;
    rc = pthread_create(&thread, NULL, read_from_shell_and_write_to_client, thread_cx);
    if (rc != 0) {
        goto done;
    }
    thread_created = true;

    // get msg from client and process the received msg
    while (true) {
        rc = recv_msg(handle, &msg_hdr, msg_buf, sizeof(msg_buf), &eof);
        if (rc < 0) {
            goto done;
        }
        if (eof) {
            goto done;
        }

        switch (msg_hdr.msgid) {
        case SHELL_MSGID_DATA:
            if (msg_hdr.datalen == 0) {
                goto done;
            }

            if (write(fdm, msg_buf, msg_hdr.datalen) != msg_hdr.datalen) {
                goto done;  
            }
            break;

        case SHELL_MSGID_WINSIZE:
            if (msg_hdr.datalen != sizeof(shell_msg_winsize_t)) {
                goto done;
            }

            memcpy(&msg_winsize, msg_buf, sizeof(msg_winsize));
            bzero(&ws, sizeof(ws));
            ws.ws_row = msg_winsize.rows;
            ws.ws_col = msg_winsize.cols;
            ioctl(fdm, TIOCSWINSZ, &ws);
            break;

        default:
            goto done;
        }
    }

done:
    // close pseudo term master,
    // p2p_disconnect,
    // wait for thread to exit  ,
    // wait for shell to exit,
    // free thread_cx
    p2p_disconnect(handle);
    if (fdm != -1) {
        close(fdm);
    }
    if (thread_created) {
        pthread_join(thread,NULL);
    }
    if (pid != 0) {
        waitpid(pid, NULL, 0);
    }
    if (thread_cx != NULL) {
        free(thread_cx);
    }

    // terminate
    INFO("shell exit\n");
    return NULL;
}
    
void * read_from_shell_and_write_to_client(void * cx)
{
    thread_cx_t * thread_cx = cx;
    int           len;
    char          buf[10000];

    // read from shell and write to client
    while (true) {
        if ((len = read(thread_cx->fdm, buf, sizeof(buf))) <= 0) {
            break;
        }
        if (p2p_send(thread_cx->handle, buf, len) != len) {
            break;
        }
    }

    // p2p disconnect
    p2p_disconnect(thread_cx->handle);

    // exit thread
    return NULL;
}

int recv_msg(int handle, shell_msg_hdr_t * msg_hdr, void * msg_data, uint32_t max_msg_data, bool * eof)
{
    int rc;

    // preset returns
    bzero(msg_hdr, sizeof(shell_msg_hdr_t));
    *eof = false;

    // get the msg_hdr
    rc = p2p_recv(handle, msg_hdr, sizeof(shell_msg_hdr_t), RECV_WAIT_ALL);
    if (rc == 0) {
        *eof = true;
        return 0;
    }
    if (rc != sizeof(shell_msg_hdr_t)) {
        return -1;
    }

    // validata the datalen
    if (msg_hdr->datalen > max_msg_data) {
        return -1;
    }

    // get the msg_data
    rc = p2p_recv(handle, msg_data, msg_hdr->datalen, RECV_WAIT_ALL);
    if (rc != msg_hdr->datalen) {
        return -1;
    }

    // return success
    return 0;
}
