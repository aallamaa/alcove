parser: 
	$(CC) -Wall -W  -lm -g3 -o parser  parser.c
speed:
	$(CC) -Wall -W  -lm -O3 -o parser  parser.c
# -Os removed
clean:
	rm -f parser
