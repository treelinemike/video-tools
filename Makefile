INCLUDE_SYS = /usr/local/include
INCLUDE = ~/include ./include /usr/include/opencv4 ../cxxopts/include /usr/local/include
LIB = /usr/local/lib /usr/local/lib/cmake/yaml-cpp

CC = g++
CFLAGS = -no-pie -pthread -Wall -Ofast
LD_FLAGS_CROP = -lavutil -lavformat -lavcodec -lavfilter -lyaml-cpp
LD_FLAGS_TIFF = -lavutil -lavformat -lavcodec -lavfilter -lswscale -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_imgcodecs
LD_FLAGS_SYNC = -lyaml-cpp

INC_SYS_PARAMS = $(addprefix -isystem,$(INCLUDE_SYS))
INC_PARAMS = $(addprefix -I,$(INCLUDE))
LIB_PARAMS = $(addprefix -L,$(LIB))

default: vclip vtiff sync_start

vclip: vclip.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIB_PARAMS) $(LD_FLAGS_CROP)
	rm -f vcrop.o

vtiff: vtiff.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIB_PARAMS) $(LD_FLAGS_TIFF)
	rm -f vtiff.o
	
sync_start: SyncDevice.o sync_start.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIB_PARAMS) $(LD_FLAGS_SYNC)
	rm -f SyncDevice.o
	rm -f sync_start.o

vclip.o: ./src/vclip.cpp
	$(CC) $(CFLAGS) $(INC_SYS_PARAMS) $(INC_PARAMS) -o $@ -c $<

vtiff.o: ./src/vtiff.cpp
	$(CC) $(CFLAGS) $(INC_SYS_PARAMS) $(INC_PARAMS) -o $@ -c $<

SyncDevice.o: ./src/SyncDevice.cpp
	$(CC) $(CFLAGS) $(INC_SYS_PARAMS) $(INC_PARAMS) -o $@ -c $<

sync_start.o: ./src/sync_start.cpp
	$(CC) $(CFLAGS) $(INC_SYS_PARAMS) $(INC_PARAMS) -o $@ -c $<

clean:
	rm -f vclip vtiff sync_start
	
