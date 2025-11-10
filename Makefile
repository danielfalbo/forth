all: forth

forth: forth.c
	gcc -o forth forth.c -Wall -W -O2

clean:
	rm forth
