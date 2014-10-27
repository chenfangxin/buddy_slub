# CC=gcc
CC=clang
CFLAGS=-g -Wall
OBJS=rte_buddy.o rte_slub.o rte_mem.o
HEADERS=rte_list.h rte_slub.h rte_buddy.h rte_spinlock.h

all: root
root: root.o $(OBJS)
	$(CC) -o $@ $^

root.o: root.c
	$(CC) $(CFLAGS) -c $<

rte_buddy.o: rte_buddy.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

rte_slub.o: rte_slub.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

rte_mem.o: rte_mem.c $(HEADERS)
	$(CC) $(CFLAGS) -c $<

clean:
	rm -rf *.o
	rm -rf root
