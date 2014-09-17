
# Build environment can be configured the following
# environment variables:
#   CC : Specify the C compiler to use
#   CFLAGS : Specify compiler options to use

BIN = mseedsyncdb

SRCS = mseedsyncdb.c md5.c
OBJS = $(SRCS:.c=.o)

# Required compiler parameters
REQCFLAGS = -I../libmseed

LDFLAGS = -L../libmseed
LDLIBS = -lmseed -lpq

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o ../$@ $(OBJS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJS) ../$(BIN)

# Implicit rule for building object files
%.o: %.c
	$(CC) $(CFLAGS) $(REQCFLAGS) -c $<

install:
	@echo
	@echo "No install target, copy the executable and man page as needed"
	@echo
