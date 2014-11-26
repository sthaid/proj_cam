#include "wc.h"

//
// defines 
//

//
// variables
//

p2p_routines_t * p2p;
int              handle;

//
// prototypes
//

void * read_from_terminal_and_write_to_bash(void * cx);

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit  rl;
    char         * user_name;
    char         * password;
    char         * wc_name;
    pthread_t      thread;
    int            len, rc;
    char           buf[1000], opt_char;
    struct termios termios;
    struct termios termios_initial;

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
        opt_char = getopt(argc, argv, "Pu:p:");
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
        default:
            exit(1);
        }
    }

    // verify args
    if ((user_name == NULL) || (password == NULL) || (argc-optind != 1)) {
        PRINTF("usage: loginwc <wc_name>\n");
        PRINTF("  -P: use http proxy server\n");
        PRINTF("  -u <user_name>: override WC_USER_NAME environment value\n");
        PRINTF("  -p <password>: override WC_PASSWORD environment value\n");
        return 1;
    }
    wc_name = argv[optind];

    // connect to the webcam
    handle = p2p_connect(user_name, password, wc_name, SERVICE_SHELL);
    if (handle < 0) {
        ERROR("p2p_connect to %s failed\n", wc_name);
        return 1;
    }

    // set terminal to raw mode
    tcgetattr(0, &termios_initial);
    termios = termios_initial;
    cfmakeraw(&termios);
    tcsetattr(0, TCSANOW, &termios);

    // create thread to read from terminal and write to bash
    pthread_create(&thread, NULL, read_from_terminal_and_write_to_bash, NULL);

    // read from bash and write to terminal
    while (true) {
        if ((len = p2p_recv(handle, buf, sizeof(buf), RECV_WAIT_ANY)) <= 0) {
            break;
        }
        if ((rc = write(1, buf, len)) != len) {
            break;
        }
    }

    // restore terminal
    tcsetattr(0, TCSANOW, &termios_initial);

    // p2p disconnect
    p2p_disconnect(handle);

    // return success
    return 0;
}

void * read_from_terminal_and_write_to_bash(void * cx)
{
    int  len, rc;
    char buf[10000];

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // read from terminal and write to bash
    while (true) {
        if ((len = read(0, buf, sizeof(buf))) <= 0) {
            break;
        }
        if ((rc = p2p_send(handle, buf, len)) != len) {
            break;
        }
    }

    // p2p disconnect
    p2p_disconnect(handle);

    // exit thread
    return NULL;
}
