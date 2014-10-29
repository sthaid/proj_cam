#include "wc.h"

// -----------------  CONNECT_TO_CLOUD_SERVER   --------------------------

int connect_to_cloud_server(char * user_name, char * password, char * service, int port)
{
    struct sockaddr_in addr;
    char               login[3*32];
    char               login_err_str[200];
    int                ret, sfd, len, login_response;
    char               s[100];

    // get address of CLOUD_SERVER
    ret =  getsockaddr(CLOUD_SERVER_HOSTNAME, port, SOCK_STREAM, 0, &addr); 
    if (ret < 0) {
        ERROR("failed to get address of %s\n", CLOUD_SERVER_HOSTNAME);
        return -1;
    }
    NOTICE("address of %s is %s\n",
          CLOUD_SERVER_HOSTNAME, sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&addr));

    // create socket
    sfd = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (sfd == -1) { 
        ERROR("socket, %s\n", strerror(errno));
        return -1;
    } 

    // connect to the cloud server
    ret = connect(sfd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == -1) {
        ERROR("connect, %s\n", strerror(errno));
        close(sfd);
        return -1;
    }

    // send login string 
    bzero(login, sizeof(login));
    strncpy(login,    user_name, 31);
    strncpy(login+32, password,  31);
    strncpy(login+64, service,   31);
    len = write(sfd, login, sizeof(login));
    if (len != sizeof(login)) {
        ERROR("sending login request to cloud server, %s\n", strerror(errno));
        close(sfd);
        return -1;
    }

    // read ack
    len = recv(sfd, &login_response, sizeof(login_response), MSG_WAITALL);
    if (len != sizeof(login_response)) {
        ERROR("reading login response from cloud server, %s\n", strerror(errno));
        close(sfd);
        return -1;
    }
    if (login_response != CLOUD_SERVER_LOGIN_OK) {
        bzero(login_err_str,sizeof(login_err_str));
        memcpy(login_err_str, &login_response, 4);
        read(sfd,login_err_str+4,sizeof(login_err_str)-5);
        ERROR("login failed: %s\n", login_err_str);
        close(sfd);
        return -1;
    }

    // return sfd
    return sfd;
}
// -----------------  SOCKET UTILS  ---------------------------------------

int getsockaddr(char * node, int port, int socktype, int protcol, struct sockaddr_in * ret_addr)
{
    struct addrinfo   hints;
    struct addrinfo * result;
    char              port_str[20];
    int               ret;

    sprintf(port_str, "%d", port);

    bzero(&hints, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags    = AI_NUMERICSERV;

    ret = getaddrinfo(node, port_str, &hints, &result);
    if (ret != 0) {
        ERROR("failed to get address of %s, %s\n", node, gai_strerror(ret));
        return -1;
    }
    if (result->ai_addrlen != sizeof(*ret_addr)) {
        ERROR("getaddrinfo result addrlen=%d, expected=%d\n",
            (int)result->ai_addrlen, (int)sizeof(*ret_addr));
        return -1;
    }

    *ret_addr = *(struct sockaddr_in*)result->ai_addr;
    freeaddrinfo(result);
    return 0;
}

void set_sock_opts(int sfd, int reuseaddr, int sndbuf, int rcvbuf, int rcvto_us)
{
    int ret;

    if (reuseaddr != -1) {
        ret = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
        if (ret == -1) {
            printf("setsockopt SO_REUSEADDR, %s", strerror(errno));
        }
    }

    if (sndbuf != -1) {
        ret = setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        if (ret == -1) {
            printf("setsockopt SO_SNDBUF, %s", strerror(errno));
        }
    }

    if (rcvbuf != -1) {
        ret = setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        if (ret == -1) {
            printf("setsockopt SO_RCVBUF, %s", strerror(errno));
        }
    }

    if (rcvto_us != -1) {
        struct timeval rcvto = {rcvto_us/1000000, rcvto_us%1000000};
        ret = setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));
        if (ret == -1) {
            printf("setsockopt SO_RCVBUF, %s", strerror(errno));
        }
    }
}

char * sock_to_options_str(int sfd, char * s, int slen)
{
    int reuseaddr=0, sndbuf=0, rcvbuf=0;
    struct timeval rcvto={0,0};
    socklen_t reuseaddr_len=4, sndbuf_len=4, rcvbuf_len=4, rcvto_len=sizeof(rcvto);

    getsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, &reuseaddr_len);
    getsockopt(sfd, SOL_SOCKET, SO_SNDBUF,    &sndbuf   , &sndbuf_len);
    getsockopt(sfd, SOL_SOCKET, SO_RCVBUF,    &rcvbuf   , &rcvbuf_len);
    getsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO,  &rcvto,     &rcvto_len);
    snprintf(s, slen, "REUSEADDR=%d SNDBUF=%d RCVBUF=%d RCVTIMEO=%d", 
             reuseaddr, sndbuf, rcvbuf,
             (int)(rcvto.tv_sec+1000000*rcvto.tv_usec));

    return s;
}

