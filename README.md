## device-opcua-c

## Overview

Device service for OPC-UA protocol written in C.

## Features
* Unencrypted, synchronous connections to OPC-UA servers.
* Read from an OPC-UA node.
* Write to an OPC-UA node.
* Subscribe to a set of OPC-UA nodes.

## Prerequisites

* A Linux build host.
* A version of GCC supporting C99.
* CMake version 3.1 or greater and make.
* Development libraries and headers for libmicrohttpd, curl, yaml, libuuid, libcbor, mbedtls, wget, python2, and py-six.
* An OPC-UA server.

## Building the Device Service

Before building the OPC-UA device service, please ensure
that you have the EdgeX C-SDK installed and make sure that
the current directory is the OPC-UA device service directory
(device-opcua-c). To build the OPC-UA device service, enter
the command below into the command line to run the build
script.

	./scripts/build.sh

In the event that your C-SDK is not installed in the system
default paths, you may specify its location using the environment
variable CSDK_DIR

After having built the device service, the executable can be
found at ./build/{debug,release}/device-opcua-c/c/device-opcua-c.

## Building with Docker

To build a Docker image of the device service, run the following command
from the OPC-UA device service directory (device-opcua-c):

`docker build --no-cache -t device-opcua --file scripts/Dockerfile.alpine-3.9 .`

Once you have built a Docker image of the device service, enter the command
below to run the Docker image within a Docker container.

`docker run --rm --name edgex-device-opc-ua --network=<docker-network> device-opcua`

where `<docker-network>` is the name of the currently running Docker network,
which the container is to be attached to.

## Device Service Configuration
### Adding a Device
To add a new OPC-UA device to the device service, insert the layout below into
the `configuration.toml` file which is located in the `res` folder.  Update the
Address, Port, and Path entries to match the device you wish to add.
```toml
[[DeviceList]]
  Name = "Simulation Server"
  Profile = "Simulation Server - Profile"
  Description = "An OPCUA device"
  [DeviceList.Protocols]
    [DeviceList.Protocols.OPC-UA]
      Address = "172.17.0.1"
      Port = 53530
      Path = "/OPCUA/SimulationServer"
```

An example device service configuration, including a pre-defined device, can be
found in `example-config/configuration.toml`.

### Device Profile

A Device Profile provides a template for an OPC-UA device, consisting of a
number of deviceResources, deviceCommands, and coreCommands.

An example profile can be found in `example-config/ExampleProfile.yaml`.

#### Subscribe Configuration
The OPC-UA device service provides support for monitoring certain nodes
within a remote OPC-UA server.  OPC-UA subscriptions are used to achieve this.

Subscriptions are setup whenever a newly created connection to the remote
OPC-UA server is made.  Each connection will set up a distinct Subscription
Item, which can contain one or more Monitored Items.  Whenever one of these
Monitored Items is changed on the server, the server is responsible for
notifying all subscribed device services about this change.

In order to configure a specific deviceResource as a Monitored Item, the
`monitored` attribute should be set to "True" within the device profile.
```yaml
# Subscription example
- name: Counter1
  description: "A Simulated Counter"
  attributes:
    { nodeID: "Counter1" , nsIndex: "5", IDType: "STRING", monitored: "True" }
  properties:
      value:
          { type: "Uint32", readWrite: "R", defaultValue: "0" }
      units:
          { type: "String", readWrite: "R", defaultValue: "String" }
```
