# BT-Mini
A WIP small scale P2P file share.
## Usage
Currently have a server that is mostly functional.
Build and start the server like so:

```bash
sudo ./build.sh
sudo ./tracker
```

You can generate a torrent file (for testing) like so:
```
sudo ./bt_mini -g <path/to/file>
```

Right now if you want to test it you can do so like this:
```bash
sudo ./bt_mini
```
This will open the nice TUI I have designed. If you navigate to the second tab, <F2>, then you can see what files the client has picked up on and open the file picker.
Basically, every <period> milliseconds, the client reaches out to the server and tells the server what files it wants to advertise.
By default this is 30000 ms. 

You can change options in the third tab as well.

## Building
### Dependencies
- You must have the boost.asio library installed at least.
- You must also have cmake, make, and a compatible c compiler
- You can install most of this like this:
```bash
sudo apt update
sudo apt install libboost-all-dev make cmake openssl
```

### Building and running
```bash
# If you just want to build you can use the build.sh script
./build.sh

# If you want to build AND then run you can use the build_and_run.sh script
./build_run.sh
```
