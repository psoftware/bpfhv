#
# test code for the sched16 algorithm.

PROGS += sched
PROGS += udprecv

CFLAGS = -O3 -pipe -g
#CFLAGS = -O1 -pipe -g -fsanitize=address -fno-omit-frame-pointer
CFLAGS += -Werror -Wall -Wunused-function -Wunused-result
CFLAGS += -Wextra -I. -Iinclude
# sem_init etc do not compile under OS/X
CFLAGS += -Wno-deprecated-declarations
#CFLAGS += -DMY_MQ_LEN=400
# CFLAGS += -g -fsanitize=address # clang only

LDLIBS += -lpthread -lnetmap
ifeq ($(shell uname),Linux)
        LDLIBS += -lrt  # on linux
endif

#SRCS= main.c sess.c # dn_sched_rr.c # dn_sched_qfq.c # dn_sched_wf2q.c
SRCS= dn_sched_fifo.c dn_sched_rr.c dn_sched_qfq.c dn_sched_wf2q.c dn_heap.c test_dn_sched.c sched_main.c dn_cfg.c
SRCS+= main.c sess.c dn_cfg.c cqueue.c tsc.c
OBJS= $(SRCS:%.c=%.o)
CLEANFILES = $(PROGS) $(OBJS)

LDFLAGS += $(LDLIBS)

all: $(PROGS)

$(OBJS): sched16.h dn_test.h cqueue.h

sched: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

udprecv: udprecv.o tsc.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	-@rm -rf $(CLEANFILES)
