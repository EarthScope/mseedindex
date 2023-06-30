
# Build environment can be configured the following
# environment variables:
#   CC : Specify the C compiler to use
#   CFLAGS : Specify compiler options to use

BIN = mseedindex

SRCS = mseedindex.c md5.c sha256.c ../sqlite/sqlite3.c
OBJS = $(SRCS:.c=.o)

# Required compiler parameters
EXTRACFLAGS = -I../libmseed -I../sqlite
EXTRALDFLAGS = -L../libmseed -I../sqlite

ifdef WITHPOSTGRESQL
EXTRACFLAGS += -DWITHPOSTGRESQL
LDLIBS = -lmseed -lpq
else
LDLIBS = -lmseed
endif

# Specific defines for sqlite3
%sqlite3.o: EXTRACFLAGS += -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DHAVE_USLEEP=1

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o ../$@ $(OBJS) $(EXTRALDFLAGS) $(LDLIBS) $(LDFLAGS)

clean:
	rm -f $(OBJS) ../$(BIN)

# Implicit rule for building object files
%.o: %.c
	$(CC) $(CFLAGS) $(EXTRACFLAGS) -c $< -o $@

install:
	@echo
	@echo "No install target, copy the executable and documentation as needed"
	@echo
