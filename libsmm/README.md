# *memif*

### Credits

* Userspace EDMA3 driver/test -- adopts code from TI's edma3-lld.
  * See TI's MCSDK edma3_lld_02_11_11_15

* test/word; primes.C -- adopts code from Psearchy, which is
	"Derived from a sequential version of searchy (Jinyang Li's search engine)"

* STREAM -- adopts code from the STREAM benchmark

* threadpool -- by Mathias Brossard.

### Build environment

* Native: TI mcsdk-3_00_04_18, which comes with GCC 4.7.2

* Host (for cross build):
    gcc version 4.7.3 20130226 (prerelease) (crosstool-NG linaro-1.13.1-4.7-2013.03-20130313 - Linaro GCC 2013.03)

### User code (for native compilation)

To build, on ks2 do:

    make -j4

* migqueue.c mig.h
  The lockfree queue implementation. Shared between user and kernel.

* migusr.c
  The userspace wrapper for the memif interface, which implements the user API.

* measure.[c|h] log-xzl.h
  Measurement and debugging facility. Can be compiled for both user and kernel.

* edma3.c edma3-xzl.h
  The wrapper for TI's userspace edma3 driver.

Also see test/ for the test code. test/scripts/ contains a useful test script.

### Off-tree kernel code (cross compilation)

* kernel/

**dependency**: the modified kernel tree, HEAD of branch: numa-highmem1.

To build the code, on host machine:

    source env-xzl.sh # you may want to change the kernel source path

* migif.c
  The interface test code. Built as migif.ko.

* migrate-ks2.c
  The kernel mechanism for migration. Built as migks2.ko

Both modules are needed at run time.

### In-tree kernel code (cross compilation)

To build, on host machine:

    source env-xzl.sh
    ./build.sh
    ./install.sh

* test-edma3.c
  The in-kernel test code for edma3 driver. Used to be useful but seems broken now.
  Can repair if needed.
  Can be used as example of in-kernel test code.

* edma3-drv.c
  The core edma3 driver.

* emda3-dmaengine.c
  The dma engine code for edma3.

### Changelog

* 2015.12 Initial public release.









