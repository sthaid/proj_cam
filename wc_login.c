#include "wc.h"

//
// notes
// - to allow bin/login to login as root, add lines for 'pts/0', 'pts/1', ...
//   to /etc/securetty.  Refer to man page for securetty.  Also refer to
//   /etc/security/access.conf and man page for pam.
//

// TBD LATER - window size when using vi, and SIGWINCH signal for resizing

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

void * read_from_bash_and_write_to_client(void * cx);

// -----------------  SERVICE SHELL  ----------------------------------------------------

void * wc_svc_shell(void * cx)
{
    int           handle = (int)(long)cx;
    int           fdm;
    pthread_t     thread;
    char        * slavename;
    thread_cx_t * thread_cx;
    pid_t         pid;
    int           len, rc;
    char          buf[10000];

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

        // exec bash
#if 1
        char * envp[] = { "TERM=xterm", "HOME=/home/pi", (char*)NULL };
        execle("/bin/bash", "-", (char*)NULL, envp);
#else
        execl("/bin/login", "TERM=xterm", (char*)NULL);
#endif
        exit(1);
    }

    // parent executes here ...

    // create thread to read from client and sent to bash
    thread_cx = malloc(sizeof(thread_cx_t));
    thread_cx->handle           = handle;
    thread_cx->fdm              = fdm;
    thread_cx->p2p_disconnected = false;
    pthread_create(&thread, NULL, read_from_bash_and_write_to_client, thread_cx);

    // read from client and write to bash
    while (true) {
        if ((len = p2p_recv(handle, buf, sizeof(buf), RECV_WAIT_ANY)) <= 0) {
            break;
        }
        if ((rc = write(thread_cx->fdm, buf, len)) != len) {
            break;
        }
    }

    // close pseudo term master
    close(fdm);

    // p2p_disconnect
    p2p_disconnect(handle);

    // wait for thread to exit  
    pthread_join(thread,NULL);

    // wait for bash to exit
    waitpid(pid, NULL, 0);

    // free thread_cx
    free(thread_cx);

    // done
    NOTICE("shell exit\n");
    return NULL;
}
    
void * read_from_bash_and_write_to_client(void * cx)
{
    thread_cx_t * thread_cx = cx;
    int           len, rc;
    char          buf[10000];

    // read from bash and write to client
    while (true) {
        if ((len = read(thread_cx->fdm, buf, sizeof(buf))) <= 0) {
            break;
        }
        if ((rc = p2p_send(thread_cx->handle, buf, len)) != len) {
            break;
        }
    }

    // p2p disconnect
    p2p_disconnect(thread_cx->handle);

    // exit thread
    return NULL;
}
