#include "wc.h"

// XXX 01/08/15 17:47:52 ERROR service_thread: reading http connect request, len=-1, Resource temporarily unavailable
// XXX nettest, print avg
// XXX need cmd to set access list
// XXX alphatize users in cmd_ls
// XXX some locking needed  

//
// defines
//

// max table entries
#define MAX_USER           1000
#define MAX_ONL_WC         1000
#define MAX_USER_WC        32
#define MAX_WC_ACCESS_LIST 32

// for calls to getcl
#define MAX_GETCL_BUFF     1000
#define MAX_GETCL_ARGV     100

// check if u is root
#define IS_ROOT(u) (strcmp((u)->user_name, "root") == 0)

// location of user account files
#define USER_DIR "user"

//
// typedefs
//

typedef struct {
    char wc_name[MAX_WC_NAME+1];
    bool is_owner;
    union {
        struct {
            char wc_macaddr[MAX_WC_MACADDR+1];
            char wc_access_list[MAX_WC_ACCESS_LIST+1];
        } owner;
        struct {
            char user_name[MAX_USER_NAME+1];
            char wc_name[MAX_WC_NAME+1];
        } link;
    } u;
} user_wc_t;

typedef struct {
    char     user_name[MAX_USER_NAME+1];
    char     password[MAX_PASSWORD+1];
    struct   timeval time_created;
    user_wc_t wc[MAX_USER_WC];
} user_t;

typedef struct {
    version_t version;
    char      wc_macaddr[MAX_WC_MACADDR+1];
    struct    sockaddr_in wc_addr;
    struct    sockaddr_in wc_addr_behind_nat;
    uint64_t  last_announce_rcv_time_us;
} onl_wc_t;

//
// variables
//

user_t           user[MAX_USER];
int              max_user;
onl_wc_t         onl_wc[MAX_ONL_WC];
int              max_onl_wc;
int              debug_mode;
__thread bool    is_root;
p2p_routines_t * p2p = &p2p1;

//
// prototypes
//

void * service_thread(void * cx);