char * sock_addr_to_str(char * s, int slen, struct sockaddr * addr)
{
    char addr_str[100];
    int port;

    if (addr->sa_family == AF_INET) {
        inet_ntop(AF_INET,
                  &((struct sockaddr_in*)addr)->sin_addr,
                  addr_str, sizeof(addr_str));
        port = ((struct sockaddr_in*)addr)->sin_port;
    } else if (addr->sa_family == AF_INET6) {
        inet_ntop(AF_INET6,
                  &((struct sockaddr_in6*)addr)->sin6_addr,
                 addr_str, sizeof(addr_str));
        port = ((struct sockaddr_in6*)addr)->sin6_port;
    } else {
        snprintf(s,slen,"Invalid AddrFamily %d", addr->sa_family);
        return s;        
    }

    snprintf(s,slen,"%s:%d",addr_str,port);
    return s;
}

// -----------------  INTERFACE UTILS -------------------------------------

int getmacaddr(char * macaddr_str)
{
    int           sfd;
    struct ifreq  ifr;
    struct ifconf ifc;
    char          buf[1000];
    uint8_t       macaddr[6];
    int           i, max_intfc, ret=-1;

    // preset return string to empty
    macaddr_str[0] = '\0';

    // open socket
    sfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sfd < 0) {
        ERROR("socket, %s\n", strerror(errno));
        goto done;
    }

    // get list of interfaces
    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sfd, SIOCGIFCONF, &ifc) < 0) {
        ERROR("SIOCGIFCONF, %s\n", strerror(errno));
        goto done;
    }
    max_intfc = ifc.ifc_len/sizeof(struct ifreq);

    // loop over list of interfaces
    for (i = 0; i < max_intfc; i++) {
        // get flags
        strcpy(ifr.ifr_name, ifc.ifc_req[i].ifr_name);
        if (ioctl(sfd, SIOCGIFFLAGS, &ifr) < 0) {
            ERROR("SIOCGIFFLAGS, %s\n", strerror(errno));
            goto done;
        }

        // skip loopback interfaces
        if (ifr.ifr_flags & IFF_LOOPBACK) {
            continue;
        }

        // get macaddr
        if (ioctl(sfd, SIOCGIFHWADDR, &ifr) < 0) {
            ERROR("SIOCGIFHWADDR, %s\n", strerror(errno));
            goto done;
        }

        // convert macaddr to string, and we're done
        memcpy(macaddr, ifr.ifr_hwaddr.sa_data, 6);
        sprintf(macaddr_str, "%2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x",
                macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5]);
        ret = 0;
        break;
    }

    // check for no macaddr found
    if (i == max_intfc) {
        ERROR("macaddr not found\n");
        goto done;
    }

done:
    // close socket and return success
    close(sfd);
    return ret;
}
        
// -----------------  LOGGING UTILS  --------------------------------------

FILE * logmsg_fp;

void logmsg_init(char * logmsg_file)
{
    if (strcmp(logmsg_file, "stdout") == 0) {
        logmsg_fp = stdout;
    } else {
        logmsg_fp = fopen(logmsg_file, "ae");   // mode: append, close-on-exec
        if (logmsg_fp == NULL) {
            FATAL("failed to create logmsg file %s, %s\n", logmsg_file, strerror(errno));
        }
    }

    setlinebuf(logmsg_fp);
}

void logmsg(char *lvl, const char *func, char *fmt, ...) 
{
    va_list ap;
    char msg[1000];
    int len;
    char time_str[MAX_TIME_STR];

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    len = strlen(msg);
    if (msg[len-1] == '\n') {
        msg[len-1] = '\0';
    }

    if (logmsg_fp) {
        fprintf(logmsg_fp, "%s %s %s: %s\n",
                time2str(time_str, time(NULL), false),
                lvl, func, msg);
    } else {
        if (strcmp(lvl,"NOTICE") == 0) {
            printf("%s\n", msg);
        } else {
            printf("%s %s: %s\n", lvl, func, msg);
        }
    }
}

// -----------------  IMAGE UTILS  ----------------------------------------

static inline unsigned char clip(int x)
{
    if (x < 0) {
        return 0;
    } else if (x > 255) {
        return 255;
    } else {
        return x;
    }
}

