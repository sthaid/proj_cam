#include "wc.h"

// TBD - is any locking required

//
// defines
//

// max table entries
#define MAX_USER          1000
#define MAX_ONL_WC        1000
#define MAX_USER_WC       32

// for calls to getcl
#define MAX_GETCL_BUFF    1000
#define MAX_GETCL_ARGV    100

// check if u is root
#define IS_ROOT(u) (strcmp((u)->user_name, "root") == 0)

// location of user account files
#define USER_DIR "user"

//
// typedefs
//

typedef struct {
    char user_name[MAX_USER_NAME+1];
    char password[MAX_PASSWORD+1];
    struct timeval time_created;
    struct {
        char wc_macaddr[MAX_WC_MACADDR+1];
        char wc_name[MAX_WC_NAME+1];
    } wc[MAX_USER_WC];
} user_t;

typedef struct {
    version_t version;
    char wc_macaddr[MAX_WC_MACADDR+1];
    struct sockaddr_in wc_addr;
    struct sockaddr_in wc_addr_behind_nat;
    uint64_t last_announce_rcv_time_us;
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
bool cmd_ls_onl_wc(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_password(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_delete_account(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_version(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_exit(user_t * u, FILE * fp, int argc, char ** argv);
bool cmd_su(user_t * u, FILE * fp, int argc, char ** argv);
void read_all_user_files(void);
bool create_user_file(user_t * u);
bool update_user_file(user_t * u);
bool delete_user_file(user_t * u);
bool getcl(FILE * fp, char * b, int * argc, char ** argv);
void prcl(FILE * fp, char * fmt, ...) __attribute__ ((format (printf, 2, 3)));
void display_user(FILE * fp, user_t * u);
void display_wc(FILE * fp, char * wc_macaddr, user_t * u, int u_wc_idx);
user_t *  verify_user_name_and_password(char * user_name, char *password);
bool verify_wc_macaddr(char * wc_macaddr);
bool verify_wc_name(char * wc_name);
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

    // call subsystem initialization routines
    INFO("initializing\n");
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
    INFO("shutdown\n");
    return 0;
}

void * service_thread(void * cx)
{
    #define VERIFY_USERNAME_AND_PASSWORD() \
        do { \
            u = verify_user_name_and_password(user_name, password); \
            if (u == NULL) { \
                REPLY_ERROR(STATUS_ERR_INVALID_USER_OR_PASSWD); \
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

    // process 0the service request
    if (strcmp(service, "create") == 0) {
        int  status;
        char s[100];

        status = account_create(user_name, password);
        if (status != STATUS_INFO_OK) {
            REPLY_ERROR(status);
            goto done;
        }
        REPLY_LOGIN_OK();
        sprintf(s,"account %s created\n\n", user_name);
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
    int      i;
    
    // verify user_name and password length, and character set
    if (strlen(user_name) > MAX_USER_NAME || strlen(user_name) < MIN_USER_NAME) {
        return STATUS_ERR_USERNAME_LENGTH;
    }
    if (strlen(password) > MAX_PASSWORD || strlen(password) < MIN_PASSWORD) {
        return STATUS_ERR_PASSWORD_LENGTH;
    }
    if (!verify_chars(user_name)) {
        return STATUS_ERR_USERNAME_CHARS;
    }
    if (!verify_chars(password)) {
        return STATUS_ERR_PASSWORD_CHARS;
    }

    // if user_name already exists then error
    for (i = 0; i < max_user; i++) {
        if (strcmp(user[i].user_name, user_name) == 0) {
            return STATUS_ERR_USERNAME_ALREADY_EXISTS;
        }
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
    { "add_wc", cmd_add_wc, 2, 2, false,
      "add_wc <wc_name> <wc_macaddr> - add webcam, owned by this user" },
    { "ren_wc", cmd_ren_wc, 2, 2, false,
      "ren_wc <wc_name> <new_wc_name> - rename webcam" },
    { "del_wc", cmd_del_wc, 1, 1, false,
      "del_wc <wc_name> - delete webcam" },
    { "ls", cmd_ls, 0, 1, false,
      "ls [*|<user>] - display user account info" },
    { "ls_onl_wc", cmd_ls_onl_wc, 0, 0, false,
      "ls_onl_wc - list online webcams" },
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
            prcl(fp, "%s: %s\n", 
                 cmd_tbl[i].name,
                 cmd_tbl[i].description);
        }
    }

    // return, no-logout
    return false;
}


// add_wc <wc_name> <wc_macaddr> - add webcam which is owned by this user
bool cmd_add_wc(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char * wc_name = argv[0];
    char * wc_macaddr = argv[1];
    int    i, j;

    // verify wc_macaddr and wc_name strings are valid
    if (verify_wc_macaddr(wc_macaddr) == false) {
        prcl(fp, "error: invalid wc_macaddr\n");
        return false;
    }
    if (verify_wc_name(wc_name) == false) {
        prcl(fp, "error: invalid wc_name\n");
        return false;
    }

    // verify wc_macaddr is not in use by any user
    for (i = 0; i < max_user; i++) {
        if (user[i].user_name[0] == '\0') {
            continue;
        }
        for (j = 0; j < MAX_USER_WC; j++) {
            if (strcmp(wc_macaddr, user[i].wc[j].wc_macaddr) == 0) {
                prcl(fp, "error: wc_macaddr %s already owned by user %s\n", 
                     wc_macaddr, user[i].user_name);
                return false;
            }
        }
    }

    // find a free slot in this user wc tbl
    for (i = 0; i < MAX_USER_WC; i++) {
        if (u->wc[i].wc_macaddr[0] == '\0') {
            break;
        }
    }
    if (i == MAX_USER_WC) {
        prcl(fp, "error: no space to add webcam\n");
        return false;
    }

    // add the webcam
    strcpy(u->wc[i].wc_macaddr, wc_macaddr);
    strcpy(u->wc[i].wc_name, wc_name);

    // store change in file
    update_user_file(u);

    // return, no-logout
    return false;
}

// ren_wc <wc_name> <new_wc_name> - rename webcam 
bool cmd_ren_wc(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char * wc_name = argv[0];
    char * new_wc_name = argv[1];
    int    i, j;

    // find wc_name in this user wc tbl
    for (i = 0; i < MAX_USER_WC; i++) {
        if (strcmp(wc_name, u->wc[i].wc_name) == 0) {
            break;
        }
    }
    if (i == MAX_USER_WC) {
        prcl(fp, "error: does not exist '%s'\n", wc_name);
        return false;
    }

    // verify new_wc_name
    if (verify_wc_name(new_wc_name) == false) {
        prcl(fp, "error: invalid new name '%s'\n", new_wc_name);
        return false;
    }

    // verify new_wc_name is not already in use
    for (j = 0; j < MAX_USER_WC; j++) {
        if (strcmp(new_wc_name, u->wc[j].wc_name) == 0) {
            prcl(fp, "error: new_wc_name '%s' is already in use\n", new_wc_name);
            return false;
        }
    }

    // replace with new name
    strcpy(u->wc[i].wc_name, new_wc_name);

    // store change in file
    update_user_file(u);

    // return, no-logout
    return false;
}

// del_wc <wc_name> - delete webcam 
bool cmd_del_wc(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char * wc_name = argv[0];
    int    i;

    // find wc_name in this user wc tbl
    for (i = 0; i < MAX_USER_WC; i++) {
        if (strcmp(wc_name, u->wc[i].wc_name) == 0) {
            break;
        }
    }
    if (i == MAX_USER_WC) {
        prcl(fp, "error: does not exist '%s'\n", wc_name);
        return false;
    }

    // delete 
    bzero(&u->wc[i], sizeof(u->wc[i]));

    // store change in file
    update_user_file(u);

    // return, no-logout
    return false;
}

// ls [*|<user>] - display user account info"
bool cmd_ls(user_t * u, FILE * fp, int argc, char ** argv) 
{
    int i;

    // if argc is zero then 
    //   display my account
    // else if argv[0] = "*" then 
    //   display all user names   
    // else
    //   otherwise display specified user
    // endif

    if (argc == 0) {
        display_user(fp,u);
    } else if (strcmp(argv[0], "*") == 0) {
        for (i = 0; i < max_user; i++) {
            if (user[i].user_name[0] == '\0') {
                continue;
            }
            display_user(fp,&user[i]);
        }
    } else {
        for (i = 0; i < max_user; i++) {
            if (strcmp(user[i].user_name, argv[0]) == 0) {
                display_user(fp,&user[i]);
                break;
            }
        }
        if (i == max_user) {
            prcl(fp, "error: user %s does not exist\n", argv[0]);
        }
    }

    // return, no-logout
    return false;
}

// ls_onl_wc - list online webcams
bool cmd_ls_onl_wc(user_t * u, FILE * fp, int argc, char ** argv) 
{
    int i;

    for (i = 0; i < max_onl_wc; i++) {
        if (onl_wc[i].wc_macaddr[0] == '\0') {
            continue;
        }

        display_wc(fp, onl_wc[i].wc_macaddr, NULL, 0);
    }

    return false;
}


// password <curr_passwd> <new_passwd> - change password
bool cmd_password(user_t * u, FILE * fp, int argc, char ** argv) 
{
    char * curr_passwd = argv[0];
    char * new_passwd = argv[1];

    // verify curr_password
    if (strcmp(u->password, curr_passwd) != 0) {
        prcl(fp, "error: current_password invalid\n");
        return false;
    }

    // validate new_password
    if (strlen(new_passwd) > MAX_PASSWORD || strlen(new_passwd) < MIN_PASSWORD) {
        prcl(fp, "error: new_password length, must be %d-%d chars\n",
             MIN_PASSWORD, MAX_PASSWORD);
        return false;
    }
    if (!verify_chars(new_passwd)) {
        prcl(fp, "error: new_password must only contain alphanumeric and '_' chars\n");
        return false;
    }

    // set new_password and write user file
    strcpy(u->password, new_passwd);
    if (!update_user_file(u)) {
        prcl(fp, "error: failed to save new password\n");
        return false;
    }

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

// su <user> - switch user
bool cmd_su(user_t * u, FILE * fp, int argc, char ** argv)
{
    char * user_name = argv[0];
    int i;

    // find the user
    for (i = 0; i < max_user; i++) {
        if (strcmp(user[i].user_name, user_name) == 0) {
            break;
        }
    }
    if (i == max_user) {
        prcl(fp, "error: user does not exist\n");
        return false;
    }

    // recursively call cmd_processor, passing in the new user struct
    cmd_processor(&user[i], fp, false);

    // return, no-logout
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

void prcl(FILE * fp, char * fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
}
                            
void display_user(FILE * fp, user_t * u)
{
    int i;

    for (i = 0; i < MAX_USER_WC; i++) {
        if (u->wc[i].wc_macaddr[0] == '\0') {
            continue;
        }
        display_wc(fp, u->wc[i].wc_macaddr, u, i);
    }
}

void display_wc(FILE * fp, char * wc_macaddr, user_t * u, int u_wc_idx)
{
    int i, j;
    bool online;
    char online_wc_addr_str[100] = "";
    char online_wc_addr_behind_nat_str[100] = "";
    char online_wc_version_str[100] = "";

    // if user not supplied then attempt to find the user that owns this wc
    if (u == NULL) {
        for (i = 0; i < max_user; i++) {
            if (user[i].user_name[0] == '\0') {
                continue;
            }
            for (j = 0; j < MAX_USER_WC; j++) {
                if (strcmp(user[i].wc[j].wc_macaddr, wc_macaddr) == 0) {
                    u = &user[i];
                    u_wc_idx = j;
                    break;
                }
            }
            if (u) {
                break;
            }
        }
    }

    // search the onl_wc table to see if it online, and if so find its wc_addr
    online = false;
    for (i = 0; i < max_onl_wc; i++) {
        if (onl_wc[i].wc_macaddr[0] != '\0' &&
            strcmp(wc_macaddr, onl_wc[i].wc_macaddr) == 0) 
        {
            online = true;
            sock_addr_to_str(online_wc_addr_str, sizeof(online_wc_addr_str), 
                             (struct sockaddr *)&onl_wc[i].wc_addr);
            sock_addr_to_str(online_wc_addr_behind_nat_str, sizeof(online_wc_addr_behind_nat_str), 
                             (struct sockaddr *)&onl_wc[i].wc_addr_behind_nat);
            sprintf(online_wc_version_str, "%d.%d", 
                    onl_wc[i].version.major, onl_wc[i].version.minor);
            break;
        }
    }

    // display, examples:
    // wc1          steve        80:1f:02:d3:9f:0c   offline
    // wc1          steve        80:1f:02:d3:9f:0c   online   1.0   14.91.92.44:7575  192.168.1.101:7575
    // ---          ---          80:1f:02:d3:9f:0c   online   1.0   14.91.92.44:7575  192.168.1.102:7575
    prcl(fp, "%-12s %-12s %-17s %-7s  %s  %s  %s\n",
         u ? u->wc[u_wc_idx].wc_name : "---",
         u ? u->user_name : "---",
         wc_macaddr,
         online ? "online " : "offline",
         online_wc_version_str,
         online_wc_addr_str,
         online_wc_addr_behind_nat_str);
}

user_t * verify_user_name_and_password(char * user_name, char *password) 
{
    int i;

    // locate user
    for (i = 0; i < max_user; i++) {
        if (strcmp(user[i].user_name, user_name) == 0) {
            break;
        }
    }
    if (i == max_user) {
        return NULL;
    }

    // validate password
    if (strcmp(user[i].password, password) != 0) {
        return NULL;
    }

    // success
    return &user[i];
}

bool verify_wc_macaddr(char * wc_macaddr)
{
    int len = strlen(wc_macaddr);

    return len > 0 && len <= MAX_WC_MACADDR && strspn(wc_macaddr, "0123456789abcdefABCDEF:") == len;
}

bool verify_wc_name(char * wc_name)
{
    int len = strlen(wc_name);

    return len > 0 && len <= MAX_WC_NAME && isalpha(wc_name[0]) && verify_chars(wc_name);
}

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
            int                  i, j, k;
            dgram_t              dgram_snd;
            struct sockaddr_in * wc_addr;
            struct sockaddr_in * wc_addr_behind_nat;
            struct sockaddr_in * client_addr = &from_addr;
            user_t             * u;
            char                 s1[100], s2[100];
            int                  status = STATUS_ERR_GENERAL_FAILURE;

            // XXX seeing a lot of prints here when attempting to connect to a wc not online

            // debug print the connect request
            INFO("recvd connect request from %s/%s user=%s wc_name=%s service=%s\n",
                   sock_addr_to_str(s1,sizeof(s1),(struct sockaddr*)client_addr),
                   sock_addr_to_str(s2,sizeof(s2),(struct sockaddr*)&dgram_rcv.u.connect_req.client_addr_behind_nat),
                   dgram_rcv.u.connect_req.user_name,
                   dgram_rcv.u.connect_req.wc_name,
                   SERVICE_STR(dgram_rcv.u.connect_req.service));

            // verify supplied user/password
            u = verify_user_name_and_password(dgram_rcv.u.connect_req.user_name, dgram_rcv.u.connect_req.password);
            if (u == NULL) {
                ERROR("invalid user_name or password\n");
                status = STATUS_ERR_INVALID_USER_OR_PASSWD;
                goto connect_reject;
            }

            // find the wc_name in the user wc table
            for (i = 0; i < MAX_USER_WC; i++) {
                if (strcmp(dgram_rcv.u.connect_req.wc_name, u->wc[i].wc_name) == 0) {
                    break;
                }
            }
            if (i == MAX_USER_WC) {
                ERROR("wc '%s' does not exist for user '%s'\n", 
                      dgram_rcv.u.connect_req.wc_name, u->user_name);
                status = STATUS_ERR_WC_DOES_NOT_EXIST;
                goto connect_reject;
            }

            // find wc_macaddr in the onl_wc table
            for (j = 0; j < max_onl_wc; j++) {
                if (strcmp(onl_wc[j].wc_macaddr, u->wc[i].wc_macaddr) == 0) {
                    break;
                }
            }
            if (j == max_onl_wc) {
                ERROR("wc '%s' is not online\n", dgram_rcv.u.connect_req.wc_name);
                status = STATUS_ERR_WC_NOT_ONLINE;
                goto connect_reject;
            }

            // extract the wc_addr from the onl_wc table, and verify
            wc_addr = &onl_wc[j].wc_addr;
            wc_addr_behind_nat = &onl_wc[j].wc_addr_behind_nat;
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
            for (k = 0; k < 3; k++) {
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
            for (k = 0; k < 3; k++) {
                len = sendto(sfd, &dgram_snd, offsetof(dgram_t,u.connect_reject.dgram_end), 0,
                            (struct sockaddr *)client_addr, sizeof(struct sockaddr_in));
                if (len != offsetof(dgram_t,u.connect_activate.dgram_end)) {
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

