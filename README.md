# BSide ESR02 firmware

This repository has been created to maintain the firmware configuration of the BSide ESR02 as the ComponentTester project versions change.

**This is not an official repository**: I created this repository to make life easier for people (and myself) wishing to update the firmware
of their device by centralizing the information for this particular model in this repository.

The source of this repository is: https://github.com/madires/Transistortester-Warehouse

![BSide ESR02 picture](bside_esr02.png)

## Instructions

### 1. Get the binaries

### From the downloaded firmware

You can download directly the firmware files from the releases from the Github project page.

### By compiling yourself the sources

For GNU/Linux and specifically on an Debian related OS, you need to install the following packages:

- gcc-avr
- binutils-avr
- avr-libc
- gdb-avr

Then, go to the `firmware` directory and enter the command: `make`.

### 2. Upload the binaries to the target

Make sure you have the package `avrdude`, then you can use the command `make upload`.

Note: By default, the `make upload` command try to use the avrispmkII programmer, if you want to use another one,
you have to edit the `Makefile` and change the ISP Programmer.
