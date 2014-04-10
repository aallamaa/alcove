parser: 
	$(CC) -Wall -W  -g3 -o alcove  alcove.c -lm 
speed:
	$(CC) -Wall -W  -O3 -o alcove  alcove.c -lm
# -Os removed
clean:
	rm -f alcove
