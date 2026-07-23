#include "SyncDevice.h"

int SyncDevice::Close(void) {
	iResult = shutdown(ConnectSocket, SD_SEND);
	if (iResult == SOCKET_ERROR) {
		printf("shutdown failed: %d\n", WSAGetLastError());
		closesocket(ConnectSocket);
		WSACleanup();
		return -1;
	}

	// cleanup
	closesocket(ConnectSocket);
	WSACleanup();

	return 0;
}

std::string SyncDevice::getName(void) {
	return dev_name;
}

void SyncDevice::PrintInfo(void) {
	std::cout << "SyncDevice Object" << std::endl;
	std::cout << "> IP Address: " << ip_address << std::endl;
	std::cout << "> Port: " << portstr << std::endl;
	return;
}

int SyncDevice::sendRecordCommand(void) {
	if (deviceInitialized) {
		iResult = send(ConnectSocket, recordCommand, recordCommandLength, 0); // not calling strlen() inline to minimize computation time on this call
		if (iResult == SOCKET_ERROR) {
			printf("sending record command failed: %d\n", WSAGetLastError());
			closesocket(ConnectSocket);
			WSACleanup();
			return 1;
		}
	}
	else {
		std::cout << "device not initialized." << std::endl;
		return -1;
	}
	return 0;
}

int SyncDevice::SendHyperDeckConfigCmd(const char* msg) {

	// send message
	//send(socket_fd, msg, strlen(msg), 0);
	iResult = send(ConnectSocket, msg, (int)strlen(msg), 0);
	if (iResult == SOCKET_ERROR) {
		printf("sending config command failed: %d\n", WSAGetLastError());
		closesocket(ConnectSocket);
		WSACleanup();
		return 1;
	}

	// Receive data until the server closes the connection
	//valread = read(socket_fd, buffer, 1024);
	iResult = recv(ConnectSocket, buffer, 1024, 0);	
	if (iResult < 6) {
		std::cout << "ERROR: not enough response chars received: <" << buffer << ">" << std::endl;
		return -1;
	}
	std::string response(buffer);
	if (response.substr(0, 6).compare("200 ok") != 0) {
		std::cout << "ERROR: bad response from device: " << buffer << std::endl;
		return -1;
	}
	memset(buffer, 0, 1024);  // reset buffer	
	return 0;
}

int SyncDevice::Init(void) {

	// make sure we're ready to connect
	if (!readyToConnect) {
		std::cout << "Not ready to connect! Provide valid parameters." << std::
			endl;
		return -1;
	}

	// set timeout
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;

	// Initialize Winsock
	iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (iResult != 0) {
		printf("WSAStartup failed: %d\n", iResult);
		return 1;
	}

	ZeroMemory(&hints, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	// Resolve the server address and port
	_itoa_s((int)port, portstr, 10);
	iResult = getaddrinfo(ip_address.c_str(), portstr, &hints, &result);
	if (iResult != 0) {
		printf("getaddrinfo failed: %d\n", iResult);
		WSACleanup();
		return 1;
	}

	// Attempt to connect to the first address returned by
	// the call to getaddrinfo
	ptr = result;

	// Create a SOCKET for connecting to server
	ConnectSocket = socket(ptr->ai_family, ptr->ai_socktype,
		ptr->ai_protocol);

	if (ConnectSocket == INVALID_SOCKET) {
		printf("Error at socket(): %ld\n", WSAGetLastError());
		freeaddrinfo(result);
		WSACleanup();
		return 1;
	}

	// set options
	setsockopt(ConnectSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(struct timeval));

	// Connect to server.
	iResult = connect(ConnectSocket, ptr->ai_addr, (int)ptr->ai_addrlen);
	if (iResult == SOCKET_ERROR) {
		closesocket(ConnectSocket);
		ConnectSocket = INVALID_SOCKET;
	}

	
	// Should really try the next address returned by getaddrinfo
	// if the connect call failed
	// But for this simple example we just free the resources
	// returned by getaddrinfo and print an error message
	freeaddrinfo(result);

	if (ConnectSocket == INVALID_SOCKET) {
		printf("Unable to connect to server!\n");
		WSACleanup();
		return 1;
	}

	// SOCKET CONNECTION IS CONFIGURED AND READY TO GO
	// NOW WE NEED TO CONFIGURE FOR THE SPECIFIC DEVICE TYPE
	switch (type) {
	case HyperDeck:
		// set record command for this device type
		strcpy_s(recordCommand, "record\n");
		recordCommandLength = strlen(recordCommand);

		// read anything in buffer from initial connection
		// for HyperDeck this will be system info
		//valread = read(socket_fd, buffer, 1024);
		// Receive data until the server closes the connection
		do {
			iResult = recv(ConnectSocket, buffer, 1024, 0);
		} while (iResult > 0);


		//printf("valread: %d response: <%s>\n",valread,buffer);
		memset(buffer, 0, 1024);  // reset buffer

		// send ping message
		if (SendHyperDeckConfigCmd("ping\n") != 0) {
			std::cout << "ERROR: failed ping." << std::endl;
			return -1;
		}

		// enable remote
		if (SendHyperDeckConfigCmd("remote: enable: true\n") != 0) {
			std::cout << "ERROR: QUITTING" << std::endl;
			return -1;
		}

		// select slot 1
		if (SendHyperDeckConfigCmd("slot select: slot id: 1\n") != 0) {
			std::cout << "ERROR: QUITTING" << std::endl;
			return -1;
		}
		break;

	case Kinematics:
		// set record command for this device type
		strcpy_s(recordCommand, "r\n");
		recordCommandLength = strlen(recordCommand);
		break;

	case Other:
		strcpy_s(recordCommand, "record\n");
		recordCommandLength = strlen(recordCommand);
		break;
	default:
		std::cout << "Undefined initialization behavior" << std::endl;
		return -1;
	}

	// done with initialization
	deviceInitialized = true;
	return 0;
}
