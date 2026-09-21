CC     := gcc
CFLAGS := -g -I. -Wall -L.
RM     := rm -f

LIB := libksocket.a

$(LIB): ksocket.o
	ar rs $(LIB) ksocket.o

ksocket.o: ksocket.h ksocket.c
	$(CC) $(CFLAGS) -c ksocket.c -o ksocket.o

init: $(LIB) initksocket.c
	$(CC) $(CFLAGS) -o initk initksocket.c -lksocket -lpthread

user: $(LIB) user1.c user2.c
	$(CC) $(CFLAGS) -o u1 user1.c -lksocket
	$(CC) $(CFLAGS) -o u2 user2.c -lksocket

clean:
	-$(RM) ksocket.o initk u1 u2 received*.txt

deepclean: clean
	-$(RM) $(LIB)

.PHONY: init user clean deepclean