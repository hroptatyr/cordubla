all: cordubla test

install: cordubla
	install -p -m 0755 $< /sbin/$<

clean:
	rm -f *.o cordubla

%: %.c
	$(CC) -std=c99 -static -g -O3 -D_GNU_SOURCE $(CPPFLAGS) -Wall $< -o $@ 
