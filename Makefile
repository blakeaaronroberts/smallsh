.PHONY: debug release prep all clean
CC := gcc
CFLAGS := -std=c99

OBJ := smallsh.o
EXE := smallsh

DBGDIR := debug
DBGEXE := $(DBGDIR)/$(EXE)
DBGOBJ := $(addprefix $(DBGDIR)/, $(OBJ))
debug: CFLAGS += -DDEBUG -g

RELDIR := release
RELEXE := $(RELDIR)/$(EXE)
RELOBJ := $(addprefix $(RELDIR)/, $(OBJ))
release: CFLAGS += -O3

all: clean debug release copy

clean:
	rm -rf debug/ release/ ./$(EXE)

prep:
	@mkdir -p $(DBGDIR) $(RELDIR)

debug: prep $(DBGEXE)

$(DBGEXE): $(DBGOBJ)
	$(CC) ${CPPFLAGS} ${CFLAGS} -o $@ $^

$(DBGDIR)/%.so: %.c
	$(CC) ${CPPFLAGS} ${CFLAGS} -shared -fPIC -c -o $@ $^

$(DBGDIR)/%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $^

release: prep $(RELEXE)

$(RELEXE): $(RELOBJ)
	$(CC) ${CPPFLAGS} ${CFLAGS} -o $@ $^

$(RELDIR)/%.so: %.c
	$(CC) ${CPPFLAGS} ${CFLAGS} -shared -fPIC -c -o $@ $^

$(RELDIR)/%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $^

copy:
	cp release/smallsh smallsh
