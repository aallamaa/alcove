parser: 
	$(CC) -Wall -W  -g3 -o parser  parser.c -lm 
speed:
	$(CC) -Wall -W  -O3 -o parser  parser.c -lm
# -Os removed
clean:
	rm -f parser
