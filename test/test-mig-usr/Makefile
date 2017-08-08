LIBSMM_PATH := ../../libsmm
CC = gcc
CFLAGS = -I ../../libsmm
LIBS := -pthread -lnuma

test-mig: test-mig.o $(LIBSMM_PATH)/libsmm.a
	$(CC) -o $@ $^ $(LIBS) $(CFLAGS)

clean:
	rm test-mig test-mig.o
