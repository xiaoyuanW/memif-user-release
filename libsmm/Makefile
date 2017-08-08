Q	:= @
O	:= obj

LIB_CXX_SRC := 

LIB_C_SRC := measure.c \
            emif.c \
            clearcache.c \
            migqueue.c \
            migusr.c \
		    log.c		  

# xzl: XXX remove this file
		    
LIB_OBJS := $(patsubst %.cc, $(O)/%.o, $(LIB_CXX_SRC))
LIB_OBJS += $(patsubst %.c, $(O)/%.o, $(LIB_C_SRC))

TARGET=libsmm.a

#EXTRACFLAGS += -DHAS_CMEM=1
#EXTRACFLAGS += -DHAS_DMA -g -O2 
#EXTRACFLAGS += -march=armv7-a	# this is for dmb(). hurts STREAM performance

#NUMALIB=-lticmem -ledma3 -ledma3rm

CFLAGS += $(EXTRACFLAGS) -Wall 
#CFLAGS += -std=c99
#CFLAGS += -std=c1x
#CFLAGS += -DNDEBUG				# disable asssertion -- does not matter much 

$(O)/%.o: %.cc 
	$(Q)mkdir -p $(@D)
	$(Q)echo "CXX	$<"
	$(Q)$(CC) $(DEPCFLAGS) $(CFLAGS) -o $@ -c $<

$(O)/%.o: %.c 
	$(Q)mkdir -p $(@D)
	$(Q)echo "CC	$<"
	$(Q)$(CC) $(DEPCFLAGS) $(CFLAGS) -o $@ -c $<
		
$(TARGET): $(LIB_OBJS) 
	$(Q)mkdir -p $(@D)
	$(Q)echo "AR	$@"
	$(Q)$(AR) r $@ $(LIB_OBJS)

clean:
	rm -f obj/*.o $(TARGET) *.html
