parser: 
	$(CC) -Wall -W  -g3 -o parser  parser.c
speed:
	$(CC) -Wall -W  -O3 -o parser  parser.c
# -Os removed
clean:
	rm -f parser
