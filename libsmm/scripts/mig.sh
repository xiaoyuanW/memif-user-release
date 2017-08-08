#
# code snippets for testing memif
# not for execution directly.
#
#

# NUMA libs. Make sure to build them first.
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/home/root/xzl/numactl/numactl-ks2/.libs

# suppress printk
echo 1 > /proc/sys/kernel/printk
echo 6 > /proc/sys/kernel/printk

######################################
# kernel-space test driver
# 12/2015: test-edma3 driver seems broken. Can fix if needed.
######################################
cd /home/root/xzl
insmod edma3-drv.ko
insmod virt-dma.ko
insmod edma3-dmaengine.ko
insmod test-edma3.ko


######################################
#  test vanilla migration speed
######################################
~/xzl/numactl/numactl-ks2/migspeed -p 32 0 1
~/xzl/numactl/numactl-ks2/migspeed -p 1 0 1

######################################
# run userspace test program
######################################
# reload kernel module
rmmod migks2.ko
#rmmod edma3-dmaengine.ko
insmod ~/xzl/virt-dma.ko
insmod ~/xzl/edma3-drv.ko
insmod ~/xzl/edma3-dmaengine.ko
insmod ~/xzl/migks2.ko
cd ~/xzl/libsmm/ && make
cd ~/xzl/libsmm/test && make

./stream	# instrumented STREAM benchmark. also has _omp version
#./runtime
#./test-mig	# userspace micro benchmarks

######################################
# stress test: run test-mig forever...
#   make sure kernel recycles every page
######################################
while true;do ./test-mig; done;

rmmod migks2.ko && insmod ~/xzl/migks2.ko
cd ~/xzl/libsmm/test && make
./test-mig

