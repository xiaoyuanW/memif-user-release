LIBSMM_PATH := ../

# Dependency.
# ticmem, edma3, edma3rm are TI's libraries. They should come with TI's
# MCSDK installation.  
#
# the cmem (contiguous memory manager) is only needed by some
# functions.
LIBS := \
-lticmem -ledma3 -ledma3rm -pthread
	
CC = gcc
CFLAGS = -O2 -g -std=c99 -Wall -I../ \
-MD -MP \
-I/home/root/xzl/numactl/numactl-ks2
#-march=armv7-a \

CFLAGS_O3 = -O3 -g -std=c99 -Wall -I../ \
-MD -MP \
-I/home/root/xzl/numactl/numactl-ks2

# main targets
LDFLAGS = $(LIBS)
EXEC = test test-emif test-mig stream stream_omp

# optional targets
ifeq ("$(wildcard $(runtime.c))","")
EXEC += runtime
endif

ifeq ("$(wildcard $(matrix.numa.c))","")
EXEC += matrix.numa
endif

all: $(EXEC) 

test.o: main.c
	$(CC) $(CFLAGS) -c $^ -o $@

test: test.o $(LIBSMM_PATH)/libsmm.a
	$(CC) $(LDFLAGS) $^ -o $@
		
test-emif: test-emif.o $(LIBSMM_PATH)/libsmm.a
	$(CC) $(LDFLAGS) -DUNITTEST $^ -o $@

test-mig: test-mig.o $(LIBSMM_PATH)/libsmm.a
	$(CC) $(LDFLAGS) -static -L/home/root/xzl/numactl/numactl-ks2/.libs $^ -lnuma -o $@

stream: stream.o $(LIBSMM_PATH)/libsmm.a word.o primes.o
	$(CC) -static -L/home/root/xzl/numactl/numactl-ks2/.libs $^ -lnuma $(LIBS) -o $@

stream_omp: stream_omp.o $(LIBSMM_PATH)/libsmm.a word.o primes.o
	$(CC) $(LDFLAGS) -L/home/root/xzl/numactl/numactl-ks2/.libs $^ -lnuma -lgomp -o $@
	
stream_omp.o: stream.c
	$(CC) $(CFLAGS) -fopenmp -c $^ -o $@
	
matrix.numa.o: matrix.numa.c
	$(CC) -O3 -g -std=c99 -Wall -fopenmp -I../ -I/home/root/xzl/numactl/numactl-ks2 -c $^ -o $@
			
primes.o: primes.C
	$(CC) -O3 -Wall -c $^ -o $@
	
word.o: word.c
	$(CC) -I../ -O3 -g -Wall -c $^ -o $@

word_omp.o: word.c
	$(CC) -I../ -O3 -g -Wall -fopenmp -c $^ -o $@
				
# unitest -- word counting program
word_test: word_omp.o primes.o  $(LIBSMM_PATH)/libsmm.a
	$(CC) -I../ -O3 -g -Wall -fopenmp -lgomp -DUNITTEST $^ -o $@
		
# cannot use -static			
matrix.numa: matrix.numa.o $(LIBSMM_PATH)/libsmm.a
	$(CC) $(LDFLAGS) -L/home/root/xzl/numactl/numactl-ks2/.libs $^ -lnuma -lgomp -o $@
	
runtime_omp.o: runtime.c
	$(CC) $(CFLAGS_O3) -fopenmp -c $^ -o $@
		
runtime: runtime_omp.o $(LIBSMM_PATH)/libsmm.a $(LIBSMM_PATH)/threadpool/libthreadpool.a
	$(CC) $(LDFLAGS) -L/home/root/xzl/numactl/numactl-ks2/.libs $^ -lnuma -lgomp -o $@
	
# primes.o takes long time to build. don't auto clean it.	
clean:	
	rm -f $(filter-out primes.o,$(wildcard *.o)) $(EXEC) *.d

-include test-mig.d
