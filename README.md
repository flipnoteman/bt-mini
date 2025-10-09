# BT-Mini
A WIP small scale P2P file share.
## Usage
Currently have a server that is mostly functional, and client that is just setup to test the server.
Build and start the server like so:
```bash
sudo ./tracker
```
The server accept incoming connections, and you must supply an infohash (this is what will be used to determine who has what file. Its basically going to be a hash of the file we want).
The server currently stores every person that has connected, saving information about what infohash they have, what their ip and port are. This allows us to make a client that checks the server for an infohash and it will return every client that contains that infohash. We can then break up our file transfer among those peers as we see fit with the client, and then the server is no longer needed after that (peer 2 peer baby!). 

Right now if you want to test it you can do so like this:
```bash
# ./bt_mini hostname port infohash client_name local_port command
./bt_mini 127.0.0.1 8080 deadbeef clientA 6881 started

# You can stop a client from showing up in the tracker server like so
./bt_mini 127.0.0.1 8080 deadbeef clientA 6881 stopped
```
Additionally, clients that have been active on the tracker server for more than 120 seconds without contacting it get purged. We can change this.

## Building
### Dependencies
- You must have the boost.asio library installed at least.
- You must also have cmake, make, and a compatible c compiler
- You can install most of this like this:
```bash
sudo apt update
sudo apt install libboost-all-dev make cmake
```

### Building and running
```bash
# If you just want to build you can use the build.sh script
./build.sh

# If you want to build AND then run you can use the build_and_run.sh script
./build_run.sh
```
