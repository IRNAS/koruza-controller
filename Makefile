.PHONY: libucl

all: koruza-control

koruza-control: main.o libucl
	$(CC) $(LDFLAGS) -o $@ main.o libucl/.obj/*.o -lrt -levent

libucl:
	$(MAKE) -C libucl -f Makefile.unix

%.o: %.c
	$(CC) $(CFLAGS) $(EXTRA_CFLAGS) -c -I. -Ilibucl/include -o $@ $<

clean:
	$(MAKE) -C libucl -f Makefile.unix clean
	rm -rf *.o koruza-control

