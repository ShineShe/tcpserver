PROG        = mongoose
CFLAGS      = -std=c99 -O2 -W -Wall -pedantic -pthread


linux:
	$(CC) mongoose.c main.c -o $(PROG) -ldl $(CFLAGS)


