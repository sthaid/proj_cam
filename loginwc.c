#include "wc.h"

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
int send_init_msg(void);
int send_window_size_msg(void);

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit  rl;
    char         * user_name;
    char         * password;
    char         * wc_name;
    pthread_t      thread;
    int            len, rc, connect_status;
    char           buf[10000], opt_char;
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
            return 1;
        }
    }

    // init logging
    logmsg_init(debug_mode ? "stderr" : "none");
    INFO("STARTING %s\n", argv[0]);

    // verify args
    if (help_mode || (user_name == NULL) || (password == NULL) || (argc-optind != 1)) {
        PRINTF("usage: loginwc <wc_name>\n");
        PRINTF("  -P: use proxy server\n");
        PRINTF("  -u <user_name>: override WC_USERNAME environment value\n");
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
    if (p2p_init(1) < 0) {
        PRINTF("p2p_init failed\n");
        return 1;
    }
    handle = p2p_connect(user_name, password, wc_name, SERVICE_SHELL, &connect_status);
    if (handle < 0) {
        PRINTF("connect to %s failed, %s\n", wc_name, status2str(connect_status));
        return 1;
    }

    // send init message to the shell server
    send_init_msg();

    // set terminal to raw mode
    tcgetattr(0, &termios_initial);
    termios = termios_initial;
    cfmakeraw(&termios);
    tcsetattr(0, TCSANOW, &termios);

    // create threads
    pthread_create(&thread, NULL, sigwinch_thread, NULL);
    pthread_create(&thread, NULL, read_from_terminal_and_write_to_shell, NULL);

    // read from shell and write to terminal
    while (true) {
        if ((len = p2p_recv(handle, buf, sizeof(buf), RECV_WAIT_ANY)) <= 0) {
            break;
        }

        if (write(1, buf, len) != len) {
            break;
        }
    }

    // restore terminal, and
    // p2p disconnect
    tcsetattr(0, TCSANOW, &termios_initial);
    p2p_disconnect(handle);

    // return success
    INFO("TERMINATING %s\n", argv[0]);
    return 0;
}

void * read_from_terminal_and_write_to_shell(void * cx)
{
    int  len;
    char buf[MAX_SHELL_MSGID_DATA_DATALEN + sizeof(shell_msg_hdr_t)];
    shell_msg_hdr_t hdr;

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // read from terminal and write to shell
    while (true) {
        // read from terminal
        if ((len = read(0, buf+sizeof(hdr), sizeof(buf)-sizeof(hdr))) <= 0) {
            break;
        }

        // send data msg to shell
        hdr.msgid    = SHELL_MSGID_DATA;
        hdr.datalen  = len;
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
    int signum;

    while (true) {
        // wait for SIGWINCH signal
        signum = 0;
        sigwait(&sig_set, &signum);

        // send the new window size to shell
        send_window_size_msg();
    }

    return NULL;
}

int send_init_msg(void)
{
    int            ret;
    struct winsize ws;
    char         * term;
    struct {
        shell_msg_hdr_t hdr;
        shell_msg_init_t init;
    } msg;

    bzero(&ws, sizeof(ws));
    ret = ioctl(0, TIOCGWINSZ, &ws);
    if (ret < 0) {
        return ret;
    }

    term = getenv("TERM");

    bzero(&msg, sizeof(msg));
    msg.hdr.msgid   = SHELL_MSGID_INIT;
    msg.hdr.datalen = sizeof(shell_msg_init_t);
    msg.init.rows   = ws.ws_row;
    msg.init.cols   = ws.ws_col;
    strcpy(msg.init.term, term ? term : "");
    ret = p2p_send(handle, &msg, sizeof(msg));
    if (ret < 0) {
        return ret;
    }

    return 0;
}

int send_window_size_msg(void)
{
    int            ret;
    struct winsize ws;
    struct {
        shell_msg_hdr_t hdr;
        shell_msg_winsize_t ws;
    } msg;

    bzero(&ws, sizeof(ws));
    ret = ioctl(0, TIOCGWINSZ, &ws);
    if (ret < 0) {
        return ret;
    }

    bzero(&msg, sizeof(msg));
    msg.hdr.msgid   = SHELL_MSGID_WINSIZE;
    msg.hdr.datalen = sizeof(shell_msg_winsize_t);
    msg.ws.rows     = ws.ws_row;
    msg.ws.cols     = ws.ws_col;
    ret = p2p_send(handle, &msg, sizeof(msg));
    if (ret < 0) {
        return ret;
    }

    return 0;
}

