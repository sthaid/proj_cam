/*
Copyright (c) 2016 Steven Haid

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
#include <usb.h>
#include <math.h>

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
    // if current temperature reading is within 60 seconds then
    //   return the current temperature
    // else
    //   return invalid
    // endif
    if (curr_temper_acquire_time != 0 &&
        MILLISEC_TIMER - curr_temper_acquire_time < 60*1000) 
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
        strncpy(dgram.u.temperature.wc_macaddr, wc_macaddr, MAX_WC_MACADDR);
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
        ret = getsockaddr(ADMIN_SERVER_HOSTNAME, ADMIN_SERVER_DGRAM_PORT, SOCK_DGRAM, IPPROTO_UDP, &addr);
        if (ret == 0) {
            addr_acquire_time = MILLISEC_TIMER;
        }
        if (addr_acquire_time != 0) {
            break;
        }
        sleep(10);
    }

    // return addr
    return addr;
}

// -----------------  THREAD - INTERFACE TO TEMPER DEVICE  ---------------

// Purchased USB Thermometer from Amazon on Nov 11, 2016
//   Device is labelled:  "TEMPer"
//   Ebest New USB Thermometer Temperature Data Record for PC Laptop
//   https://www.amazon.com/gp/product/B009YRP906/ref=oh_aui_detailpage_o00_s00?ie=UTF8&psc=1
//
//   The TEMPer device I purchased:
//     pi@raspberrypi ~ $ lsusb
//        Bus 001 Device 007: ID 0c45:7401 Microdia 
//     pi@raspberrypi ~ $ dmesg | grep -i temper
//        usb 1-1.2.3: Product: TEMPerV1.4
// 
// Looking for source code, found the following choices
//   1- http://www.linuxjournal.com/content/temper-pi
//       If you have ID 0c45:7401 Microdia then congratulations, you have the new 
//       TEMPer probe and will use
//          wget http://www.isp-sl.com/pcsensor-1.0.0.tgz
//   2- http://dev-random.net/temperature-measuring-using-linux-and-raspberry-pi/
//          wget http://dev-random.net/wp-content/uploads/2016/01/temperv14.zip
//   3- https://github.com/edorfaus/TEMPered
//
// Decided to use choice #1, http://www.isp-sl.com/pcsensor-1.0.0.tgz as the 
// basis for the code that follows.
//
// Needed to do the following on the Raspberry Pi, to build and run the pcsensor program
// when logged in as 'pi'.
//    # install libusb-dev
//    sudo apt-get install libusb-dev
//    # build
//    tar -xvf pcsensor-1.0.0.tgz
//    cd pcsensor-1.0.0
//    make clean
//    make
//    # install udev rules so the pcsensor pgm can be run by the pi user
//    sudo cp 99-tempsensor.rules /etc/udev/rules.d
//    reboot
//    # run it
//    ./pcsensor   
//
// The udev file contains
//    pi@raspberrypi ~/pcsensor-1.0.0 $ cat 99-tempsensor.rules 
//    SUBSYSTEMS=="usb", ACTION=="add", ATTRS{idVendor}=="0c45", ATTRS{idProduct}=="7401", MODE="666"
//
// This is the .vimrc file I use on raspberry pi
//    set expandtab
//    set tabstop=8
//    set softtabstop=4
//    set shiftwidth=4
//    set background=dark
//    autocmd FileType * setlocal formatoptions-=c formatoptions-=r formatoptions-=o
//    set guifont=Monospace\ Bold\ 12
//    colors default
//    " enable horizontal scroll
//    "set nowrap
//    map <C-L> 20zl " Scroll 20 characters to the right
//    map <C-H> 20zh " Scroll 20 characters to the left
//    syntax on
//
// And this is the indent program options used to reformat pcsensor.c
//    indent -br -l100 -i4 -ce -cdw -nut -brs  pcsensor.c

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

// The following code is derivied from http://www.isp-sl.com/pcsensor-1.0.0.tgz
// and substantially modified.

// pcsensor.c by Juan Carlos Perez(c) 2011(cray@isp-sl.com)
// based on Temper.c by Robert Kavaler(c) 2009(relavak.com)
// All rights reserved.
//
// Temper driver for linux. This program can be compiled either as a library
// or as a standalone program(-DUNIT_TEST). The driver will work with some
// TEMPer usb devices from RDing(www.PCsensor.com).
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
// 
// THIS SOFTWARE IS PROVIDED BY Juan Carlos Perez ''AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL Robert kavaler BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
//(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// defines

#define VENDOR_ID  0x0c45
#define PRODUCT_ID 0x7401
#define INTERFACE1 0x00
#define INTERFACE2 0x01

// usb extension cable:  -2
// micro usb hub:        -8
#define CALIBRATION_DEG_F (-8)

// variables

const static int reqIntLen = 8;
const static int timeout = 5000;    // timeout in ms 
const static char uTemperatura[] = { 0x01, 0x80, 0x33, 0x01, 0x00, 0x00, 0x00, 0x00 };
const static char uIni1[] = { 0x01, 0x82, 0x77, 0x01, 0x00, 0x00, 0x00, 0x00 };
const static char uIni2[] = { 0x01, 0x86, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00 };

// prototypes

static usb_dev_handle * setup_libusb_access();
static bool ini_control_transfer(usb_dev_handle * dev);
static bool control_transfer(usb_dev_handle * dev, const char *pquestion);
static bool interrupt_read(usb_dev_handle * dev);
static bool interrupt_read_temperatura(usb_dev_handle * dev, double *tempf);

// implementation

static void * temper_read_thread(void * cx)
{
    usb_dev_handle *lvr_winusb = NULL;

    usb_init();

try_again:
    lvr_winusb = setup_libusb_access();
    if (lvr_winusb == NULL) {
        DEBUG("setup_libusb_access failed\n");
        goto cleanup_and_try_again;
    }

    if (!ini_control_transfer(lvr_winusb) ||
        !control_transfer(lvr_winusb, uTemperatura) ||
        !interrupt_read(lvr_winusb) ||
        !control_transfer(lvr_winusb, uIni1) ||
        !interrupt_read(lvr_winusb) ||
        !control_transfer(lvr_winusb, uIni2) ||
        !interrupt_read(lvr_winusb) ||
        !interrupt_read(lvr_winusb))
    {
        ERROR("init fail, cleanup_and_try_again\n");
        goto cleanup_and_try_again;
    }

    while (true) {
        double tempf;

        if (!control_transfer(lvr_winusb, uTemperatura) ||
            !interrupt_read_temperatura(lvr_winusb, &tempf))
        {
            ERROR("read temperature fail, cleanup_and_try_again\n");
            goto cleanup_and_try_again;
        }

        curr_temper_degs_f = round(tempf);
        curr_temper_acquire_time = MILLISEC_TIMER;     
        DEBUG("read temperature %lf F, %d F\n", tempf, curr_temper_degs_f);

        sleep(1);
    }

cleanup_and_try_again:
    if (lvr_winusb != NULL) {
        usb_release_interface(lvr_winusb, INTERFACE1);
        usb_release_interface(lvr_winusb, INTERFACE2);
        usb_close(lvr_winusb);
        lvr_winusb = NULL;
    }
    sleep(10);
    DEBUG("DEBUG try again\n");
    goto try_again;

    return NULL;
}

static usb_dev_handle * setup_libusb_access()
{
    usb_dev_handle *lvr_winusb = NULL;
    struct usb_bus *bus;
    struct usb_device *dev;
    int ret;

    usb_find_busses();
    usb_find_devices();

    for (bus = usb_busses; bus; bus = bus->next) {
        for (dev = bus->devices; dev; dev = dev->next) {
            if (dev->descriptor.idVendor == VENDOR_ID && dev->descriptor.idProduct == PRODUCT_ID) {
                if (!(lvr_winusb = usb_open(dev))) {
                    ERROR("could not open TEMPer device\n");
                    goto error;  
                }
            }
        }
    }

    if (lvr_winusb == NULL) {
        static bool first = true;
        if (first) {
            INFO("could not find TEMPer device\n");
            first = false;
        } else {
            DEBUG("could not find TEMPer device\n");
        }
        goto error;  
    }

    usb_detach_kernel_driver_np(lvr_winusb, INTERFACE1);
    usb_detach_kernel_driver_np(lvr_winusb, INTERFACE2);

    ret = usb_set_configuration(lvr_winusb, 0x01);
    if (ret) {
        ERROR("could not set configuration ret=%d\n", ret);
        goto error;  
    }

    ret = usb_claim_interface(lvr_winusb, INTERFACE1);
    if (ret) {
        ERROR("could not claim interface1 ret=%d\n", ret);
        goto error;  
    }

    ret = usb_claim_interface(lvr_winusb, INTERFACE2);
    if (ret) {
        ERROR("could not claim interface2 ret=%d\n", ret);
        goto error;  
    }

    return lvr_winusb;

error:
    if (lvr_winusb) {
        usb_release_interface(lvr_winusb, INTERFACE1);
        usb_release_interface(lvr_winusb, INTERFACE2);
        usb_close(lvr_winusb);
    }
    return NULL;
}

static bool ini_control_transfer(usb_dev_handle * dev)
{
    int r;
    char question[] = { 0x01, 0x01 };

    r = usb_control_msg(dev, 0x21, 0x09, 0x0201, 0x00,(char *) question, 2, timeout);
    if (r < 0) {
        ERROR("usb_control_msg ret=%d\n", r);
        return false;
    }

    return true;
}

static bool control_transfer(usb_dev_handle * dev, const char *pquestion)
{
    int r;
    char question[reqIntLen];

    memcpy(question, pquestion, sizeof question);
    r = usb_control_msg(dev, 0x21, 0x09, 0x0200, 0x01,(char *) question, reqIntLen, timeout);
    if (r < 0) {
        ERROR("usb_control_msg ret=%d\n", r);
        return false;
    }

    return true;
}

static bool interrupt_read(usb_dev_handle * dev)
{
    int r;
    char answer[reqIntLen];
    bzero(answer, reqIntLen);

    r = usb_interrupt_read(dev, 0x82, answer, reqIntLen, timeout);
    if (r != reqIntLen) {
        ERROR("usb_interrupt_read r=%d\n", r);
        return false;
    }

    return true;
}

static bool interrupt_read_temperatura(usb_dev_handle * dev, double *tempf)
{
    int r, x;
    char answer[reqIntLen];
    bzero(answer, reqIntLen);

    r = usb_interrupt_read(dev, 0x82, answer, reqIntLen, timeout);
    if (r != reqIntLen) {
        ERROR("usb_interrupt_read ret=%d\n", r);
        return false;
    }

    x = ((unsigned char)(answer[2]) << 8) |
        ((unsigned char)(answer[3])     );

    DEBUG("hi=%2.2x lo=%2.2x combined=%4.4x\n",
        (unsigned char)(answer[2]),
        (unsigned char)(answer[3]),
        x);

    *tempf = (x / 256.0) * 1.8 + 32.0 + CALIBRATION_DEG_F;

    return true;
}

