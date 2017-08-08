LIBSMM_PATH := ../../libsmm
CC = gcc
CFLAGS = -I ../../libsmm

test-mig-usr: test-mig-usr.o $(LIBSMM_PATH)/libsmm.a
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	rm test-mig-usr test-mig-usr.o
