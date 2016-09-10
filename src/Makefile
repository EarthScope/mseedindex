
# Build environment can be configured the following
# environment variables:
#   CC : Specify the C compiler to use
#   CFLAGS : Specify compiler options to use

BIN = mseedindex

SRCS = mseedindex.c md5.c ../sqlite/sqlite3.c
OBJS = $(SRCS:.c=.o)

# Required compiler parameters
REQCFLAGS = -I../libmseed -I../sqlite
REQLDFLAGS = -L../libmseed -I../sqlite

LDLIBS = -lmseed -lpq

# Specific defines for sqlite3
%sqlite3.o: TARGETCFLAGS = -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o ../$@ $(OBJS) $(REQLDFLAGS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJS) ../$(BIN)

# Implicit rule for building object files
%.o: %.c
	$(CC) $(CFLAGS) $(REQCFLAGS) $(TARGETCFLAGS) -c $< -o $@

install:
	@echo
	@echo "No install target, copy the executable and man page as needed"
	@echo
