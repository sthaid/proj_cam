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
    int             connect_status;
    bool            create_account = false;
    bool            debug_mode = false;
    bool            help_mode = false;

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
    user_name = getenv("WC_USERNAME");
    password  = getenv("WC_PASSWORD");

    // parse options
    while (true) {
        opt_char = getopt(argc, argv, "cu:p:hdv");
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

    // verify user_name, password, and args
    if (help_mode || (user_name == NULL) || (password == NULL) || (argc-optind != 0)) {
        PRINTF("usage: wc_admin\n");
        PRINTF("  -c: create account\n");
        PRINTF("  -u <user_name>: override WC_USERNAME environment value\n");
        PRINTF("  -p <password>: override WC_PASSWORD environment value\n");
        PRINTF("  -h: display this help text\n");
        PRINTF("  -d: enable debug mode\n");
        PRINTF("  -v: display version and exit\n");
        return 1;
    }

    // connect to CAMSERVER
    sfd = connect_to_admin_server(user_name, 
                                  password, 
                                  create_account ? "create" : "login",
                                  &connect_status);
    if (sfd == -1) {
        PRINTF("error: %s\n", status2str(connect_status));
        return 1;
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
    INFO("TERMINATING %s\n", argv[0]);
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
