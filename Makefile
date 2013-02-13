BINARIES = cordubla test

all: $(BINARIES)

install: cordubla
	install -p -m 0755 $< /sbin/$<

clean:
	rm -f *.o $(BINARIES)

%: %.c
	$(CC) -std=c99 -static -g -O3 -D_GNU_SOURCE $(CPPFLAGS) -Wall $< -o $@ 
