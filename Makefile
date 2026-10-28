.RECIPEPREFIX = >
CC      := gcc
BASE    := -std=gnu11 -Wall -Wextra -Werror -Iinclude -MMD -MP
LIBS    := -pthread -lcrypt
MODE    ?= dev

ifeq ($(MODE),dev)
  CFLAGS := $(BASE) -g -O0
else ifeq ($(MODE),asan)
  CFLAGS := $(BASE) -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
else ifeq ($(MODE),tsan)
  CFLAGS := $(BASE) -g -O1 -fsanitize=thread
else ifeq ($(MODE),release)
  CFLAGS := $(BASE) -O2 -DNDEBUG
endif
LDFLAGS := $(filter -fsanitize=%,$(CFLAGS))

BLD      := build/$(MODE)
SRCS     := $(wildcard src/*.c)
SRV_OBJS := $(patsubst src/%.c,$(BLD)/%.o,$(filter-out src/client.c,$(SRCS)))
CLI_OBJS := $(BLD)/client.o

all: $(BLD)/server $(BLD)/client

$(BLD)/server: $(SRV_OBJS)
> $(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BLD)/client: $(CLI_OBJS)
> $(CC) $(LDFLAGS) -o $@ $^

$(BLD)/%.o: src/%.c | $(BLD)
> $(CC) $(CFLAGS) -c $< -o $@

$(BLD):
> mkdir -p $@

asan:
> $(MAKE) MODE=asan
tsan:
> $(MAKE) MODE=tsan
release:
> $(MAKE) MODE=release
clean:
> rm -rf build

-include $(wildcard $(BLD)/*.d)
.PHONY: all asan tsan release clean

LIBOBJS := $(BLD)/log.o $(BLD)/lock.o $(BLD)/storage.o $(BLD)/journal.o $(BLD)/ledger.o $(BLD)/auth.o $(BLD)/loans.o

$(BLD)/race_test: tests/race_test.c $(LIBOBJS) | $(BLD)
> $(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter-out %.h,$^) $(LIBS)

$(BLD)/ledger_test: tests/ledger_test.c $(LIBOBJS) | $(BLD)
> $(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter-out %.h,$^) $(LIBS)

$(BLD)/loan_test: tests/loan_test.c $(LIBOBJS) | $(BLD)
> $(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter-out %.h,$^) $(LIBS)

$(BLD)/user_test: tests/user_test.c $(LIBOBJS) | $(BLD)
> $(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter-out %.h,$^) $(LIBS)

test: $(BLD)/race_test $(BLD)/ledger_test $(BLD)/loan_test $(BLD)/user_test
> ./$(BLD)/race_test lock
> ./$(BLD)/ledger_test
> ./$(BLD)/loan_test
> ./$(BLD)/user_test

.PHONY: test

$(BLD)/net_test: tests/net_test.c | $(BLD)
> $(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< -pthread

nettest: $(BLD)/net_test
> ./$(BLD)/net_test

.PHONY: nettest