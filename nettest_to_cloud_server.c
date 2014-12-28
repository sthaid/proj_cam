#include "wc.h"

//
// defines 
//

//
// variables
//

//
// prototypes
//

void nettest_to_server(char * user_name, char * password);

// -----------------  MAIN  ----------------------------------------------

int main(int argc, char **argv)
{
    struct rlimit rl;
    char        * user_name;
    char        * password;
    int           ret;
    char          opt_char;

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
        opt_char = getopt(argc, argv, "u:p:v");
        if (opt_char == -1) {
            break;
        }
        switch (opt_char) {
        case 'u':
            user_name = optarg;
            break;
        case 'p':
            password = optarg;
            break;
        case 'v':
            PRINTF("version %d.%d\n", VERSION_MAJOR, VERSION_MINOR);
            return 0;
        default:
            exit(1);
        }
    }

    // verify user_name, password, and args
    if ((user_name == NULL) || (password == NULL) || (argc-optind != 0)) {
        PRINTF("usage: wc_nettest_server\n");
        PRINTF("  -u <user_name>: override WC_USER_NAME environment value\n");
        PRINTF("  -p <password>: override WC_PASSWORD environment value\n");
        PRINTF("  -v: display version and exit\n");
        return 1;
    }

    // run network speed test to the server
    nettest_to_server(user_name, password);

    // return success
    return 0;
}

// -----------------  NETTEST TO SERVER  ---------------------------------

void nettest_to_server(char * user_name, char * password)
{
    int   sfd, len, i;
    long  start1, end1, start2, end2;
    char  buff[CLOUD_SERVER_MAX_NETTEST_BUFF];
    bool  access_denied;

    // connect to cloud_server
    sfd = connect_to_cloud_server(user_name, password, "nettest", &access_denied);
    if (sfd == -1) {
        FATAL("unable to connect to server - %s\n",
              access_denied ? "access denied" : "host unreachable");

    }

    // log starting notice
    PRINTF("Starting network speed test to %s\n", CLOUD_SERVER_HOSTNAME);
    PRINTF("\n");
    PRINTF("ClientToServer    ServerToClient\n");
    PRINTF("  (Mbit/Sec)        (Mbit/Sec)  \n");

    // init buff 
    bzero(buff,sizeof(buff));
    buff[0] = 0x11;
    buff[sizeof(buff)-1] = 0x77;

    // start testing
    for (i = 0; i < 10; i++) {
        // client->server test
        // . save start time, and 
        // . send data to server
        // . read 1 byte from server, and verify
        // . save end time
        start1 = microsec_timer();
        len = write(sfd, buff, sizeof(buff));
        if (len != sizeof(buff)) {
            FATAL("writing buff, %s\n", strerror(errno));
        }
        len = recv(sfd, buff, 1, MSG_WAITALL);
        if (len != 1) {
            FATAL("reading buff, %s\n", strerror(errno));
        }
        end1 = microsec_timer();

        // server->client test
        // . receive data from server
        // . save end time
        start2 = end1;
        len = recv(sfd, buff+1, sizeof(buff)-1, MSG_WAITALL);
        if (len != sizeof(buff)-1) {
            FATAL("reading buff, %s\n", strerror(errno));
        }
        end2 = microsec_timer();

        // verify first and last location in buff 
        if (buff[0] != 0x11 || buff[sizeof(buff)-1] != 0x77) {
            FATAL("verify buff, 0x%2.2x 0x%2.2x\n",
                   buff[0], buff[sizeof(buff)-1]);
        }

        // print result 
        PRINTF("%10d        %10d\n",
             (int)((8 * sizeof(buff)) / (end1 - start1)),
             (int)((8 * sizeof(buff)) / (end2 - start2)));
    }
}

