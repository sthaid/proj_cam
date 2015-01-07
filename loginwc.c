#include "wc.h"

// XXX p2p2 update for disconnect

//
// defines 
//

//
// variables
//

p2p_routines_t * p2p;
int              handle;
sigset_t         sig_set;

//
// prototypes
//

void * read_from_terminal_and_write_to_shell(void * cx);
void * sigwinch_thread(void * cx);
void send_window_size_msg(void);

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit  rl;
    char         * user_name;
    char         * password;
    char         * wc_name;
    pthread_t      thread;
    int            len, rc, connect_status;
    char           buf[MAX_SHELL_DATA_LEN], opt_char;
    struct termios termios;
    struct termios termios_initial;
    bool           debug_mode = false;
    bool           help_mode = false;

    // set resource limti to allow core dumps
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    rc = setrlimit(RLIMIT_CORE, &rl);
    if (rc < 0) {
        WARN("setrlimit for core dump, %s\n", strerror(errno));
    }

    // get user_name and password from environment
    user_name = getenv("WC_USER_NAME");
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
            return 1;
        }
    }

    // init logging
    logmsg_init(debug_mode ? "stderr" : "none");
    INFO("STARTING %s\n", argv[0]);

    // verify args
    if (help_mode || (user_name == NULL) || (password == NULL) || (argc-optind != 1)) {
        PRINTF("usage: loginwc <wc_name>\n");
        PRINTF("  -P: use http proxy server\n");
        PRINTF("  -u <user_name>: override WC_USER_NAME environment value\n");
        PRINTF("  -p <password>: override WC_PASSWORD environment value\n");
        PRINTF("  -h: display this help text\n");
        PRINTF("  -d: enable debug mode\n");
        PRINTF("  -v: display version and exit\n");
        return 1;
    }
    wc_name = argv[optind];

    // perform initialization for the sigwinch_thread, this needs to be
    // done prior to any pthread_create calls
    sigemptyset(&sig_set);
    sigaddset(&sig_set, SIGWINCH);
    pthread_sigmask(SIG_BLOCK, &sig_set, NULL);

    // connect to the webcam
    handle = p2p_connect(user_name, password, wc_name, SERVICE_SHELL, &connect_status);
    if (handle < 0) {
        PRINTF("connect to %s failed, %s\n", wc_name, status2str(connect_status));
        return 1;
    }

    // set terminal to raw mode
    tcgetattr(0, &termios_initial);
    termios = termios_initial;
    cfmakeraw(&termios);
    tcsetattr(0, TCSANOW, &termios);

    // create threads
    pthread_create(&thread, NULL, sigwinch_thread, NULL);
    pthread_create(&thread, NULL, read_from_terminal_and_write_to_shell, NULL);

    // send window size to server
    send_window_size_msg();

    // read from shell and write to terminal
    while (true) {
        if ((len = p2p_recv(handle, buf, sizeof(buf), RECV_WAIT_ANY)) <= 0) {
            break;
        }

        if (write(1, buf, len) != len) {
            break;
        }
    }

    // restore terminal
    tcsetattr(0, TCSANOW, &termios_initial);

    // p2p disconnect
    p2p_disconnect(handle);

    // return success
    INFO("TERMINATING %s\n", argv[0]);
    return 0;
}

void * read_from_terminal_and_write_to_shell(void * cx)
{
    int  len;
    char buf[MAX_SHELL_DATA_LEN + sizeof(shell_msg_to_wc_hdr_t)];
    shell_msg_to_wc_hdr_t hdr;

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // read from terminal and write to shell
    while (true) {
        // read from terminal, len is number of bytes read
        if ((len = read(0, buf+sizeof(hdr), sizeof(buf)-sizeof(hdr))) <= 0) {
            break;
        }

        // write to shell 
        hdr.block_id = BLOCK_ID_DATA;
        hdr.u.block_id_data.len = len;
        memcpy(buf, &hdr, sizeof(hdr));
        if (p2p_send(handle, buf, sizeof(hdr)+len) != sizeof(hdr)+len) {
            break;
        }
    }

    // p2p disconnect
    p2p_disconnect(handle);

    // exit thread
    return NULL;
}

void * sigwinch_thread(void * cx)
{
    while (true) {
        // wait for SIGWINCH signal
        int signum = 0;
        sigwait(&sig_set, &signum);

        // call send_window_size which will read the new window size, and
        // send window size to server
        send_window_size_msg();
    }

    return NULL;
}

void send_window_size_msg(void)
{
    struct winsize ws;
    shell_msg_to_wc_hdr_t hdr;

    bzero(&ws, sizeof(ws));
    ioctl(0, TIOCGWINSZ, &ws);

    hdr.block_id = BLOCK_ID_WINSIZE;
    hdr.u.block_id_winsize.rows = ws.ws_row;
    hdr.u.block_id_winsize.cols = ws.ws_col;
    p2p_send(handle, &hdr, sizeof(hdr));
}
