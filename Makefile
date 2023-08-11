INCLUDE_SYS = /usr/local/include
INCLUDE = ~/include ./include /usr/include/opencv4 ../cxxopts/include /usr/local/include
LIB = /usr/local/lib /usr/local/lib/cmake/yaml-cpp

CC = g++
CFLAGS = -no-pie -pthread -Wall
LD_FLAGS_CROP = -lavutil -lavformat -lavcodec -lyaml-cpp
LD_FLAGS_TIFF = -lavutil -lavformat -lavcodec -lswscale -lopencv_core -lopencv_imgproc -lopencv_highgui -lopencv_imgcodecs

INC_SYS_PARAMS = $(addprefix -isystem,$(INCLUDE_SYS))
INC_PARAMS = $(addprefix -I,$(INCLUDE))
LIB_PARAMS = $(addprefix -L,$(LIB))

default: vcrop vtiff

vcrop: vcrop.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIB_PARAMS) $(LD_FLAGS_CROP)
	rm -f vcrop.o

vtiff: vtiff.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIB_PARAMS) $(LD_FLAGS_TIFF)
	rm -f vtiff.o

vcrop.o: ./src/vcrop.cpp
	$(CC) $(CFLAGS) $(INC_SYS_PARAMS) $(INC_PARAMS) -o $@ -c $<

vtiff.o: ./src/vtiff.cpp
	$(CC) $(CFLAGS) $(INC_SYS_PARAMS) $(INC_PARAMS) -o $@ -c $<

clean:
	rm -f vcrop vtiff
	