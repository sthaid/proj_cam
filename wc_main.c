#include "wc.h"

// TBD LATER - improve debug interfaces

//
// defines
//

#define MAX_SERVICE_TBL (sizeof(service_tbl)/sizeof(service_tbl[0]))
#define MAX_GETCL_ARGV  10
#define MAX_HOSTNAME    100

//
// typedefs
//

typedef struct {
    int service;
    int (*init_proc)(void);
    void * (*service_proc)(void*);
    char * name;
    bool disabled;
} service_t;

//
// variables
//

p2p_routines_t * p2p = &p2p1;
char             wc_macaddr[MAX_WC_MACADDR+1];
int              debug_mode;

//
// prototypes
//

int wc_svc_webcam_init(void);

void * wc_svc_nettest(void * cx);
void * wc_svc_shell(void * cx);
void * wc_svc_webcam(void * cx);

void * debug_thread(void * cx);
bool getcl(int * argc, char ** argv);

//
// service table 
//

service_t service_tbl[] = {
    { SERVICE_NETTEST, NULL,               wc_svc_nettest, "nettest"  },
    { SERVICE_SHELL,   NULL,               wc_svc_shell,   "shell"    },
    { SERVICE_WEBCAM,  wc_svc_webcam_init, wc_svc_webcam,  "webcam"   },
            };

// -----------------  MAIN  ---------------------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit  rl;
    pthread_t      thread;
    int            i, ret, tries, handle, service;
    char           user_name[MAX_USER_NAME+1];
    char           opt_char;
    pthread_attr_t thread_attr;

    // set resource limti to allow core dumps
    rl.rlim_cur = RLIM_INFINITY;
    rl.rlim_max = RLIM_INFINITY;
    ret = setrlimit(RLIMIT_CORE, &rl);
    if (ret < 0) {
        WARN("setrlimit for core dump\n");
    }

    // parse options
    while (true) {
        opt_char = getopt(argc, argv, "d");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'd':
            debug_mode++;
            break;
        default:
            exit(1);
        }
    }

    // initialize message logging
    logmsg_init(debug_mode == 0 ? "wc_server.log" : "stderr");

    // get the wc_macaddr, this is used to identify this webcam in the wc_announce dgram
    tries = 0;
    while (true) {
        if (getmacaddr(wc_macaddr) < 0) {
            break;
        }
        if (++tries < 10) {
            WARN("unable to read wc_macaddr, retrying\n");
        } else {
            FATAL("unable to read wc_macaddr\n");
        }
        sleep(5);
    }
    INFO("wc_macaddr: %s\n", wc_macaddr);

    // init thread attribute for creating threads detached
    pthread_attr_init(&thread_attr);
    pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);

    // init services
    for (i = 0; i < MAX_SERVICE_TBL; i++) {
        if (service_tbl[i].init_proc) {
            ret = service_tbl[i].init_proc();
            if (ret < 0) {
                ERROR("service %s failed to initialize, disabling this service\n",
                      service_tbl[i].name);
                service_tbl[i].disabled = true;
            }
        }
    }

    // create debug_thread
    if (debug_mode > 0) {
        pthread_create(&thread, NULL, debug_thread, NULL);
    }

    // loop forever
    while (true) {
        // accept a connection         
        handle = p2p_accept(wc_macaddr, &service, user_name);
        if (handle < 0) {
            ERROR("p2p_accept failed\n");
            sleep(10);
            continue;
        }

        // scan the service_tbl for the requested service
        for (i = 0; i < MAX_SERVICE_TBL; i++) {
            if (service_tbl[i].service == service) {
                break;
            }
        }
        if (i == MAX_SERVICE_TBL) {
            ERROR("invalid service %d requested by user %s\n", service, user_name);
            p2p_disconnect(handle);
            continue;
        }

        // check if this service is disabled
        if (service_tbl[i].disabled) {
            ERROR("unable to start  service %s for user %s, it is disabled\n", 
                   service_tbl[i].name, user_name);
            p2p_disconnect(handle);
            continue;
        }

        // create the service thread 
        INFO("starting service %s for user %s\n", service_tbl[i].name, user_name);
        pthread_create(&thread, &thread_attr, service_tbl[i].service_proc, (void*)(long)handle);
    }

    return 0;
}

// -----------------  DEBUG  --------------------------------------------------------

void * debug_thread(void * cx)
{
    int    argc;
    char * argv[MAX_GETCL_ARGV];

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    while (true) {
        // print prompt and read cmd/args
        PRINTF("DEBUG> ");
        if (getcl(&argc,argv) == false) {
            break;
        }

        // if blank line then continue
        if (argc == 0) {
            continue;
        }

        // cmd: help
        if (strcmp(argv[0], "help") == 0) {
            PRINTF("p2p_debug_con [<handle>]\n");
            PRINTF("p2p_monitor_ctl <handle> <secs>\n");
            continue;
        }

        // cmd: p2p_debug_con 
        if (strcmp(argv[0], "p2p_debug_con") == 0) {
            int handle;

            if (argc == 1) {
                handle = -1;
            } else if (sscanf(argv[1], "%d", &handle) != 1) {
                PRINTF("usage: p2p_debug_con [<handle>]\n");
                continue;
            }
            p2p_debug_con(handle);
            continue;
        }

        // cmd: p2p_monitor_ctl
        if (strcmp(argv[0], "p2p_monitor_ctl") == 0) {
            int handle, secs;

            if (argc != 3 || 
                sscanf(argv[1], "%d", &handle) != 1 ||
                sscanf(argv[2], "%d", &secs) != 1)
            {
                PRINTF("usage: p2p_monitor_ctl <handle> <secs>\n");
                continue;
            }
            p2p_monitor_ctl(handle, secs);
            continue;
        }
    }

    INFO("program terminating\n");
    exit(0);
}

bool getcl(int * argc, char ** argv)
{
    char * saveptr;
    char * token;
    char   b[100];

    *argc = 0;

    if (fgets(b, sizeof(b), stdin) == NULL) {
        return false;
    }

    while (true) {
        token = strtok_r(*argc==0?b:NULL, "   \n", &saveptr);
        if (token == NULL) {
            return true;
        }
        argv[*argc] = token;
        *argc = *argc + 1;
        if (*argc == MAX_GETCL_ARGV) {
            return true;
        }
    }
}

