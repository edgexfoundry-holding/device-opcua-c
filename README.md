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
* A version of GCC supporting C11.
* CMake version 3.1 or greater and make.
* The EdgeX Device SDK for C, version 1.x.
* The opensource library, open62541, version 0.3.1.
* An OPC-UA server.

## Building the open62541 Library

This step is not required if building the device service as a Docker container.

The following steps can be used to build and install the opensource open62541
library.  Installation of the open62541 library may require `sudo` privileges.

Retrieve v0.3.1 of the open62541 library from the official archive at
`https://github.com/open62541/open62541`.  This archive will come in the form
of a zipfile which should be unzipped into a suitable location.

Once the open62541 codebase has been unzipped, the following commands may be run
from the top level of the library (open62541-0.3.1):
```
   mkdir build
   cd build
   cmake .. -DBUILD_SHARED_LIBS=ON -DUA_ENABLE_AMALGAMATION=ON
   make
   sudo make install
```

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
variable CSDK_DIR.

After having built the device service, the executable can be
found at ./build/{debug,release}/device-opcua-c/c/device-opcua-c.

## Running the Device Service

With no options specified the service runs with a name of "device-opcua", the
default configuration profile, no registry and a configuration directory of
res/.
These settings may be changed on the command line as follows:

```
   -n, --name <name>          : Set the device service name
   -r, --registry <url>       : Use the registry service
   -p, --profile <name>       : Set the profile name
   -c, --confdir <dir>        : Set the configuration directory
```

## Building with Docker

To build a Docker image of the device service, run the following command
from the OPC-UA device service directory (device-opcua-c):

`docker build --no-cache -t device-opcua --file scripts/Dockerfile.alpine-3.9 .`

Using this method of building the device service, there is no need to build
the C-SDK or open62541 library separately; these will be automatically built as
part of the docker build process.

Once you have built a Docker image of the device service, enter the command
below to run the Docker image within a Docker container.

`docker run --rm --name edgex-device-opc-ua --network=<docker-network> device-opcua`

where `<docker-network>` is the name of the currently running Docker network,
which the container is to be attached to.

## Supported Data Types

Below is a table detailing which OPC-UA data types are supported and which
EdgeX data type each corresponds to:

| OPC-UA Data type | EdgeX Data type |
|:----------------:|:---------------:|
| UA_Boolean       | Bool            |
| UA_SByte         | Int8            |
| UA_Byte          | Uint8           |
| UA_Int16         | Int16           |
| UA_UInt16        | Uint16          |
| UA_Int32         | Int32           |
| UA_UInt32        | Uint32          |
| UA_Int64         | Int64           |
| UA_UInt64        | Uint64          |
| UA_Float         | Float32         |
| UA_Double        | Float64         |
| UA_String        | String          |
| UA_DateTime      | Int64           |

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

Each deviceResource within the Device Profile corresponds to a specific OPC-UA
node.  Below is an example of two deviceResources.

```yaml
# Example deviceResource
- name: StringStatic
  description: "A static string variable"
  attributes:
    { nodeID: "String" , nsIndex: "3", IDType: "STRING" }
  properties:
    value:
      { type: "String", readWrite: "RW", defaultValue: "" }
    units:
      { type: "String", readWrite: "R", defaultValue: "Integer" }

- name: ServerState
  description: "The Server State"
  attributes:
    { nodeID: "2259" , nsIndex: "0", IDType: "NUMERIC" }
  properties:
    value:
      { type: "Int32", readWrite: "R" }
    units:
      { type: "String", readWrite: "R", defaultValue: "Int32" }
```

There are three required attributes which must be specified for each
deviceResource; nsIndex, nodeID, and IDType.  These attributes have the
following meanings, and must match the values assigned to the corresponding
node on the OPC-UA server:

```
   nsIndex      : The namespace of the node we are interested in.
   nodeID       : The name of the node we are interested in.
   IDType       : Represents the type of nodeID, and must be one of the following: {NUMERIC, STRING, BYTESTRING, GUID}
```

An example profile can be found in `example-config/ProSysSimulator.yaml`.

#### Subscribe Configuration
The OPC-UA device service provides support for monitoring certain nodes
within a remote OPC-UA server.  OPC-UA subscriptions are used to achieve this.

Subscriptions are setup whenever a newly created connection to the remote
OPC-UA server is made.  Each connection will set up a distinct Subscription
Item, which can contain one or more Monitored Items.  Whenever one of these
Monitored Items is changed on the server, the server is responsible for
notifying all subscribed device services about this change.

If the device service is notified of a change in value of a monitored item,
the updated value is automatically posted back to EdgeX.

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
          { type: "Uint32", readWrite: "R" }
      units:
          { type: "String", readWrite: "R", defaultValue: "String" }
```

### Example Configuration
This example makes use of the Prosys OPC-UA Simulation Server which can be
downloaded from `https://www.prosysopc.com/products/opc-ua-simulation-server/`.
For simplicity, it is assumed that this simulation server will be correctly
configured and running on the same host machine as the OPC-UA device service.

The files located in the `example-config` directory can be used to create a
device service which will connect to a remote simulation server.

The example device service configuration and profile files should first be
copied into the `res` directory.

```
   mv res/configuration.toml res/configuration.toml.backup
   cp example-config/configuration.toml res/configuration.toml
   cp example-config/ProSysSimulator.yaml res/ProSysSimulator.yaml
```

Build the device service.

`docker build --no-cache -t device-opcua --file scripts/Dockerfile.alpine-3.9 .`

Ensure there is a running EdgeX docker network.

Run the OPC-UA device service.

`docker run --rm --name edgex-device-opc-ua --network=<docker-network> device-opcua`

where `<docker-network>` is the name of the currently running Docker network,
which the container is to be attached to.

Verify that the example OPC-UA device has been added to the network.

`curl -s http://edgex-core-command:48082/api/v1/device`

Take note of the `id` which has been assigned to the example device.  The
list of available commands for this device can also be displayed.

`curl -s http://edgex-core-command:48082/api/v1/device/<deviceId>`

where `<deviceId>` is the id displayed by the previous `curl` command.  Take
note of the `id` value which has been assigned to the `StringStatic` attribute.

Run the simulation server, ensuring that the endpoint representing the
simulation server has been set to
`opc.tcp://<host-ip-address>:53530/OPCUA/SimulationServer`, where
`<host-ip-address>` is the IP address of the machine running the server.

A connection to the server can now be made and GET/PUT requests issued. So,
for example, to request a GET of the `StringStatic` attribute (and implicitly
connect to the simulation server), the following command could be issued:

`curl -s edgex-core-command:48082/api/v1/device/<deviceId>/command/<commandId>`

where `<deviceId>` and `<commandId>` are the values noted previously.

This shoudl give output similar to the following:

`{"device":"Prosys OPC-UA Simulation Server","origin":1571659730682,"readings":[{"name":"StringStatic","value":"TestString","origin":1571659730682}]}`
