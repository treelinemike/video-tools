// Simple command-line program to send TCP packets over LAN
// to synchronize data collection devices
//
// Author: Mike Kokko
// Updated: 09-Sep-2024

// following: https://learn.microsoft.com/en-us/windows/win32/winsock/winsock-client-application

#include "SyncDevice.h"
#include <cxxopts.hpp>  		// https://www.github.com/jarro2783/cxxopts
#include <yaml-cpp/yaml.h>
#include <iostream>




// main
int main(int argc, char const* argv[])
{

	// variables
	cxxopts::Options options("sync_start", "LAN-based device synchronization");
	YAML::Node config;
	std::string yamlfile_name, device_name, device_type_str, device_ip;
	unsigned int device_port;
	bool manual_start_flag, device_active;
	std::vector<SyncDevice> devices;
	syncDeviceType device_type;
	int retval;

	// PARSE COMMAND LINE OPTIONS
	try
	{
		options.add_options()
			("y,yamlconfig", "name of config YAML file (REQUIRED)", cxxopts::value<std::string>());
		auto cxxopts_result = options.parse(argc, argv);
		if (cxxopts_result.count("yamlconfig") != 1)
		{
			std::cout << options.help() << std::endl;
			return -1;
		}
		yamlfile_name = cxxopts_result["yamlconfig"].as<std::string>();
	}
	catch (const cxxopts::exceptions::exception& e)
	{
		std::cout << "Error parsing options: " << e.what() << std::endl;
		return -1;
	}

	// LOAD CONFIGURATION FILE
	try {
		// get input file
		config = YAML::LoadFile(yamlfile_name);

		// get manual start flag
		if (config["manual_start"]) {
			manual_start_flag = config["manual_start"].as<bool>();
		}
		else {
			manual_start_flag = true;
			std::cout << "YAML: no 'manual_start' key in file, assuming manual start" << std::endl;
		}

		// parse each clip definition
		if (!config["devices"]) {
			std::cout << "ERROR: no 'devices' key found in YAML config file" << std::endl;
			return -1;
		}
		YAML::Node yamldevices = config["devices"];
		for (YAML::const_iterator it = yamldevices.begin(); it != yamldevices.end(); ++it) {
			device_name = it->first.as<std::string>();

			// get the YAML node for this device description
			YAML::Node device_details = it->second;

			// skip this device if we're not intersted in synchronizing it (i.e. if active == false)
			device_active = device_details["active"].as<bool>();
			if (!device_active) {
				continue;
			}

			// extract device info
			// TODO: add error checking
			device_type_str = device_details["type"].as<std::string>();
			device_ip = device_details["ip"].as<std::string>();
			device_port = device_details["port"].as<unsigned int>();

			// map device_type_str from std::string to enum
			// TODO: is there a better way to bind enum?
			if (device_type_str.compare("HyperDeck") == 0) {
				device_type = HyperDeck;
			}
			else if (device_type_str.compare("Kinematics") == 0) {
				device_type = Kinematics;
			}
			else if (device_type_str.compare("Other") == 0) {
				device_type = Other;
			}
			else {
				std::cout << "Invalid device type: " << device_type_str << std::endl;
				return -1;
			}

			// create a device
			std::cout << "Adding " << device_ip << ":" << device_port << " as " << device_name << std::endl;
			SyncDevice single_device(device_name, device_ip, device_port, device_type); // on stack (not heap), will be automatically freed 
			devices.push_back(single_device); // object is *copied* into vector
		}

		// make sure we have some clip definitions in our vector
		if (!devices.size()) {
			std::cout << "ERROR: no valid devices found in YAML config file" << std::endl;
			return -1;
		}
	}
	catch (const YAML::Exception& e)
	{
		std::cout << "ERROR PARSING YAML CONFIGURATION: " << e.what() << std::endl;
		return -1;
	}


	// initialize each device
	std::cout << std::flush;
	for (auto& it : devices) {
		std::cout << "Initializing: " << it.getName() << "... " << std::flush;
		retval = it.Init();
		std::cout << "Device initialized" << std::endl;
		if (retval) {
			std::cout << "Error initializing " << it.getName() << std::endl;
			return -1;
		}
		std::cout << " done." << std::endl;
	}

	// wait for keypress if desired
	if (manual_start_flag) {
		std::cout << "Press ENTER to synchronize devices" << std::endl;
		std::cin.get();
	}

	// send sync pulse(s)
	for (auto& it : devices) {
		if (it.sendRecordCommand() != 0) {
			std::cout << "Error starting: " << it.getName() << std::endl;
			return -1;
		}
	}

	// close sockets
	for (auto& it : devices) {
		it.Close();
	}

	// done
	std::cout << "done" << std::endl;
	return 0;
}