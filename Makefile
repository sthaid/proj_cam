
TARGETS = cloud_server wc_server admin viewer loginwc nettest_to_cloud_server nettest_to_wc

CC = gcc
CFLAGS = -c -g -O2 -pthread -fsigned-char -Wall 
SDLCFLAGS = $(shell sdl2-config --cflags)

CLOUD_SERVER_OBJS            = cloud_server.o p2p1.o util.o
WC_SERVER_OBJS               = wc_main.o wc_nettest.o wc_login.o wc_webcam.o p2p1.o util.o jpeg_decode.o
ADMIN_OBJS                   = admin.o p2p1.o util.o 
VIEWER_OBJS                  = viewer.o p2p1.o p2p2.o util.o jpeg_decode.o
LOGIN_OBJS                   = loginwc.o p2p1.o p2p2.o util.o
NETTEST_TO_CLOUD_SERVER_OBJS = nettest_to_cloud_server.o util.o
NETTEST_TO_WC_OBJS           = nettest_to_wc.o p2p1.o p2p2.o util.o 

#
# build rules
#

all: $(TARGETS)

cloud_server: $(CLOUD_SERVER_OBJS) 
	$(CC) -pthread -lrt -o $@ $(CLOUD_SERVER_OBJS)

wc_server: $(WC_SERVER_OBJS) 
	$(CC) -pthread -lrt -ljpeg -o $@ $(WC_SERVER_OBJS)

admin: $(ADMIN_OBJS) 
	$(CC) -pthread -lrt -lreadline -o $@ $(ADMIN_OBJS)

viewer: $(VIEWER_OBJS) 
	$(CC) -pthread -lrt -ljpeg -lSDL2 -lSDL2_ttf -o $@ $(VIEWER_OBJS)

loginwc: $(LOGIN_OBJS) 
	$(CC) -pthread -lrt -o $@ $(LOGIN_OBJS)

nettest_to_cloud_server: $(NETTEST_TO_CLOUD_SERVER_OBJS) 
	$(CC) -pthread -lrt -o $@ $(NETTEST_TO_CLOUD_SERVER_OBJS)

nettest_to_wc: $(NETTEST_TO_WC_OBJS) 
	$(CC) -pthread -lrt -o $@ $(NETTEST_TO_WC_OBJS)

#
# clean rule
#

clean:
	rm -f $(TARGETS) *.o

#
# compile rules
#

admin.o:                   admin.c wc.h
wc_main.o:                 wc_main.c wc.h
wc_nettest.o:              wc_nettest.c wc.h
wc_login.o:                wc_login.c wc.h
wc_webcam.o:               wc_webcam.c wc.h
nettest_to_wc.o:           nettest_to_wc.c wc.h
nettest_to_cloud_server.o: nettest_to_cloud_server.c wc.h
cloud_server.o:            cloud_server.c wc.h
loginwc.o:                 loginwc.c wc.h
p2p1.o:                    p2p1.c wc.h
p2p2.o:                    p2p2.c wc.h
util.o:                    util.c wc.h
jpeg_decode.o:             jpeg_decode.c wc.h

viewer.o: viewer.c wc.h
	$(CC) $(CFLAGS) $(SDLCFLAGS) $< -o $@

.c.o: 
	$(CC) $(CFLAGS) $< -o $@


