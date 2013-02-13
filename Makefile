all: cordubla test

clean:
	rm -f *.o cordubla

%: %.c
	$(CC) -std=c99 -static -g -O3 -D_GNU_SOURCE $(CPPFLAGS) -Wall $< -o $@ 
