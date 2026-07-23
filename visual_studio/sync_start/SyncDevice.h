#pragma once
// SyncDevice class to manage each LAN device to be synchronized
// includes socket control info and provides a method for quickly
// issuing a TCP command for starting the remote device
//
// uses snippits of socket test code from:
// * https://www.geeksforgeeks.org/socket-programming-cc/
// * https://monoxid.net/c/socket-connection-timeout/
//
// Author: Mike Kokko
// Updated: 02-Nov-2023

#ifndef SYNCDEVICE_H
#define SYNCDEVICE_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <fcntl.h>
#include <errno.h> 
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <iostream> 

#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "AdvApi32.lib")

#define DEFAULT_BUFLEN 512
#define DEFAULT_PORT "9993"

// enum for types of device
enum syncDeviceType {
	HyperDeck,
	Kinematics,
	Other
};

// class for synchronizable devices
// TODO: should have a copy constructor to avoid issues in std::vector
class SyncDevice {
private:
	std::string dev_name;
	std::string ip_address;
	unsigned int port;
	syncDeviceType type;
	bool readyToConnect = false;
	int socket_fd, status, res, opt, valread;
	bool deviceInitialized = false;
	struct timeval timeout;    // timeval defined in <sys/time.h> in POSIX, <winsock2.h> in Windows
	char buffer[1024] = { 0 }; // doesn't need delete[] in destructor because it is allocated here
	char recordCommand[512] = { 0 };
	char dev_command[1024] = { 0 };
	char dev_resp[1024] = { 0 };
	unsigned int recordCommandLength;
	char portstr[sizeof(int) * 8 + 1];

	SOCKET ConnectSocket = INVALID_SOCKET;
	WSADATA wsaData;
	int iResult;
	struct addrinfo* result = NULL,
		* ptr = NULL,
		hints;
	int recvbuflen = DEFAULT_BUFLEN;
	const char* sendbuf = "record";

public:
	SyncDevice(std::string arg_dev_name, std::string arg_ip, unsigned int arg_port, syncDeviceType arg_type) :
		dev_name{ arg_dev_name },
		ip_address{ arg_ip },
		port{ arg_port },
		type{ arg_type },
		readyToConnect{ true } {}
	~SyncDevice() {
		// do nothing
	};

	int Close(void);
	std::string getName(void);
	void PrintInfo(void);
	int sendRecordCommand(void);
	int SendHyperDeckConfigCmd(const char* msg);
	int Init(void);
};

#endif