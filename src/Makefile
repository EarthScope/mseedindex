
# Build environment can be configured the following
# environment variables:
#   CC : Specify the C compiler to use
#   CFLAGS : Specify compiler options to use

# Options specific for GCC
GCC = gcc
GCCFLAGS = -O2 -Wall -I../libmseed

# Required compiler parameters
REQCFLAGS = -I../libmseed

BIN = mseedsyncdb

LDFLAGS = -L../libmseed
LDLIBS = -lmseed -lpq

OBJS = $(BIN).o md5.o

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o ../$@ $(OBJS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJS) ../$(BIN)

cc:
	@$(MAKE) "CC=$(CC)" "CFLAGS=$(CFLAGS)"

debug:
	$(MAKE) "CFLAGS=-g $(CFLAGS)"

# Implicit rule for building object files
%.o: %.c
	$(CC) $(CFLAGS) $(REQCFLAGS) -c $<

install:
	@echo
	@echo "No install target, copy the executable(s) yourself"
	@echo
