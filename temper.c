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
// XXX clean up prints, and review

#include "wc.h"

//
// defines 
//

#define MILLISEC_TIMER  (microsec_timer() / 1000)

//
// typedefs
//

//
// variables  
//

static int32_t    curr_temper_degs_f;
static uint64_t   curr_temper_acquire_time;
static char     * wc_macaddr;

//
// prototypes
//

static void * temper_send_value_to_admin_server_thread(void * cx);
static struct sockaddr_in get_admin_server_addr(void);
static void * temper_read_thread(void * cx);

// -----------------  API  -----------------------------------------------

int temper_init(char * wc_macaddr_arg)
{
    pthread_t thread_id;

    INFO("XXX called\n");

    // remember the macaddr
    wc_macaddr = wc_macaddr_arg;

    // create thread to periodically read the temperature from the temper device
    pthread_create(&thread_id, NULL, temper_read_thread, NULL);

    // create thread to send temperature values to admin_server
    pthread_create(&thread_id, NULL, temper_send_value_to_admin_server_thread, NULL);

    // return success
    return 0;
}

int temper_read(void)
{
    // if current temperature reading is within 10 seconds then
    //   return the current temperature
    // else
    //   return invalid
    // endif
    if (curr_temper_acquire_time != 0 &&
        MILLISEC_TIMER - curr_temper_acquire_time < 10*1000) 
    {
        return curr_temper_degs_f;
    } else {
        return INVALID_TEMPERATURE;
    }
}

// -----------------  THREAD - SEND VALUE TO ADMIN SEVER -----------------

static void * temper_send_value_to_admin_server_thread(void * cx)
{
    int32_t temperature;
    int32_t ret;
    int32_t sfd;
    dgram_t dgram;
    static struct sockaddr_in admin_server_addr;

    INFO("starting\n");
    
    // create socket 
    sfd = socket(AF_INET, SOCK_DGRAM|SOCK_CLOEXEC, IPPROTO_UDP);
    if (sfd == -1) {
        ERROR("socket, %s\n", strerror(errno));
        return NULL;
    }

    while (true) {
        // read temperature, if invalid sleep for 1 sec and continue
        temperature = temper_read();
        if (temperature == INVALID_TEMPERATURE) {
            sleep(1);
            continue;
        }

        // send temperature dgram
        admin_server_addr = get_admin_server_addr();
        bzero(&dgram, sizeof(dgram));
        dgram.id = DGRAM_ID_TEMPERATURE;
        dgram.u.temperature.temperature = temperature;
        strncpy(dgram.u.wc_announce.wc_macaddr, wc_macaddr, MAX_WC_MACADDR);
        ret = sendto(sfd, &dgram, offsetof(dgram_t,u.temperature.dgram_end), 0,
                     (struct sockaddr *)&admin_server_addr, sizeof(admin_server_addr));
        if (ret != offsetof(dgram_t,u.temperature.dgram_end)) {
            char s[100];
            ERROR("send temperature to %s, %s\n",
                  sock_addr_to_str(s, sizeof(s), (struct sockaddr *)&admin_server_addr),
                  strerror(errno));
        }

        // delay 10 secs
        sleep(10);
    }

    INFO("terminating\n");
}

static struct sockaddr_in get_admin_server_addr(void)
{
    int32_t ret;

    static struct sockaddr_in addr;
    static uint64_t           addr_acquire_time;

    // if we already have the admin_server_addr and it is less than 10 minutes old
    // then return
    if (addr_acquire_time != 0 && MILLISEC_TIMER - addr_acquire_time < 10*60*1000) {
        return addr;
    }

    // attempt to get admin_server_addr, if successful then set the time it was acquired;
    // remain in this loop if admin_server_addr has never been acquired
    while (true) {
        INFO("XXX call getsockaddr\n");
        ret = getsockaddr(ADMIN_SERVER_HOSTNAME, ADMIN_SERVER_DGRAM_PORT, SOCK_DGRAM, IPPROTO_UDP, &addr);
        if (ret == 0) {
            addr_acquire_time = MILLISEC_TIMER;
        } else {
            ERROR("XXX call getsockaddr failed, %s\n", strerror(errno));
        }

        if (addr_acquire_time != 0) {
            break;
        }

        INFO("XXX failed, try again in 10 secs\n");
        sleep(10);
    }

    // return addr
    return addr;
}

// -----------------  THREAD - INTERFACE TO TEMPER DEVICE  ---------------

static void * temper_read_thread(void * cx)
{
    INFO("starting\n");

    while (true) {
        usleep(1000000);
        curr_temper_degs_f++;
        curr_temper_acquire_time = MILLISEC_TIMER;     
    }

    INFO("terminating\n");
    return NULL;
}

