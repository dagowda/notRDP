CC64   = x86_64-w64-mingw32-gcc
CC32   = i686-w64-mingw32-gcc
CFLAGS = -c -Wall -Wno-unused-variable

all: screenshot.x64.o screeninput.x64.o notrdp_mgr.x64.o

both: screenshot.x64.o screeninput.x64.o notrdp_mgr.x64.o \
      screenshot.x86.o screeninput.x86.o notrdp_mgr.x86.o

screenshot.x64.o: screenshot.c beacon.h
	$(CC64) $(CFLAGS) -o $@ screenshot.c

screeninput.x64.o: screeninput.c beacon.h
	$(CC64) $(CFLAGS) -o $@ screeninput.c

notrdp_mgr.x64.o: notrdp_mgr.c beacon.h
	$(CC64) $(CFLAGS) -o $@ notrdp_mgr.c

screenshot.x86.o: screenshot.c beacon.h
	$(CC32) $(CFLAGS) -o $@ screenshot.c

screeninput.x86.o: screeninput.c beacon.h
	$(CC32) $(CFLAGS) -o $@ screeninput.c

notrdp_mgr.x86.o: notrdp_mgr.c beacon.h
	$(CC32) $(CFLAGS) -o $@ notrdp_mgr.c

clean:
	rm -f *.x64.o *.x86.o

.PHONY: all both clean
