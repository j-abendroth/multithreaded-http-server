#------------------------------------------------------------------------------
#  Makefile for HTTP Server
#
#  make                     makes httpserver
#  make clean               cleans the directory
#  make spotless            cleans the directory and removes executable
#------------------------------------------------------------------------------

CLIENT     = httpserver
NAME       = Server
SOURCE     = $(NAME).c
OBJECT     = $(NAME).o
ADT        = Queue
ADT_SOURCE = $(ADT).c
ADT_OBJECT = $(ADT).o
ADT_HEADER = $(ADT).h
COMPILE    = gcc -c -O2 -Wall -Wextra -Wpedantic -Wshadow
LINK       = gcc -o
FLAG       = -pthread
REMOVE     = rm -f

$(CLIENT) : $(OBJECT) $(ADT_OBJECT)
	$(LINK) $(CLIENT) $(OBJECT) $(ADT_OBJECT) $(FLAG)

$(OBJECT) : $(SOURCE) $(ADT_HEADER)
	$(COMPILE) $(SOURCE) $(FLAG)

$(ADT_OBJECT) : $(ADT_SOURCE) $(ADT_HEADER)
	$(COMPILE) $(ADT_SOURCE)

clean : 
	$(REMOVE) *.o

spotless: 
	$(REMOVE) $(CLIENT) *.o
