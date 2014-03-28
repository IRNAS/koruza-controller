.PHONY: libucl

all: koruza-control

koruza-control: main.o server.o client.o controller.o collector.o util.o libucl
	$(CC) $(LDFLAGS) -o $@ main.o server.o client.o controller.o collector.o util.o libucl/.obj/*.o -lrt -levent -lz

libucl:
	$(MAKE) -C libucl -f Makefile.unix

%.o: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c -I. -Ilibucl/include -o $@ $<

clean:
	$(MAKE) -C libucl -f Makefile.unix clean
	rm -rf *.o koruza-control

