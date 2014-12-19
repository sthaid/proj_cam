#include "wc.h"

#include <readline/readline.h>
#include <readline/history.h>

//
// defines 
//

//
// prototypes
//

void * server_output_thread(void * cx) ;

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    int             ret;
    char            s[1000];
    char          * s1;
    char          * user_name;
    char          * password;
    pthread_t       thread;
    int             sfd;
    struct rlimit   rl;
    char            opt_char;
    bool            access_denied;
    bool            create_account = false;

    // set stdout to unbuffered
    setvbuf(stdout, NULL, _IONBF, 0);

    // set resource limti to allow core dumps
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    ret = setrlimit(RLIMIT_CORE, &rl);
    if (ret < 0) {
        WARN("setrlimit for core dump, %s\n", strerror(errno));
    }

    // get user_name and password from environment
    user_name = getenv("WC_USER_NAME");
    password  = getenv("WC_PASSWORD");

    // parse options
    while (true) {
        opt_char = getopt(argc, argv, "cu:p:");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'c':
            create_account = true;
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

    // verify user_name, password, and args
    if ((user_name == NULL) || (password == NULL) || (argc-optind != 0)) {
        PRINTF("usage: wc_admin\n");
        PRINTF("  -c: create account\n");
        PRINTF("  -u <user_name>: override WC_USER_NAME environment value\n");
        PRINTF("  -p <password>: override WC_PASSWORD environment value\n");
        return 1;
    }

    // connect to CAMSERVER
    sfd = connect_to_cloud_server(user_name, 
                                  password, 
                                  create_account ? "create" : "login",
                                  &access_denied);
    if (sfd == -1) {
        FATAL("unable to connect to server - %s\n",
              access_denied ? "access denied" : "host unreachable");
    }

    // create thread to copy output from server to stdout
    pthread_create(&thread, NULL, server_output_thread, (void*)(long)sfd);

    // loop, copying stdin to server input
    while (true) {
        if ((s1 = readline("")) == NULL) {
            break;
        }
        if (s1[0] != '\0') {
            add_history(s1);
        }

        sprintf(s, "%s\n", s1);
        if (write(sfd, s, strlen(s)) <= 0) {
            break;
        }

        free(s1);
    }

    // return
    PRINTF("\n");
    return 0;
}

void * server_output_thread(void * cx) 
{
    int sfd = (int)(long)cx;
    char s[1000];
    int len;

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // loop, copying server output to stdout
    while (true) {
        len = read(sfd, s, sizeof(s)-1);
        if (len <= 0) {
            break;
        }
        s[len] = '\0';

        fputs(s, stdout);
    }

    // send SIGTERM, this will invoke the readline signal handler which will restore
    // the terminal attributes, followed by program terminate
    kill(getpid(), SIGTERM);
    pause();

    // return
    return NULL;
}
