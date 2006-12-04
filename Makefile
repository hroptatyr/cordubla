all: codu test

clean:
	rm -f *.o codu

%: %.c
	$(CC) -std=c99 -g -O3 -D_GNU_SOURCE -Wall $< -o $@ -lc 