void account_init(void);
int account_create(char * user_name, char * password);
void account_login(int sockfd, user_t * u);
void account_command(int sockfd, user_t * u);
void cmd_processor(user_t * u, FILE * fp, bool single_cmd);
bool cmd_help(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_add_wc(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_ren_wc(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_del_wc(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_ls(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_password(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_delete_account(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_version(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_exit(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_su(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_set_password(user_t * u, FILE * fp, int argc, char ** argv);
void read_all_user_files(void);
bool create_user_file(user_t * u);
bool update_user_file(user_t * u);
bool delete_user_file(user_t * u);
bool getcl(FILE * fp, char * b, int * argc, char ** argv);
void prcl(FILE * fp, char * fmt, ...) __attribute__ ((format (printf, 2, 3)));
void display_user(FILE * fp, user_t * u, bool verbose);
void display_user_wc(FILE * fp, user_t * u, int u_wc_idx, bool verbose);
void display_unclaimed(FILE * fp, bool verbose);
void display_unclaimed_wc(FILE * fp, int onl_wc_idx, bool verbose);
user_t * find_user(char * user_name);
user_wc_t * find_user_wc_from_user_and_name(user_t * u, char * wc_name);
user_wc_t * find_user_wc_from_macaddr(char * wc_macaddr, user_t ** u);
onl_wc_t * find_onl_wc_from_macaddr(char * wc_macaddr);
user_wc_t * find_wc_owner_from_wc_link(user_wc_t * link_wc);
bool verify_user_name_and_password(char * user_name, char *password, user_t ** u);
bool verify_wc_macaddr(char * wc_macaddr);
bool verify_wc_link(char * wc_link_str, char * link_user_name, char * link_wc_name);
bool verify_wc_access_permitted(user_wc_t * wc, char * user_name);
bool verify_wc_access_list(char * wc_access_list, int * status);
bool verify_user_name(char * user_name, int * status);
bool verify_password(char * password, int * status);
bool verify_wc_name(char * wc_name, int * status);
bool verify_chars(char * s);

void nettest(int sockfd);

void wccon(int sockfd, int handle); 
void * wccon_thread(void * cxarg);

void dgram_init(void);
void * dgram_thread(void * cx);
void * dgram_monitor_onl_wc_thread(void * cx);
uint64_t gen_con_id(void);

// -----------------  MAIN  ---------------------------------------

int main(int argc, char ** argv)
{
    struct rlimit      rl;
    char               opt_char;
    int                ret, listen_sockfd, sockfd;
    pthread_t          thread;
    socklen_t          addrlen;
    struct sockaddr_in addr;

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
    logmsg_init(debug_mode == 0 ? "admin_server.log" : "stderr");
    INFO("STARTING %s\n", argv[0]);

    // initialize p2p connection module,
    // this is used for the webcam connect proxy,
    // allow max of 100 connections
    if (p2p_init(128) < 0) {
        FATAL("p2p_init failed\n");
    }

    // call subsystem initialization routines
    account_init();
    dgram_init();

    // create listen socket, and
    // set socket option SO_REUSEADDR; and
    // bind socket; and
    // listen on socket
    listen_sockfd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (listen_sockfd == -1) {
        FATAL("socket listen_sockfd\n");
    }

    set_sock_opts(listen_sockfd, 1, -1, -1, -1);

    bzero(&addr,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htons(INADDR_ANY);
    addr.sin_port = htons(ADMIN_SERVER_PORT);
    ret = bind(listen_sockfd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1) {
        FATAL("bind listen_sockfd\n");
    }
    
    ret = listen(listen_sockfd, 50);
    if (ret == -1) {
        FATAL("listen\n");
    }

    // loop, accepting connection, and create thread to service the client
    while (true) {
        // accept connection
        addrlen = sizeof(addr);
        sockfd = accept4(listen_sockfd, (struct sockaddr *) &addr, &addrlen, SOCK_CLOEXEC);
        if (sockfd == -1) {
            ERROR("accept\n");
            continue;
        }
        DEBUG("accept from %s on port %d\n", 
              sock_addr_to_str(s,sizeof(s),(struct sockaddr*)&addr),
              ADMIN_SERVER_PORT);

        // create thread to service the connection
        pthread_create(&thread, NULL, service_thread, (void*)(long)sockfd);
    }

    // return
    INFO("TERMINATING %s\n", argv[0]);
    return 0;
}

void * service_thread(void * cx)
{
    #define VERIFY_USERNAME_AND_PASSWORD() \
        do { \
            if (!verify_user_name_and_password(user_name, password, &u)) { \
                REPLY_ERROR(STATUS_ERR_ACCESS_DENIED); \
                goto done; \
            } \
        } while (0)

    #define REPLY_ERROR(stat) \
        do { \
            char s[100]; \
            sprintf(s,"status=%d", (stat)); \
            write(sockfd,s,strlen(s)); \
        } while (0)

    #define REPLY_LOGIN_OK() \
        do { \
            int login_okay = ADMIN_SERVER_LOGIN_OK; \
            write(sockfd,&login_okay,sizeof(login_okay)); \
        } while (0)

    int      sockfd = (long)cx;
    char     http_connect_req[sizeof(HTTP_CONNECT_REQ)];
    char     login[96];
    char   * user_name;
    char   * password;
    char   * service;
    user_t * u;
    int      len;

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // read and validate http connect request;
    // with 5 sec tout
    bzero(http_connect_req, sizeof(http_connect_req));
    set_sock_opts(sockfd, -1, -1, -1, 5000000);
    len = recv(sockfd, http_connect_req, sizeof(http_connect_req)-1, MSG_WAITALL);
    set_sock_opts(sockfd, -1, -1, -1, 0);
    if (len != sizeof(http_connect_req)-1) {
        ERROR("reading http connect request, len=%d, %s\n", len, strerror(errno));
        goto done;
    }
    if (strcmp(http_connect_req, HTTP_CONNECT_REQ) != 0) {
        ERROR("invalid http connect request, '%s'\n", http_connect_req);
        goto done;
    }

    // send http connect response
    write(sockfd, HTTP_CONNECT_RESP, sizeof(HTTP_CONNECT_RESP)-1);

    // read 96 bytes which contain the user_name,password,service;
    // with 5 sec tout
    set_sock_opts(sockfd, -1, -1, -1, 5000000);
    len = recv(sockfd, login, sizeof(login), MSG_WAITALL);
    set_sock_opts(sockfd, -1, -1, -1, 0);
    if (len != sizeof(login)) {
        ERROR("read of login info\n");
        goto done;
    }
    login[sizeof(login)-1] = '\0';
    user_name = login;
    password  = login+32;
    service   = login+64;

    // process the service request
    if (strcmp(service, "create") == 0) {
        int  status;
        char s[100];

        status = account_create(user_name, password);
        if (status != STATUS_INFO_OK) {
            REPLY_ERROR(status);
            goto done;
        }
        REPLY_LOGIN_OK();
        sprintf(s,"account %s created\n", user_name);
        write(sockfd, s, strlen(s));

    } else if (strcmp(service, "login") == 0) {
        VERIFY_USERNAME_AND_PASSWORD();
        REPLY_LOGIN_OK();
        account_login(sockfd, u);
        sockfd = -1;  // closed by above call

    } else if (strcmp(service, "command") == 0) {
        VERIFY_USERNAME_AND_PASSWORD();
        REPLY_LOGIN_OK();
        account_command(sockfd, u);
        sockfd = -1;  // closed by above call

    } else if (strcmp(service, "nettest") == 0) {
        VERIFY_USERNAME_AND_PASSWORD();
        if (strcmp(user_name, "root") != 0) {
            REPLY_ERROR(STATUS_ERR_MUST_BE_ROOT);
            goto done;
        }
        REPLY_LOGIN_OK();
        nettest(sockfd);

    } else if (strncmp(service, "wccon", 5) == 0) {
        char wc_name[100];
        int  service_id, status, handle;

        VERIFY_USERNAME_AND_PASSWORD();
        if (sscanf(service, "wccon %s %d", wc_name, &service_id) != 2) {
            REPLY_ERROR(STATUS_ERR_INVALID_SERVICE);
            goto done;
        }
        handle = p2p_connect(user_name, password, wc_name, service_id, &status);
        if (handle < 0) {
            REPLY_ERROR(status);
            goto done;
        }
        REPLY_LOGIN_OK();
        wccon(sockfd, handle);

    } else {
        VERIFY_USERNAME_AND_PASSWORD();
        REPLY_ERROR(STATUS_ERR_INVALID_SERVICE);
        goto done;
    }

    // close sockat and exit thread
done:
    if (sockfd != -1) {
        close(sockfd);
        sockfd = -1;
    }
    return NULL;
}

// ----------------- SERVICE - ACCOUNT CREATE, LOGIN, AND COMMAND  ----------

void account_init(void)
{
    // init user[] by reading all files in the user dir
    read_all_user_files();

    // if root user doesn't exist then create it;
    // note - slot 0 is reserved for root user
    user_t * u = &user[0];
    if (strcmp(u->user_name, "root") != 0) {
        if (u->user_name[0] != '\0') {
            FATAL("root user slot in use by %s\n", u->user_name);
        }

        bzero(u,sizeof(user_t));
        strcpy(u->user_name, "root");
        strcpy(u->password, "su");
        gettimeofday(&u->time_created, NULL);
        if (!create_user_file(u)) {
            FATAL("create_user_file root\n");
        }
    }
}

int account_create(char * user_name, char * password)
{
    user_t * u;
    int      i, status;
    
    // verify user_name and password length and character set
    if (!verify_user_name(user_name, &status)) {
        return status;
    }
    if (!verify_password(password, &status)) {
        return status;
    }

    // if user_name already exists then error
    if (find_user(user_name)) {
        return STATUS_ERR_USERNAME_ALREADY_EXISTS;
    }

    // add entry in user table
    for (i = 0; i < MAX_USER; i++) {
        if (user[i].user_name[0] == '\0') {
            break;
        }
    }
    if (i == MAX_USER) {
        return STATUS_ERR_TOO_MANY_USERS;        
    }
    u = &user[i];
    bzero(u,sizeof(user_t));
    strcpy(u->user_name, user_name);
    strcpy(u->password, password);
    gettimeofday(&u->time_created, NULL);

    // may need to increase max_user to accomodate the new entry
    if (i >= max_user) {
        max_user = i + 1;
    }
    
    // create user file
    if (!create_user_file(u)) {
        return STATUS_ERR_CREATE_USER_PROFILE;
    }

    // return success
    return STATUS_INFO_OK;
}

void account_login(int sockfd, user_t * u)
{
    FILE  * fp;

    // associate fp with sockfd
    fp = fdopen(sockfd, "w+");   // mode: read/write
    if (fp == NULL) {
        ERROR("fdopen\n");
        close(sockfd);
        return;
    }
    setlinebuf(fp);

    // set thread local storage flag to indicate this thread is privleged
    is_root = IS_ROOT(u);

    // call cmd_processor
    cmd_processor(u, fp, false);

    // close stream, this also closes sockfd
    fclose(fp);
}

void account_command(int sockfd, user_t * u)
{
    FILE * fp;

    // associate fp with sockfd
    fp = fdopen(sockfd, "w+");   // mode: read/write
    if (fp == NULL) {
        ERROR("fdopen\n");
        close(sockfd);
        return;
    }
    setlinebuf(fp);

    // set thread local storage flag to indicate this thread is privleged
    is_root = IS_ROOT(u);

    // call cmd_processor
    cmd_processor(u, fp, true);

    // close stream, this also closes sockfd
    fclose(fp);
}

typedef struct {
    char * name;
    bool (*proc)(user_t * u, FILE * fp, int argc, char ** argv);
    int    min_expected_argc;
    int    max_expected_argc;
    bool   root_cmd;
    char * description;
} cmd_tbl_t;

cmd_tbl_t cmd_tbl[] = {
    // the following cmds are for all users
    { "help", cmd_help, 0, 0, false,
      "help - display description of commands" },
    { "add_wc", cmd_add_wc, 2, 3, false,
      "add_wc <wc_name> <wc_macaddr|wc_link> [<access_list>] - add webcam" },
    { "ren_wc", cmd_ren_wc, 2, 2, false,
      "ren_wc <wc_name> <new_wc_name> - rename webcam" },
    { "del_wc", cmd_del_wc, 1, 1, false,
      "del_wc <wc_name> - delete webcam" },
    { "ls", cmd_ls, 0, 2, false,
      "ls [-v] [everyone|unclaimed|<user_name>] - list webcams" },
    { "password", cmd_password, 2, 2, false,
      "password <curr_passwd> <new_passwd> - change password" },
    { "delete_account", cmd_delete_account, 0, 0, false,
      "delete_account - deletes the current user account and logout" },
    { "version", cmd_version, 0, 0, false,
      "version - display program version" },
    { "exit", cmd_exit, 0, 0, false,
      "exit - logout" },

    // the following cmds are root only
    { "su", cmd_su, 1, 1, true,
      "su <user> - switch user" },
    { "set_password", cmd_set_password, 1, 1, true,
      "set_password <new_password> - set password" },
            };

#define MAX_CMD_TBL (sizeof(cmd_tbl) / sizeof(cmd_tbl[0]))

void cmd_processor(user_t * u, FILE * fp, bool single_cmd)
{
    char     b[MAX_GETCL_BUFF];
    char   * argv[MAX_GETCL_ARGV];
    int      argc, i;
    bool     logout;

    // loop, read and process cmd
    while (true) {
        // issue prompt 
        if (!single_cmd) {
            prcl(fp, "%s%s> ", is_root ? "(root) " : "", u->user_name);
        }

        // get client command
        if (getcl(fp,b,&argc,argv) == false) {
            break;
        }

        // if emtpy line then we're done if processing single_cmd, else continue
        if (argc == 0) {
            if (single_cmd) {
                break;
            } else {
                continue;
            }
        }

        // search cmd_tbl for matching cmd
        for (i = 0; i < MAX_CMD_TBL; i++) {
            if ((strcmp(cmd_tbl[i].name, argv[0]) == 0) &&
                (is_root || !cmd_tbl[i].root_cmd))
            {
                break;
            }
        }

        // matching cmd not found
        if (i == MAX_CMD_TBL) {
            prcl(fp, "error: invalid command\n");
            continue;
        }

        // verify number of args
        if (argc-1 < cmd_tbl[i].min_expected_argc ||
            argc-1 > cmd_tbl[i].max_expected_argc)
        {
            prcl(fp, "error: invalid number of args\n");
            continue;
        }

        // process the cmd
        logout = cmd_tbl[i].proc(u, fp, argc-1, argv+1);

        // if logout requested or processing a single_cmd then break
        if (single_cmd || logout) {
            break;
        }
    }
}

// help - display description of commands
bool cmd_help(user_t * u, FILE * fp, int argc, char ** argv) 
{
    int i;

    // display help 
    for (i = 0; i < MAX_CMD_TBL; i++) {
        if (is_root || !cmd_tbl[i].root_cmd) {
            prcl(fp, "%s\n", cmd_tbl[i].description);
        }
    }

    // return, no-logout
    return false;
}

// add_wc <wc_name> <wc_macaddr|wc_link> [<access_list>] - add webcam for this user
bool cmd_add_wc(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char      * wc_name        = argv[0];
    char      * wc_access_list = (argc > 2 ? argv[2] : NULL);
    user_wc_t * wc;
    int         i, status;
    char        link_user_name[MAX_USER_NAME+1];
    char        link_wc_name[MAX_WC_NAME+1];

    // argv[0] is the name of the webcam being added
    // argv[1] is either the webcams macaddr or link to another user's webcam
    //         - macaddr : "11:22:33:44:55:66"  (this user owns the webcam)
    //         - link    : 'joe.garage'         (Joe owns this webcam)
    // argv[2] is optional list of other users who can access, or 'everyone';
    //         this is only valid when this user is the webcam's owner
    //
    // examples:
    //   add_wc my_garage 11:22:33:44:55:66 joe
    //   add_wc joes_garage joe.garage
    // 

    // verify the name of webcam being added
    if (verify_wc_name(wc_name, &status) == false) {
        prcl(fp, "error: invalid wc_name, %s\n", status2str(status));
        return false;
    }

    // verify this name is not already being used by this user
    if (find_user_wc_from_user_and_name(u,wc_name)) {
        prcl(fp, "error: wc_name '%s' is already being used\n", wc_name);
        return false;
    }

    // find a free slot in this user wc tbl
    for (i = 0; i < MAX_USER_WC; i++) {
        if (u->wc[i].wc_name[0] == '\0') {
            break;
        }
    }
    if (i == MAX_USER_WC) {
        prcl(fp, "error: no space to add webcam\n");
        return false;
    }
    wc = &u->wc[i];

    // check if wc_macaddr is supplied
    if (verify_wc_macaddr(argv[1])) {
        char   * wc_macaddr = argv[1];
        user_t * u_owner;

        // convert macaddr to lowercase
        for (i = 0; wc_macaddr[i] != '\0'; i++) {
            if (wc_macaddr[i] >= 'A' && wc_macaddr[i] <= 'F') {
                wc_macaddr[i] += ('a' - 'A');
            }
        }

        // verify wc_macaddr is not in use by any user
        if (find_user_wc_from_macaddr(wc_macaddr, &u_owner)) {
            prcl(fp, "error: wc_macaddr %s already owned by user %s\n", 
                 wc_macaddr, u_owner->user_name);
            return false;
        }

        // if access list is supplied then verify format is 
        // a comma seperated list of usernames
        if (wc_access_list) {
            if (verify_wc_access_list(wc_access_list, &status) == false) {
                prcl(fp, "error: invalid access_list '%s', %s\n", 
                     wc_access_list, status2str(status));
                return false;
            }
        }

        // add the webcam to this users wc table, and
        // store change in file
        bzero(wc, sizeof(user_wc_t));
        strncpy(wc->wc_name, wc_name, MAX_WC_NAME);
        wc->is_owner = true;
        strncpy(wc->u.owner.wc_macaddr, wc_macaddr, MAX_WC_MACADDR);
        strncpy(wc->u.owner.wc_access_list, wc_access_list ? wc_access_list : "", MAX_WC_ACCESS_LIST);
        update_user_file(u);

    // check if wc_link is supplied
    } else if (verify_wc_link(argv[1], link_user_name, link_wc_name)) {
        // error if wc_access_list is supplied
        if (wc_access_list) {
            prcl(fp, "error: access_list not allowed\n");
            return false;
        }

        // add the webcam to this users wc table, and
        // store change in file
        bzero(wc, sizeof(user_wc_t));
        strncpy(wc->wc_name, wc_name, MAX_WC_NAME);
        wc->is_owner = false;
        strncpy(wc->u.link.user_name, link_user_name, MAX_USER_NAME);
        strncpy(wc->u.link.wc_name, link_wc_name, MAX_WC_NAME);
        update_user_file(u);

    // argv[1] is not a macaddr or link
    } else {
        prcl(fp, "error: invalid macaddr or link\n");
    }

    // return, no-logout
    return false;
}

// ren_wc <wc_name> <new_wc_name> - rename webcam 
bool cmd_ren_wc(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char      * wc_name = argv[0];
    char      * new_wc_name = argv[1];
    user_wc_t * wc;
    int         status;

    // verify new_wc_name, and
    // verify new_wc_name is not already in use
    if (verify_wc_name(new_wc_name, &status) == false) {
        prcl(fp, "error: invalid new name '%s', %s\n", new_wc_name, status2str(status));
        return false;
    }
    if (find_user_wc_from_user_and_name(u,new_wc_name)) {
        prcl(fp, "error: new_wc_name '%s' is already in use\n", new_wc_name);
        return false;
    }

    // find wc_name in this user wc tbl
    wc = find_user_wc_from_user_and_name(u,wc_name);
    if (wc == NULL) {
        prcl(fp, "error: wc_name does not exist '%s'\n", wc_name);
        return false;
    }

    // replace with new name
    strcpy(wc->wc_name, new_wc_name);

    // store change in file
    update_user_file(u);

    // return, no-logout
    return false;
}

// del_wc <wc_name> - delete webcam 
bool cmd_del_wc(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char      * wc_name = argv[0];
    user_wc_t * wc;

    // find wc_name in this user wc tbl
    wc = find_user_wc_from_user_and_name(u,wc_name);
    if (wc == NULL) {
        prcl(fp, "error: does not exist '%s'\n", wc_name);
        return false;
    }

    // delete 
    bzero(wc, sizeof(user_wc_t));

    // store change in file
    update_user_file(u);

    // return, no-logout
    return false;
}

// ls [everyone|unclaimed|<user_name>] - list webcams
bool cmd_ls(user_t * u, FILE * fp, int argc, char ** argv) 
{
    int i;
    bool verbose;

    // check for verbose option
    verbose = (argc >= 1 && strcmp(argv[0], "-v") == 0);
    if (verbose) {
        if (--argc > 0) {
            argv[0] = argv[1];
        }
    }

    // validate argc
    if (argc > 1) {
        prcl(fp, "error: invalid number of args\n");
        return false;
    }

    // if argc is zero then 
    //   display my account
    // else if argv[0] = "everyone" then 
    //   display all user names   
    // else if argv[0] = 'unclaimed' then
    //   display webcams that are online and not owned
    // else
    //   otherwise display specified user
    // endif

    if (argc == 0) {
        display_user(fp,u,verbose);
    } else if (strcmp(argv[0], "everyone") == 0) {
        for (i = 0; i < max_user; i++) {
            if (user[i].user_name[0] == '\0' || IS_ROOT(&user[i])) {
                continue;
            }
            prcl(fp, "USER: %s\n", user[i].user_name);
            display_user(fp,&user[i],verbose);
            prcl(fp, "\n");
        }
    } else if (strcmp(argv[0], "unclaimed") == 0) {
        display_unclaimed(fp, verbose);
    } else {
        user_t * u = find_user(argv[0]);
        if (u == NULL) {
            prcl(fp, "error: user %s does not exist\n", argv[0]);
        }
        display_user(fp, u, verbose);
    }

    // return, no-logout
    return false;
}

// password <curr_passwd> <new_passwd> - change password
bool cmd_password(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char * curr_passwd = argv[0];
    char * new_passwd = argv[1];
    int    status;

    // verify curr_password
    if (strcmp(u->password, curr_passwd) != 0) {
        prcl(fp, "error: current_password invalid\n");
        return false;
    }

    // validate new_password
    if (!verify_password(new_passwd, &status)) {
        prcl(fp, "error: new_password invalid, %s\n", status2str(status));
        return false;
    }

    // set new_password and write user file
    strcpy(u->password, new_passwd);
    update_user_file(u);

    // return no-logout
    prcl(fp, "password has been changed\n");
    return false;
}

// delete_account - deletes the current user account and logout
bool cmd_delete_account(user_t * u, FILE * fp, int argc, char ** argv) 
{
    // don't allow deleting of root
    if (IS_ROOT(u)) {
        prcl(fp, "eror: can not delete root user\n");
        return false;
    }

    // delete the account
    delete_user_file(u);
    bzero(u,sizeof(user_t));

    // return, logout
    prcl(fp, "account has been deleted, logging out\n");
    return true;
}

// version - display program version
bool cmd_version(user_t * u, FILE * fp, int argc, char ** argv) 
{
    prcl(fp, "version %d.%d\n", VERSION_MAJOR, VERSION_MINOR);
    return false;
}

// exit - logout
bool cmd_exit(user_t * u, FILE * fp, int argc, char ** argv) 
{
    return true;
}

// -- the following commands can be executed only by root --

// su <user> - switch user
bool cmd_su(user_t * u, FILE * fp, int argc, char ** argv)
{
    char * user_name = argv[0];
    user_t * new_u;

    // find the user
    new_u = find_user(user_name);
    if (new_u == NULL) {
        prcl(fp, "error: user does not exist\n");
        return false;
    }

    // recursively call cmd_processor, passing in the new user struct
    cmd_processor(new_u, fp, false);

    // return, no-logout
    return false;
}

// set_password <new_password> - set password
bool cmd_set_password(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char * new_passwd = argv[0];
    int    status;

    // validate new_password
    if (!verify_password(new_passwd, &status)) {
        prcl(fp, "error: new_password invalid, %s\n", status2str(status));
        return false;
    }

    // set new_password and write user file
    strcpy(u->password, new_passwd);
    update_user_file(u);

    // return no-logout
    prcl(fp, "password has been changed for user %s\n", u->user_name);
    return false;
}

// - - - - - - - - -  acct mgmt - utils  - - - - - - - - - - - - - 

void read_all_user_files(void)
{
    int fd = -1;
    struct dirent * dirent;
    struct stat buf;
    char pathname[PATH_MAX];
    char * filename;
    DIR * dir;
    int len, idx=1;
    user_t u;

    // open directory containing user files
    dir = opendir(USER_DIR);
    if (dir == NULL) {
        FATAL("opendir USER_DIR\n");
    }

    while (true) {
        // if fd is not closed, then close it now
        if (fd != -1) {
            close(fd);
            fd = -1;
        }

        // get next user filename
        dirent = readdir(dir);
        if (dirent == NULL) {
            break;
        }
        filename = dirent->d_name;

        // skip files that include ".old"
        if (strstr(filename, ".old") != NULL) {
            continue;
        }

        // open user file
        sprintf(pathname, USER_DIR"/%s", filename);
        fd = open(pathname, O_RDONLY|O_CLOEXEC);
        if (fd == -1) {
            ERROR("open  %s\n", pathname);
            continue;
        }

        // stat user file
        if (fstat(fd, &buf) == -1) {
            ERROR("fstat %s\n", pathname);
            continue;
        }

        // skip if not a file, such as directories "." and ".."
        if (!S_ISREG(buf.st_mode)) {
            continue;
        }

        // verify user file size
        if (buf.st_size != sizeof(user_t)) {
            ERROR("size %s, act=%d exp=%d\n", 
                  pathname, (int)buf.st_size, (int)sizeof(user_t));
            continue;
        }

        // read user file
        len = read(fd, &u, sizeof(user_t));
        if (len != sizeof(user_t)) {
            ERROR("read %s, act=%d exp=%d\n", pathname, len, (int)sizeof(user_t));
            continue;
        }

        // if u is root then 
        //   save date in user tbl entry 0
        // else 
        //   save date in idx
        // endif
        if (strcmp(u.user_name, "root") == 0) {
            user[0] = u;
        } else {
            if (idx >= MAX_USER) {
                ERROR("too man users, can't add %s\n", pathname);
                continue;
            }
            user[idx++] = u;
        }
    }

    // close user file dir
    closedir(dir);

    // set max_user
    max_user = idx;
}

bool create_user_file(user_t * u)
{
    char file_name[NAME_MAX+1];
    int fd, len;

    sprintf(file_name, USER_DIR"/%s", u->user_name);

    // write the new file
    fd = open(file_name, O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC, 0600);
    if (fd == -1) {
        ERROR("create %s\n", file_name);
        return false;
    }
    len = write(fd, u, sizeof(user_t));
    if (len != sizeof(user_t)) {
        ERROR("write %s, len=%d exp=%d\n", file_name, len, (int)sizeof(user_t));
        close(fd);
        return false;
    }
    close(fd);

    // return success
    return true;
}

bool update_user_file(user_t * u)
{
    char file_name[NAME_MAX+1];
    char file_name_old[NAME_MAX+1];
    int fd=-1, len;

    sprintf(file_name, USER_DIR"/%s", u->user_name);
    sprintf(file_name_old, USER_DIR"/%s.old", u->user_name);

    // rename the file to  file.old
    if (rename(file_name, file_name_old) == -1) {
        ERROR("rename %s to %s\n", file_name, file_name_old);
        return false;
    }

    // write the new file
    fd = open(file_name, O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC, 0600);
    if (fd == -1) {
        ERROR("create %s\n", file_name);
        goto error;
    }
    len = write(fd, u, sizeof(user_t));
    if (len != sizeof(user_t)) {
        ERROR("write %s, len=%d exp=%d\n", file_name, len, (int)sizeof(user_t));
        goto error;
    }
    close(fd);
    fd = -1;

    // return success
    return true;

error:
    // return error 
    if (fd != -1) {
        close(fd);
        fd = -1;
    }
    char cmd[1000];
    sprintf(cmd, "cp %s %s ", file_name_old, file_name);
    system(cmd);
    return false;
}

bool delete_user_file(user_t * u) 
{
    char file_name[NAME_MAX+1];
    char file_name_old[NAME_MAX+1];
    int ret;

    sprintf(file_name, USER_DIR"/%s", u->user_name);
    sprintf(file_name_old, USER_DIR"/%s.old", u->user_name);

    unlink(file_name_old);
    ret = unlink(file_name);

    return ret == 0;
}

bool getcl(FILE * fp, char * b, int * argc, char ** argv)
{
    char * saveptr;
    char * token;

    *argc = 0;

    if (fgets(b, MAX_GETCL_BUFF, fp) == NULL) {
        return false;
    }

    while (true) {
        token = strtok_r(*argc==0?b:NULL, " \n", &saveptr);
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

void prcl(FILE * fp, char * fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
}
                            
void display_user(FILE * fp, user_t * u, bool verbose)
{
    int i;

    for (i = 0; i < MAX_USER_WC; i++) {
        if (u->wc[i].wc_name[0] == '\0') {
            continue;
        }
        display_user_wc(fp, u, i, verbose);
    }
}

void display_user_wc(FILE * fp, user_t * u, int u_wc_idx, bool verbose)
{
    user_wc_t * caller_wc;
    user_wc_t * owner_wc;
    char      * state;
    char        verb_str[1000];
    char        online_wc_addr_str[100];
    char        online_wc_addr_behind_nat_str[100];
    int         i;

    // init locals
    caller_wc   = &u->wc[u_wc_idx];
    state       = "unknown";
    verb_str[0] = '\0';

    // if webcam is a link then 
    //   follow the link to determine owner_wc;
    //   if not found or access is not allowed then 
    //     set state_str accordingly
    //     set owner_wc to NULL
    //   endif
    // else
    //   set owner_wc
    // endif
    if (!caller_wc->is_owner) {
        owner_wc = find_wc_owner_from_wc_link(caller_wc);
        if (owner_wc == NULL) {
            state = "nodev";
        } else if (!verify_wc_access_permitted(owner_wc, u->user_name)) {
            state = "denied";
            owner_wc = NULL;
        } 
    } else {
        owner_wc = caller_wc;
    }

    // if owner_wc has been identified by the above code
    //   search the onl_wc table to see if wc_macaddr is online, and 
    //   set verb_str
    // endif
    if (owner_wc != NULL) {
        for (i = 0; i < max_onl_wc; i++) {
            if (onl_wc[i].wc_macaddr[0] != '\0' &&
                strcmp(owner_wc->u.owner.wc_macaddr, onl_wc[i].wc_macaddr) == 0) 
            {
                state = "online";
                sprintf(verb_str, "%d.%d %s %s",
                        onl_wc[i].version.major, onl_wc[i].version.minor,
                        sock_addr_to_str(online_wc_addr_str, sizeof(online_wc_addr_str), 
                                         (struct sockaddr *)&onl_wc[i].wc_addr),
                        sock_addr_to_str(online_wc_addr_behind_nat_str, sizeof(online_wc_addr_behind_nat_str), 
                                         (struct sockaddr *)&onl_wc[i].wc_addr_behind_nat));
                break;
            }
        }
        if (i == max_onl_wc) {
            state = "offline";
        }
    }

    // print - examples:
    // - computer     steve        online  80:1f:02:d3:9f:0c bill,bob,joe,sally   1.3 73.218.14.230:36657 192.168.1.121:36657
    // - joes_garage  steve        online  joe.garage                             1.3 73.218.14.230:36657 192.168.1.121:36657
    //   123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 
    char   str[1000];
    char * s = str;
    s += sprintf(s, "%-12s %-12s %-7s ", caller_wc->wc_name, u->user_name, state);
    if (caller_wc->is_owner) {
        s += sprintf(s, "%-17s %-20s ", caller_wc->u.owner.wc_macaddr, caller_wc->u.owner.wc_access_list);
    } else {
        s += sprintf(s, "%s.%s", caller_wc->u.link.user_name, caller_wc->u.link.wc_name);
    }
    if (verbose) {
        if (strlen(str) < 74) {
            s += sprintf(s, "%*s", (int)(74-strlen(str)), "");
        }
        s += sprintf(s, "%s", verb_str);
    }
    prcl(fp, "%s\n", str);
}

void display_unclaimed(FILE * fp, bool verbose)
{
    int      i;
    user_t * u_owner;

    for (i = 0; i < max_onl_wc; i++) {
        if (find_user_wc_from_macaddr(onl_wc[i].wc_macaddr, &u_owner) == NULL) {
            display_unclaimed_wc(fp, i, verbose);
        }
    }
}

void display_unclaimed_wc(FILE * fp, int onl_wc_idx, bool verbose)
{
    char verb_str[1000];
    char online_wc_addr_str[100];
    char online_wc_addr_behind_nat_str[100];

    // print - examples:
    // - ---          ---          online  80:1f:02:d3:9f:0c                      1.3 73.218.14.230:36657 192.168.1.121:36657

    verb_str[0] = '\0';
    if (verbose) {
        sprintf(verb_str, "%d.%d %s %s",
                onl_wc[onl_wc_idx].version.major, onl_wc[onl_wc_idx].version.minor,
                sock_addr_to_str(online_wc_addr_str, sizeof(online_wc_addr_str), 
                                (struct sockaddr *)&onl_wc[onl_wc_idx].wc_addr),
                sock_addr_to_str(online_wc_addr_behind_nat_str, sizeof(online_wc_addr_behind_nat_str), 
                                (struct sockaddr *)&onl_wc[onl_wc_idx].wc_addr_behind_nat));
    }

    prcl(fp, "%-12s %-12s %-7s %-17s %-20s %s\n", 
         "---",                           // wc_name
         "---",                           // owner's user_name
         "online",                        // state
         onl_wc[onl_wc_idx].wc_macaddr,   // macaddr
         "",                              // access_list
         verb_str);                       // verbose_str
}

// search user tbl for entry associated with user_name
user_t * find_user(char * user_name)
{
    int i;

    if (!user_name || user_name[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < max_user; i++) {
        if (strcmp(user[i].user_name, user_name) == 0) {
            return &user[i];
        }
    }

    return NULL;
}

// search u for wc_name
user_wc_t * find_user_wc_from_user_and_name(user_t * u, char * wc_name)
{
    int i;

    if (!u || !wc_name || wc_name[0] == '\0') {
        return NULL;
    }

    for (i = 0; i < MAX_USER_WC; i++) {
        if (strcmp(wc_name, u->wc[i].wc_name) == 0) {
            return &u->wc[i];
        }
    }

    return NULL;
}

// search all u for wc_macaddr
user_wc_t * find_user_wc_from_macaddr(char * wc_macaddr, user_t ** u)
{
    int      i, j;

    // preset u return value
    *u = NULL;
    
    // if wc_macaddr is empty str return NULL
    if (!wc_macaddr || wc_macaddr[0] == '\0') {
        return NULL;
    }

    // attempt to find the user that owns this wc
    for (i = 0; i < max_user; i++) {
        if (user[i].user_name[0] == '\0') {
            continue;
        }
        for (j = 0; j < MAX_USER_WC; j++) {
            user_wc_t * wc = &user[i].wc[j];
            if (wc->wc_name[0] != '\0' &&
                wc->is_owner &&
                strcmp(wc->u.owner.wc_macaddr, wc_macaddr) == 0) 
            {
                *u = &user[i];
                return &user[i].wc[j];
            }
        }
    }

    // not found
    return NULL;
}

// search onl_wc for wc_macaddr
onl_wc_t * find_onl_wc_from_macaddr(char * wc_macaddr)
{
    int i;

    // if wc_macaddr is empty str return NULL
    if (!wc_macaddr || wc_macaddr[0] == '\0') {
        return NULL;
    }

    // search onl_wc table for webcam with wc_macaddr
    for (i = 0; i < max_onl_wc; i++) {
        if (onl_wc[i].wc_macaddr[0] != '\0' &&
            strcmp(wc_macaddr, onl_wc[i].wc_macaddr) == 0) 
        {
            return &onl_wc[i];
        }
    }

    // not found
    return NULL;
}

// follow wc_link to obtain wc_owner
user_wc_t * find_wc_owner_from_wc_link(user_wc_t * wc_link)
{
    user_t    * u = NULL;
    user_wc_t * wc_owner = NULL;

    // verify wc_link is truly a link
    if (wc_link->is_owner) {
        ERROR("wc_link is not a link\n");
        return NULL;
    }

    // verify link user_name and link wc_name are not empty
    if (wc_link->u.link.user_name[0] == '\0' || wc_link->u.link.wc_name[0] == '\0') {
        ERROR("wc_link user_name or wc_name strings are empty\n");
        return NULL;
    }

    // locate user
    u = find_user(wc_link->u.link.user_name);
    if (u == NULL) {
        return NULL;
    }

    // locate webcam
    wc_owner = find_user_wc_from_user_and_name(u, wc_link->u.link.wc_name);
    if (wc_owner == NULL) {
        return NULL;
    }

    // return wc_owner
    return wc_owner;
}

// verify user_name exists and supplied password is correct
bool verify_user_name_and_password(char * user_name, char *password, user_t ** u) 
{
    user_t * u1;

    // verify user_name and password are not empty
    if (!user_name || user_name[0] == '\0' || !password || password[0] == '\0') {
        *u = NULL;
        return false;
    }

    // locate user
    u1 = find_user(user_name);
    if (u1 == NULL) {
        *u = NULL;
        return false;
    }

    // validate password
    if (strcmp(u1->password, password) != 0) {
        *u = NULL;
        return false;
    }

    // success
    *u = u1;
    return true;
}

// verify wc_macaddr string is correctly formatted macaddr
bool verify_wc_macaddr(char * wc_macaddr)
{
    char * s = wc_macaddr;
    int i;

    if (strlen(s) != 17) {
        return false;
    }

    for (i = 0; i < 17; i++) {
        if (i == 2 || i == 5 || i == 8 || i == 11 || i == 14) {
            if (s[i] != ':') {
                return false;
            }
        } else {
            if ((s[i] < '0' && s[i] > '9') &&
                (s[i] < 'a' && s[i] > 'f') &&
                (s[i] < 'A' && s[i] > 'F'))
            {
                return false;
            }
        }
    }

    return true;
}

// verify wc_link_str is correctly formatted link "<link_user_name>.<link_wc_name>";
// this does not verify the link_user_name and link_wc_name actually exist;
// the link_user_name and link_wc_name are returned if validation okay
bool verify_wc_link(char * wc_link_str, char * link_user_name, char * link_wc_name)
{
    char * user_name;
    char * wc_name;
    char * saveptr;
    int    status;

    // note: wc_link_str is passed to strtok, and is therefore modified

    user_name = strtok_r(wc_link_str, ".", &saveptr);
    wc_name  = strtok_r(NULL, "", &saveptr);

    if (!verify_user_name(user_name, &status) || !verify_wc_name(wc_name, &status)) {
        return false;
    }

    strcpy(link_user_name, user_name);
    strcpy(link_wc_name, wc_name);
    return true;
}

// verify wc_owner allows access for user_name
bool verify_wc_access_permitted(user_wc_t * wc_owner, char * user_name)
{
    char * acl_user_name;
    char * saveptr;
    char   s[MAX_WC_ACCESS_LIST+1];
    bool   first = true;

    if (wc_owner->is_owner == false) {
        ERROR("is_owner is false\n");
        return false;
    }

    strncpy(s, wc_owner->u.owner.wc_access_list, MAX_WC_ACCESS_LIST);
    s[MAX_WC_ACCESS_LIST] = '\0';

    while (true) {
        acl_user_name = strtok_r(first ? s : NULL, ",", &saveptr);
        first = false;
    
        if (acl_user_name == NULL) {
            break;
        }

        if (strcmp(acl_user_name, "everyone") == 0 ||
            strcmp(acl_user_name, user_name) == 0)
        {
            return true;
        }
    }

    return false;
}

// verify wc_access_list str is properly formatted,
// for example "job,bob,bill,sally"
bool verify_wc_access_list(char * wc_access_list, int * status)
{
    char s[MAX_WC_ACCESS_LIST+1];
    char * user_name;
    char * saveptr;
    bool   first = true;

    // wc_access_list  should be a comma seperated list, with 0 or more user_names,
    // 'everyone' grants all users access,

    if (strlen(wc_access_list) > MAX_WC_ACCESS_LIST) {
        *status = STATUS_ERR_WC_ACCESS_LIST_LENGTH;
        return false;
    }

    strcpy(s, wc_access_list);

    while (true) {
        user_name = strtok_r(first ? s : NULL, ",", &saveptr);
        first = false;

        if (user_name == NULL) {
            break;
        }

        if (!verify_user_name(user_name, status)) {
            return false;
        }
    }

    *status = STATUS_INFO_OK;
    return true;
}

// verify user_name string length is correct and contains valid chars
bool verify_user_name(char * user_name, int * status)
{
    int len = strlen(user_name);

    if (len < MIN_USER_NAME || len > MAX_USER_NAME) {
        *status = STATUS_ERR_USERNAME_LENGTH;
        return false;
    }

    if (!isalpha(user_name[0]) || !verify_chars(user_name)) {
        *status = STATUS_ERR_USERNAME_CHARS;
        return false;
    }

    *status = STATUS_INFO_OK;
    return true;
}

// verify password string length is correct and contains valid chars
bool verify_password(char * password, int * status)
{
    int len = strlen(password);

    if (len < MIN_PASSWORD || len > MAX_PASSWORD) {
        *status = STATUS_ERR_PASSWORD_LENGTH;
        return false;
    }

    if (!verify_chars(password)) {
        *status = STATUS_ERR_PASSWORD_CHARS;
        return false;
    }

    *status = STATUS_INFO_OK;
    return true;
}

// verify wc_name string length is correct and contains valid chars
bool verify_wc_name(char * wc_name, int * status)
{
    int len = strlen(wc_name);

    if (len == 0 || len > MAX_WC_NAME) {
        *status = STATUS_ERR_WCNAME_LENGTH;
        return false;
    }

    if (!isalpha(wc_name[0]) || !verify_chars(wc_name)) {
        *status = STATUS_ERR_WCNAME_CHARS;
        return false;
    }

    *status = STATUS_INFO_OK;
    return true;
}

// verify string has only allowed chars
bool verify_chars(char * s) 
{
    static char accept[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789"
        "_";

    return strspn(s,accept) == strlen(s);
}

// ----------------- SERVICE - NETWORDK SPEED TEST  ----------------

void nettest(int sockfd) 
{
    char  buff[ADMIN_SERVER_MAX_NETTEST_BUFF];
    int   len;

    // loop, reading and writing back a buffer, the client decides when to stop
    while (true) {
        len = recv(sockfd, buff, sizeof(buff), MSG_WAITALL);
        if (len == 0) {
            INFO("nettest complete\n");
            goto done;
        }
        if (len != sizeof(buff)) {
            ERROR("nettest abort, read len=%d expected=%d\n", len, (int)sizeof(buff));
            goto done;
        }

        len = write(sockfd, buff, sizeof(buff));
        if (len != sizeof(buff)) {
            ERROR("nettest abort, write len=%d expected=%d\n", len, (int)sizeof(buff));
            goto done;
        }
    }

done:
    return;
}

// ----------------- SERVICE - WCCON CONNECT  ---------------------

typedef struct {
    int sockfd;
    int handle;
} wccon_cx_t;

void wccon(int sockfd, int handle)  
{
    int len, rc;
    char buff[50000];
    pthread_t thread;
    wccon_cx_t cx = {sockfd, handle};

    // create thread to read from the webcam and write to the socket
    pthread_create(&thread, NULL, wccon_thread, &cx);

    // read from sockfd and write to webcam
    while (true) {
        if ((len = read(sockfd, buff, sizeof(buff))) <= 0) {
            break;
        }
        if ((rc = p2p_send(handle, buff, len)) != len) {
            break;
        }
    }

    // disconnect from webcam, 
    // the sockfd is closed in wccon_thread
    p2p_disconnect(handle);

    // waith for wccon_thread to exit
    pthread_join(thread, NULL);
}

void * wccon_thread(void *cxarg)
{
    wccon_cx_t * cx = cxarg;
    char buff[50000];
    int len, rc;

    // read from webcam and write to sockfd
    while (true) {
        if ((len = p2p_recv(cx->handle, buff, sizeof(buff), RECV_WAIT_ANY)) <= 0) {
            break;
        }
        if ((rc = write(cx->sockfd, buff, len)) != len) {
            break;
        }
    }

    // shutdown the socket
    shutdown(cx->sockfd, SHUT_RDWR);

    // exit thread
    return NULL;
}

// ----------------- DATAGRAM SUPPORT  -------------------

void dgram_init(void)
{
    pthread_t thread;

    // create dgram_thread - to receive announce and connect datagrams, 
    // and establish the p2p connection
    pthread_create(&thread, NULL, dgram_thread, NULL);

    // create dgram_monitor_onl_wc_thread - to monitor the onl_wc table
    // for webcams from which we have not received an announce in a long time;
    // remove these webcams from onl_wc
    pthread_create(&thread, NULL, dgram_monitor_onl_wc_thread, NULL);
}

void * dgram_thread(void * cx)
{
    int                sfd; 
    struct sockaddr_in local_addr;
    int                ret;

    // detach because this thread will not be joined
    pthread_detach(pthread_self());

    // create socket, and bind
    sfd = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, 0);
    if (sfd == -1) {
        FATAL("socket sfd");
    }
    bzero(&local_addr,sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = htons(INADDR_ANY);
    local_addr.sin_port = htons(ADMIN_SERVER_DGRAM_PORT);
    ret = bind(sfd,
               (struct sockaddr *)&local_addr,
               sizeof(local_addr));
    if (ret == -1) {
        FATAL("bind sfd");
    }

    while (true) {
        struct    sockaddr_in from_addr;
        socklen_t from_addrlen;
        int       len;
        dgram_t   dgram_rcv;

        // receive datagram
        from_addrlen = sizeof(from_addr);
        len = recvfrom(sfd, &dgram_rcv, sizeof(dgram_t) , 0, 
                       (struct sockaddr*)&from_addr, &from_addrlen);
        if (len < 0) {
            ERROR("recvfrom");
            continue;
        }
        if (from_addrlen != sizeof(struct sockaddr_in)) {
            ERROR("recvfrom addrlen act=%d  exp=%d\n",
                  from_addrlen, (int)sizeof(struct sockaddr_in));
            continue;
        }

#ifdef DEBUG_PRINTS
        // debug print the reived dgram
        char  s[100];
        DEBUG("recv dgram %s from %s\n", 
              DGRAM_ID_STR(dgram_rcv.id),
              sock_addr_to_str(s,sizeof(s),(struct sockaddr*)&from_addr));
#endif

        // if dgram is DGRAM_ID_WC_ANNOUNCE (from webcam) then
        if (dgram_rcv.id == DGRAM_ID_WC_ANNOUNCE) do {
            int idx=-1, i;
            char s1[100], s2[100];
            struct sockaddr_in * wc_addr = &from_addr;
            bool just_gone_online = false;

            // verify wc_macaddr
            if (verify_wc_macaddr(dgram_rcv.u.wc_announce.wc_macaddr) == false) {
                ERROR("not valid wc_macaddr %s\n", dgram_rcv.u.wc_announce.wc_macaddr);
                break;
            }

            // search for the announce.wc_macaddr in the onl_wc tbl,
            for (i = 0; i < max_onl_wc; i++) {
                if (strcmp(dgram_rcv.u.wc_announce.wc_macaddr, onl_wc[i].wc_macaddr) == 0) {
                    idx = i;
                    break;
                }
            }

            // if not found in the onl_wc tbl then pick an empty slot
            if (idx == -1) {
                for (i = 0; i < MAX_ONL_WC; i++) {
                    if (onl_wc[i].wc_macaddr[0] == '\0') {
                        idx = i;
                        break;
                    }
                }

                if (idx == -1) {
                    ERROR("onl_wc tbl is full\n");
                    break;
                }

                if (idx >= max_onl_wc) {
                    max_onl_wc = idx + 1;
                }

                just_gone_online = true;
            }

            // construct a new entry for onl_wc;
            // note - at this point the last_announce_rcv_time_us field is left unchanged
            //        to support the memcmp below; the last_announce_rcv_time_us field is
            //        subsequently updated to the current time
            onl_wc_t x;
            bzero(&x, sizeof(x));
            x.version = dgram_rcv.u.wc_announce.version;
            strcpy(x.wc_macaddr, dgram_rcv.u.wc_announce.wc_macaddr);
            x.wc_addr = *wc_addr;
            x.wc_addr_behind_nat = dgram_rcv.u.wc_announce.wc_addr_behind_nat;
            x.last_announce_rcv_time_us = onl_wc[idx].last_announce_rcv_time_us; 

            // if the new info is different from what was there before then 
            // - issue notice 
            // - publish new info
            // endif
            if (memcmp(&x, &onl_wc[idx], sizeof(onl_wc_t)) != 0) {
                // issue notice
                INFO("wc %s v%d.%d %s, %s/%s\n",
                     x.wc_macaddr,
                     x.version.major, x.version.minor,
                     just_gone_online ? "is now online" : "update",
                     sock_addr_to_str(s1,sizeof(s1),(struct sockaddr*)&x.wc_addr),
                     sock_addr_to_str(s2,sizeof(s2),(struct sockaddr*)&x.wc_addr_behind_nat));

                // publish new entry
                onl_wc[idx] = x;
            }

            // update the last_announce_rcv_time_us to the current time
            onl_wc[idx].last_announce_rcv_time_us = microsec_timer();
        } while (0);

        // if dgram is CONNECT_REQ (from a client) then
        if (dgram_rcv.id == DGRAM_ID_CONNECT_REQ) do {
            int                  i;
            dgram_t              dgram_snd;
            struct sockaddr_in * wc_addr;
            struct sockaddr_in * wc_addr_behind_nat;
            struct sockaddr_in * client_addr = &from_addr;
            user_t             * u;
            user_wc_t          * wc;
            onl_wc_t           * onlwc;
            char                 s1[100], s2[100];
            int                  status = STATUS_ERR_GENERAL_FAILURE;

            // debug print the connect request
            INFO("recvd connect request from %s/%s user=%s wc_name=%s service=%s\n",
                   sock_addr_to_str(s1,sizeof(s1),(struct sockaddr*)client_addr),
                   sock_addr_to_str(s2,sizeof(s2),(struct sockaddr*)&dgram_rcv.u.connect_req.client_addr_behind_nat),
                   dgram_rcv.u.connect_req.user_name,
                   dgram_rcv.u.connect_req.wc_name,
                   SERVICE_STR(dgram_rcv.u.connect_req.service));

            // verify supplied user/password
            if (!verify_user_name_and_password(dgram_rcv.u.connect_req.user_name, dgram_rcv.u.connect_req.password, &u)) {
                ERROR("invalid user_name or password\n");
                status = STATUS_ERR_ACCESS_DENIED;
                goto connect_reject;
            }

            // find the wc_name in the user wc table
            wc = find_user_wc_from_user_and_name(u, dgram_rcv.u.connect_req.wc_name);
            if (wc == NULL) {
                ERROR("wc '%s' does not exist for user '%s'\n", dgram_rcv.u.connect_req.wc_name, u->user_name);
                status = STATUS_ERR_WC_DOES_NOT_EXIST;
                goto connect_reject;
            }

            // if this user owns the webcam then
            //   find wc_macaddr in the onl_wc table
            // else
            //   find wc owner by following the link
            //   check access list
            //   find wc_macaddr in the onl_wc table
            // endif
            if (wc->is_owner) {
                onlwc = find_onl_wc_from_macaddr(wc->u.owner.wc_macaddr);
                if (onlwc == NULL) {
                    ERROR("wc '%s' webcam not online\n", dgram_rcv.u.connect_req.wc_name);
                    status = STATUS_ERR_WC_NOT_ONLINE;
                    goto connect_reject;
                }
            } else {
                user_wc_t * wc_owner = find_wc_owner_from_wc_link(wc);
                if (wc_owner == NULL) {
                    ERROR("wc '%s' link invalid\n", dgram_rcv.u.connect_req.wc_name);
                    status = STATUS_ERR_INVALID_LINK;
                    goto connect_reject;
                }

                if (!verify_wc_access_permitted(wc_owner, u->user_name)) {
                    ERROR("wc '%s' link permission denied\n", dgram_rcv.u.connect_req.wc_name);
                    status = STATUS_ERR_ACCESS_DENIED;
                    goto connect_reject;
                }

                onlwc = find_onl_wc_from_macaddr(wc_owner->u.owner.wc_macaddr);
                if (onl_wc == NULL) {
                    ERROR("wc '%s' link not online\n", dgram_rcv.u.connect_req.wc_name);
                    status = STATUS_ERR_WC_NOT_ONLINE;
                    goto connect_reject;
                }
            }

            // onlwc has been set to the webcam that is to be connected to the client

            // verify the onlwc's wc_addr and wc_addr_behind_nat
            wc_addr = &onlwc->wc_addr;
            wc_addr_behind_nat = &onlwc->wc_addr_behind_nat;
            if (wc_addr->sin_family == 0 || wc_addr_behind_nat->sin_family == 0) {
                ERROR("wc '%s' address is not currently available\n", dgram_rcv.u.connect_req.wc_name);
                status = STATUS_ERR_WC_ADDR_NOT_AVAIL;
                goto connect_reject;
            }

            // construct the connect activate dgram
            bzero(&dgram_snd, sizeof(dgram_snd));
            dgram_snd.id = DGRAM_ID_CONNECT_ACTIVATE;
            dgram_snd.u.connect_activate.con_id  = gen_con_id();
            dgram_snd.u.connect_activate.service = dgram_rcv.u.connect_req.service;
            strncpy(dgram_snd.u.connect_activate.user_name, 
                    dgram_rcv.u.connect_req.user_name, 
                    MAX_USER_NAME);
            dgram_snd.u.connect_activate.user_name[MAX_USER_NAME] = '\0';
            if (client_addr->sin_addr.s_addr == wc_addr->sin_addr.s_addr) {
                dgram_snd.u.connect_activate.client_addr = dgram_rcv.u.connect_req.client_addr_behind_nat;
                dgram_snd.u.connect_activate.wc_addr    = *wc_addr_behind_nat;
            } else {
                dgram_snd.u.connect_activate.client_addr = *client_addr;
                dgram_snd.u.connect_activate.wc_addr    = *wc_addr;
            }
            dgram_snd.u.connect_activate.dgram_uid = dgram_rcv.u.connect_req.dgram_uid;

            // send the connect activate dgram to both peers
            for (i = 0; i < 3; i++) {
                len = sendto(sfd, &dgram_snd, offsetof(dgram_t,u.connect_activate.dgram_end), 0,
                            (struct sockaddr *)client_addr, sizeof(struct sockaddr_in));
                if (len != offsetof(dgram_t,u.connect_activate.dgram_end)) {
                    ERROR("send connect_activate to client\n");
                }
                len = sendto(sfd, &dgram_snd, offsetof(dgram_t,u.connect_activate.dgram_end), 0,
                            (struct sockaddr *)wc_addr, sizeof(struct sockaddr_in));
                if (len != offsetof(dgram_t,u.connect_activate.dgram_end)) {
                    ERROR("send connect_activate to webcam\n");
                }
            }

            // clear wc_addr, because we need to receive new wc_addr for the next connection
            bzero(wc_addr, sizeof(struct sockaddr_in));
            bzero(wc_addr_behind_nat, sizeof(struct sockaddr_in));
            break;

connect_reject:
            // send the connect reject dgram back to the requesting client
            bzero(&dgram_snd, sizeof(dgram_snd));
            dgram_snd.id = DGRAM_ID_CONNECT_REJECT;
            dgram_snd.u.connect_reject.status = status;
            dgram_snd.u.connect_reject.dgram_uid = dgram_rcv.u.connect_req.dgram_uid;
            for (i = 0; i < 3; i++) {
                len = sendto(sfd, &dgram_snd, offsetof(dgram_t,u.connect_reject.dgram_end), 0,
                            (struct sockaddr *)client_addr, sizeof(struct sockaddr_in));
                if (len != offsetof(dgram_t,u.connect_reject.dgram_end)) {
                    ERROR("send connect_reject to client\n");
                }
            }
        } while (0);
    }

    return NULL;
}

void * dgram_monitor_onl_wc_thread(void * cx) 
{
    int i;
    uint64_t curr_us;

    // loop forever
    while (true) {
        // get current timer
        curr_us = microsec_timer();

        // loop over online webcams
        for (i = 0; i < max_onl_wc; i++) {
            // if unused entry in table then contine
            if (onl_wc[i].wc_macaddr[0] == '\0') {
                continue;
            }

            // if haven't received announde in 5x the announce interval then 
            // remove this webcam from the onl_wc table
            if (curr_us - onl_wc[i].last_announce_rcv_time_us > 5 * P2P_ANNOUNCE_INTVL_US) {
                INFO("wc %s is no longer online\n", onl_wc[i].wc_macaddr);
                bzero(&onl_wc[i], sizeof(onl_wc_t));
            }
        }

        // sleep for the announce interval
        usleep(P2P_ANNOUNCE_INTVL_US);
    }

    return NULL;
}

uint64_t gen_con_id(void)
{
    static uint64_t con_id = 0;

    con_id++;
    return con_id;
}