void convert_yuy2_to_rgb(uint8_t * yuy2, uint8_t * rgb, int pixels)
{
    int y0,u0,y1,v0,c,d,e;
    int i;

    for (i = 0;  i < pixels/2; i++) {
        y0 = yuy2[0];
        u0 = yuy2[1];
        y1 = yuy2[2];
        v0 = yuy2[3];
        yuy2 += 4;
        c = y0 - 16;
        d = u0 - 128;
        e = v0 - 128;
        rgb[0] = clip(( 298 * c + 516 * d           + 128) >> 8); // blue
        rgb[1] = clip(( 298 * c - 100 * d - 208 * e + 128) >> 8); // green
        rgb[2] = clip(( 298 * c           + 409 * e + 128) >> 8); // red
        rgb[3] = 0;
        c = y1 - 16;
        rgb[4] = clip(( 298 * c + 516 * d           + 128) >> 8); // blue
        rgb[5] = clip(( 298 * c - 100 * d - 208 * e + 128) >> 8); // green
        rgb[6] = clip(( 298 * c           + 409 * e + 128) >> 8); // red
        rgb[7] = 0;
        rgb += 8;
    }
}

void convert_yuy2_to_gs(uint8_t * yuy2, uint8_t * gs, int pixels)
{
    int y0,u0,y1,v0,c,d,e,b,g,r;
    int i;
    
    for (i = 0;  i < pixels/2; i++) {
        y0 = yuy2[0];
        u0 = yuy2[1];
        y1 = yuy2[2];
        v0 = yuy2[3];
        yuy2 += 4;

        c = y0 - 16;
        d = u0 - 128;
        e = v0 - 128;
        b = clip(( 298 * c + 516 * d           + 128) >> 8); // blue
        g = clip(( 298 * c - 100 * d - 208 * e + 128) >> 8); // green
        r = clip(( 298 * c           + 409 * e + 128) >> 8); // red
        *gs++ = (b + g + r) / 3;

        c = y1 - 16;
        b = clip(( 298 * c + 516 * d           + 128) >> 8); // blue
        g = clip(( 298 * c - 100 * d - 208 * e + 128) >> 8); // green
        r = clip(( 298 * c           + 409 * e + 128) >> 8); // red
        *gs++ = (b + g + r) / 3;
    }
}

// -----------------  TIME UTILS  -----------------------------------------

uint64_t microsec_timer(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC,&ts);
    return  ((uint64_t)ts.tv_sec * 1000000) + ((uint64_t)ts.tv_nsec / 1000);
}

uint64_t get_real_time_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME,&ts);
    return  ((uint64_t)ts.tv_sec * 1000000) + ((uint64_t)ts.tv_nsec / 1000);
}

char * time2str(char * str, time_t time, bool gmt) 
{
    struct tm tm;

    if (gmt) {
        gmtime_r(&time, &tm);
    } else {
        localtime_r(&time, &tm);
    }

    snprintf(str, MAX_TIME_STR,
            "%2.2d/%2.2d/%2.2d %2.2d:%2.2d:%2.2d",
            tm.tm_mon+1, tm.tm_mday, tm.tm_year%100,
            tm.tm_hour, tm.tm_min, tm.tm_sec);

    return str;
}

bool system_clock_is_set(void)
{
    FILE * fp;
    int    len, cnt, stratum=0;
    char   s[100] = "";
    bool   clock_is_set;

    fp = popen("ntpq -c \"rv 0 stratum\" < /dev/null 2>&1", "re");
    if (fp == NULL) {
        ERROR("popen, %s\n", strerror(errno));
        return false;
    }

    fgets(s, sizeof(s), fp);
    len = strlen(s);
    if (len > 0 && s[len-1] == '\n') {
        s[len-1] = '\0';
    }

    cnt = sscanf(s, "stratum=%d", &stratum);
    clock_is_set = (cnt == 1 && stratum < 16);

    NOTICE("ntpq returned '%s' - clock_is_set=%d\n", s, clock_is_set);

    fclose(fp);

    return clock_is_set;
}

// -----------------  FILE SYSTEM UTILS  ----------------------------------

uint64_t fs_avail_bytes(char * path)
{
    int ret;
    uint64_t free_bytes;
   struct statvfs buf;

    ret = statvfs(path, &buf);
    if (ret < 0) {
        ERROR("statvfs, %s\n", strerror(errno));
        return 0;
    }

    free_bytes = (uint64_t)buf.f_bavail * buf.f_bsize;
    NOTICE("fs path %s, free_bytes=%"PRId64"\n", path, free_bytes);

    return free_bytes;
}


// -----------------  MISC UTILS  -----------------------------------------

char * status2str(uint32_t status)
{
    return STATUS_STR(status);
}

char * int2str(char * str, int64_t n)
{
    sprintf(str, "%"PRId64, n);
    return str;
}

