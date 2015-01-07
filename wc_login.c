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
    int             handle;
    int             fdm;
    bool            p2p_disconnected;
} thread_cx_t;

//
// variables
//

//
// prototypes
//

void * read_from_shell_and_write_to_client(void * cx);

// -----------------  SERVICE SHELL  ----------------------------------------------------

void * wc_svc_shell(void * cx)
{
    int                   handle = (int)(long)cx;
    int                   fdm;
    pthread_t             thread;
    char                * slavename;
    thread_cx_t         * thread_cx;
    pid_t                 pid;
    int                   len, rc;
    char                  buf[MAX_SHELL_DATA_LEN];
    shell_msg_to_wc_hdr_t hdr;
    struct winsize        ws;

    // init ptm - set fdm and slavename
    if ((fdm = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC)) < 0) {
        perror("open /dev/ptmx");
        p2p_disconnect(handle);
        return NULL;
    }
    grantpt(fdm);
    unlockpt(fdm);
    slavename = ptsname(fdm);

    // fork and execute child 
    if ((pid = fork()) == 0) {
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

        // exec /bin/bash
#if 1
        char * envp[] = { "TERM=xterm", "HOME=/home/pi", (char*)NULL };
        execle("/bin/bash", "-", (char*)NULL, envp);
#else
        execl("/bin/login", "TERM=xterm", (char*)NULL);
#endif
        exit(1);
    }

    // parent executes here ...

    // create thread to read from client and sent to shell
    thread_cx = malloc(sizeof(thread_cx_t));
    thread_cx->handle           = handle;
    thread_cx->fdm              = fdm;
    thread_cx->p2p_disconnected = false;
    pthread_create(&thread, NULL, read_from_shell_and_write_to_client, thread_cx);

    // read from client and, based on received block_id either:
    // - send data to shell, OR
    // - set new window size
    while (true) {
        if ((rc = p2p_recv(handle, &hdr, sizeof(hdr), RECV_WAIT_ALL)) != sizeof(hdr)) {
            goto done;
        }

        switch (hdr.block_id) {
        case BLOCK_ID_DATA:
            len = hdr.u.block_id_data.len;
            if (len > MAX_SHELL_DATA_LEN) {
                goto done;
            }
            if ((rc=p2p_recv(handle, buf, len, RECV_WAIT_ALL)) != len) {
                goto done;  
            }
            if (write(thread_cx->fdm, buf, len) != len) {
                goto done;  
            }
            break;

        case BLOCK_ID_WINSIZE:
            bzero(&ws, sizeof(ws));
            ws.ws_row = hdr.u.block_id_winsize.rows;
            ws.ws_col = hdr.u.block_id_winsize.cols;
            rc = ioctl(thread_cx->fdm, TIOCSWINSZ, &ws);
            break;

        default:
            ERROR("hdr.block_id 0x%x invalid\n", hdr.block_id);
            goto done;
        }
    }

done:
    // close pseudo term master,
    // p2p_disconnect,
    // wait for thread to exit  ,
    // wait for shell to exit,
    // free thread_cx
    close(fdm);
    p2p_disconnect(handle);
    pthread_join(thread,NULL);
    waitpid(pid, NULL, 0);
    free(thread_cx);

    // done
    INFO("shell exit\n");
    return NULL;
}
    
void * read_from_shell_and_write_to_client(void * cx)
{
    thread_cx_t * thread_cx = cx;
    int           len;
    char          buf[MAX_SHELL_DATA_LEN];

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
