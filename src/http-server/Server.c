//-----------------------------------------------------------------------------
// Server.c
// Implementation file for multithreaded HTTP server with logging
// John Abendroth
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include "Queue.h"

#define BUFFER_SIZE 16384
static const char* error_201 = "HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n";
static const char* error_400 = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
static const char* error_403 = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
static const char* error_404 = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
static const char* error_500 = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";

typedef struct httpObject {
	char method[30];
	char filename[30];
	char filenamebuf[105];
	char httpversion[30];
	ssize_t content_length;
	int status_code;
	uint8_t buffer[BUFFER_SIZE];
} httpObject;

typedef struct loggingObject {
	uint8_t logflag, entries, errors;
	int8_t logfd;
	Queue queue;
	pthread_t thread_id;
	pthread_cond_t condition_var;
	pthread_mutex_t lock, error_lock, entries_lock;
} loggingObject;

typedef struct worker {
	uint8_t id;
	int8_t client_socketfd, tmplogfd, requestfd;
	httpObject message;
	loggingObject* log;
	Queue queue;
	pthread_t worker_id;
	pthread_cond_t condition_var;
	pthread_mutex_t lock;
} worker;

//helper function for returning errors
void error_response(int error_code, struct worker* workerobj) {
	workerobj->message.status_code = error_code;
	switch (error_code) {
		case 400:
			send(workerobj->client_socketfd, error_400, strlen(error_400), 0);
			workerobj->message.status_code = 400;
			break;
		case 403:
			send(workerobj->client_socketfd, error_403, strlen(error_403), 0);
			workerobj->message.status_code = 403;
			break;
		case 404:
			send(workerobj->client_socketfd, error_404, strlen(error_404), 0);
			workerobj->message.status_code = 404;
			break;
		case 500:
			send(workerobj->client_socketfd, error_500, strlen(error_500), 0);
			workerobj->message.status_code = 500;
			break;
	}

	if (workerobj->log->logflag == 1) {
		//generate failure response buffer
		char buffer[200];
		int8_t count = snprintf(buffer, sizeof(buffer), "FAIL: %s %s %s --- response ", workerobj->message.method, workerobj->message.filenamebuf, workerobj->message.httpversion);
		snprintf(buffer+count, sizeof(buffer)-count, "%d\n", workerobj->message.status_code);

		int8_t tmplogfd;
		if ((tmplogfd = open(".", O_RDWR | __O_TMPFILE, 0666)) == -1) {
			perror("Error opening tmplog");
		}
		if (write(tmplogfd, buffer, strlen(buffer)) == -1) {
			perror("Error response write");
		}
		workerobj->tmplogfd = tmplogfd;
		//dummy requestfd so that only 1 fd is written to log file
		workerobj->requestfd = -2;

		pthread_mutex_lock(&workerobj->log->error_lock);
		workerobj->log->errors++;
		pthread_mutex_unlock(&workerobj->log->error_lock);
	}
	dprintf(STDOUT_FILENO, "[+] response sent\n");
}

//helper function to construct response for 200 and 201 codes
//called at end of PUT/GET/HEAD if no other errors have been returned
void construct_http_response(int client_socketfd, struct worker* workerobj) {
	if (workerobj->message.status_code == 201) {
		send(client_socketfd, error_201, strlen(error_201), 0);
	}
	else if (workerobj->message.status_code == 200) {
		char buffer[200];
		snprintf(buffer, sizeof(buffer), "HTTP/1.1 200 OK\r\nContent-Length: %zd\r\n\r\n", workerobj->message.content_length);
		send(client_socketfd, buffer, strlen(buffer), 0);
	}
	dprintf(STDOUT_FILENO, "[+] response sent\n");
}

//core read response function
//does the intial receive header from client
//checks if the header is valid, parses it, and updates httpObject message data fields
void read_http_header(int8_t client_socketfd, struct worker* workerobj) {
	//can't assume all the header will come in one recv
	//use strstr to test if "\r\n\r\n" is in buffer, indicating end of header
	//if not, receiving until "\r\n\r\n" is found
	//add the offset of total bytes received to the buffer and subtract
	//the total bytes recieved from the amount of bytes to receive
	//to put split headers all into 1 buffer
	//idea from Clark's section
	char* search = "\r\n\r\n";
	size_t size = 4096;
	ssize_t total = 0;
	while (total < 4096) {
		ssize_t rec_sz = recv(client_socketfd, (workerobj->message.buffer)+total, size-total, 0);
		if (rec_sz == -1) {
			error_response(500, workerobj);
			perror("Header receive from client:");
			return;
		}
		total += rec_sz;
		workerobj->message.buffer[total] = '\0';
		char* result = strstr((char*)workerobj->message.buffer, search);
		if (result != NULL) {
			break;
		}
	}
	
	//parse the http header
	//read through with sscanf()
	//assign the pointer workerobj->message.filename to the static array tempfilename
	//so that we can do pointer arithmetic on workerobj->message.filename
	uint8_t headret = sscanf((char*)workerobj->message.buffer, "%s %s %s", workerobj->message.method, workerobj->message.filenamebuf, workerobj->message.httpversion);
	if (headret < 3) {
		error_response(400, workerobj);
		return;
	}

	//check if the file name adheres to resource name restriction
	if (workerobj->message.filenamebuf[0] != 47) {
		//needs filename to start with "/"
		error_response(400, workerobj);
		return;
	}

	//if so move the filename forwards one to get rid of "/"
	char* tempfilename = strdup(workerobj->message.filenamebuf);
	char* tempfilename_2 = &(*(tempfilename + 1));
	strncpy(workerobj->message.filename, tempfilename_2, 29);
	//clean up filename mess
	free(tempfilename);
	tempfilename = NULL;
	tempfilename_2 = NULL;

	size_t len = strlen(workerobj->message.filename);
	if(len > 27) {
		//throw 400 status code error for bad filename
		error_response(400, workerobj);
		return;
	}
	//validate method sent
	if ((strcmp(workerobj->message.method, "GET") != 0) && (strcmp(workerobj->message.method, "PUT") != 0) && strcmp(workerobj->message.method, "HEAD") != 0) {
		//bad method sent
		error_response(400, workerobj);
		return;
	}
	//validate httpversion
	if (strcmp(workerobj->message.httpversion, "HTTP/1.1") != 0) {
		//bad httpversion sent
		error_response(400, workerobj);
		return;
	}
	//validate filename
	//idea for strspn() taken from 
	//https://stackoverflow.com/a/44348019
	const char *okchar = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
	size_t spnsz = strspn(workerobj->message.filename, okchar);
	if(workerobj->message.filename[spnsz] != '\0') {
		//bad file name
		error_response(400, workerobj);
		return;
	}
	//else resource name is valid, parsing complete
}

//helper function to create first tmp file for logging header
void generate_logging_header(struct worker* workerobj) {
	//generate first line of logging with the header parsed into a httpObject
	char buffer[200];
	snprintf(buffer, sizeof(buffer), "%s %s length %zd\n", workerobj->message.method, workerobj->message.filenamebuf, workerobj->message.content_length);
	int8_t tmplogfd;
	if ((tmplogfd = open(".", O_RDWR | __O_TMPFILE, 0666)) == -1) {
		perror("Generate header error opening tmplog");
	}
	if (write(tmplogfd, buffer, strlen(buffer)) == -1) {
		perror("Generate header error writing tmplog");
	}
	workerobj->tmplogfd = tmplogfd;
}

//breakout function to handle PUT requests
void handle_PUT(int client_socketfd, struct worker* workerobj) {
	//extract content length from header
	//use strtok to search for \r\n
	int contentlen;
	char* posptr;
	char* token = strtok_r((char*)workerobj->message.buffer, "\r\n", &posptr);
	uint8_t ret = 0;
	while(token != NULL) {
		ret += sscanf(token, "Content-Length: %d", &contentlen);
		token = strtok_r(NULL, "\r\n", &posptr);
	}
	//if contentlen is still uninitialized, then the header format was wrong. Otherwise, it was found.
	if (ret != 0) {
		workerobj->message.content_length = (ssize_t)contentlen;
	}
	else {
		error_response(400, workerobj);
		return;
	}
	if (strcmp(workerobj->message.filename, "healthcheck") == 0) {
		error_response(403, workerobj);
		return;
	}

	generate_logging_header(workerobj);

	int8_t fd;
	if ((fd = open(workerobj->message.filename, O_RDWR | O_CREAT | O_TRUNC, 0644)) == -1) {
		uint8_t errsv = errno;
		perror("PUT opening file:");
		if (errsv == EACCES) {
			error_response(403, workerobj);
			return;
		}
		else {
			error_response(500, workerobj);
			return;
		}
	}
	
	//check for write permissions to the file opened
	//works for when program is run with root access and open will produce a fd anyways
	struct stat filestat;
	if (fstat(fd, &filestat) == 0) {
		if (!(filestat.st_mode & S_IWUSR)) {
			error_response(403, workerobj);
			return;
		}
	}

	///keep receiving util the content length amount is filled
	ssize_t recsz, writesz, total = 0;
	while (total < workerobj->message.content_length) {
		recsz = recv(client_socketfd, workerobj->message.buffer, BUFFER_SIZE, 0);
		if (recsz == -1) {
			perror("PUT receive from client:");
			error_response(500, workerobj);
			return;
		}
		writesz = write(fd, workerobj->message.buffer, recsz);
		if (writesz == -1) {
			perror("PUT write to buffer:");
			error_response(500, workerobj);
			return;
		}
		if (recsz == 0) {
			break;
		}
		else {
			total += recsz;
		}
	}
	//set the request fd equal to the fd for the file we just wrote to
	workerobj->requestfd = fd;
	//send 201 status code
	workerobj->message.status_code = 201;
	construct_http_response(client_socketfd, workerobj);
	if (workerobj->log->logflag == 0) {
		close(fd);
	}
}

//function to handle opening files for GET/HEAD requests
//check if the file requested can be opened
//if not, return (-1) as symbol of bad fd; if it has correct privileges, return its (pos) fd
//handle_get() will then read the contents of that fd and write it to socket
int open_file(struct worker* workerobj) {
	int8_t fd;
	if ((fd = open(workerobj->message.filename, O_RDONLY)) == -1) {
		uint8_t errsv = errno;
		perror("GET/HEAD opening file:");
		if (errsv == EACCES) {
			error_response(403, workerobj);
			close (fd);
			return (-1);
		}
		else if (errsv == ENOENT) {
			error_response(404, workerobj);
			close (fd);
			return (-1);
		}
		else {
			error_response(500, workerobj);
			close (fd);
			return (-1);
		}
	}

	//error checking for when program is run in sudo mode
	//taken from:
	//https://stackoverflow.com/questions/10323060/printing-file-permissions-like-ls-l-using-stat2-in-c
	struct stat filestat;
	if (fstat(fd, &filestat) == 0) {
		if (!(filestat.st_mode & S_IRUSR)) {
			error_response(403, workerobj);
			close (fd);
			return (-1);
		}
		if (!(S_ISREG(filestat.st_mode))) {
			error_response(403, workerobj);
			close(fd);
			return (-1);
		}
		if (S_ISDIR(filestat.st_mode)) {
			error_response(400, workerobj);
			close (fd);
			return (-1);
		}
	}
	else {
		//else fd didn't open properly for fstat
		uint8_t errsv = errno;
		perror("GET/HEAD fstat:");
		if (errsv == EBADF) {
			error_response(404, workerobj);
			return (-1);
		}
		else {
			error_response(500, workerobj);
			return (-1);
		}
	}
	workerobj->message.content_length = filestat.st_size;

	generate_logging_header(workerobj);
	
	return(fd);
}

//breakout function to handle GET requests
void handle_GET(int client_socketfd, struct worker* workerobj) {
	//check if health check was requested
	if (workerobj->log->logflag == 1) {
		if (strcmp(workerobj->message.filename, "healthcheck") == 0) {
			uint8_t errors, entries;

			pthread_mutex_lock(&workerobj->log->error_lock);
			errors = workerobj->log->errors;
			pthread_mutex_unlock(&workerobj->log->error_lock);

			pthread_mutex_lock(&workerobj->log->entries_lock);
			entries = workerobj->log->entries;
			pthread_mutex_unlock(&workerobj->log->entries_lock);

			//fill small buffer with special format of healthcheck
			char smallbuf[50];
			snprintf(smallbuf, sizeof(smallbuf), "%d\n%d", errors, entries);
			int8_t len = strlen(smallbuf);
			//set the content legnth to the smallbuf since healthcheck is just supposed to be like
			//returning a file of just the smallbuf
			workerobj->message.content_length = (ssize_t)len;

			generate_logging_header(workerobj);

			//send the 200 OK response like normal GET request
			workerobj->message.status_code = 200;
			construct_http_response(client_socketfd, workerobj);
			//send the healthcheck contents
			send(client_socketfd, smallbuf, len, 0);

			int8_t tmplogfd2;
			if ((tmplogfd2 = open(".", O_RDWR | __O_TMPFILE, 0666)) == -1) {
				perror("Health check open tmp file");
			}
			if (write(tmplogfd2, smallbuf, strlen(smallbuf)) == -1) {
				perror("Health check write to tmp file");
			}
			//use our created message as the requestfd
			//since the healthcheck file doesn't actually exist
			workerobj->requestfd = tmplogfd2;
			return;
		}
	}
	int fd;
	if ((fd = open_file(workerobj)) == -1) {
		return;
	}

	//send 200 okay code
	workerobj->message.status_code = 200;
	construct_http_response(client_socketfd, workerobj);

	//set the request fd equal to the file we just read from
	workerobj->requestfd = fd;

	ssize_t rdsz, total = 0;
	while (total < workerobj->message.content_length) {
		rdsz = read(fd, workerobj->message.buffer, BUFFER_SIZE);
		if (write(client_socketfd, workerobj->message.buffer, rdsz) == -1) {
			error_response(500, workerobj);
			perror("GET write failure:");
			return;
		}
		if (rdsz == -1) {
			error_response(500, workerobj);
			perror("GET read failure:");
			return;
		}
		if (rdsz == 0) {
			break;
		}
		else {
			total += rdsz;
		}
	}
	if (workerobj->log->logflag == 0) {
		close(fd);
	}
}

//core process request function
//determines if the request from the header is put get or head and executes that request type
void process_request(int client_socketfd, struct worker* workerobj) {
	if(strcmp(workerobj->message.method, "PUT") == 0) {
		handle_PUT(client_socketfd, workerobj);
	}
	else if(strcmp(workerobj->message.method, "GET") == 0) {
		handle_GET(client_socketfd, workerobj);
	}
	else if (strcmp(workerobj->message.method, "HEAD") == 0) {
		if (strcmp(workerobj->message.filename, "healthcheck") == 0) {
			error_response(403, workerobj);
			return;
		}
		int fd;
		if ((fd = open_file(workerobj)) == -1) {
			return;
		}
		//since only 1 valid fd will be enqueued we need to close this now
		close(fd);
		//send 200 okay code
		workerobj->message.status_code = 200;
		construct_http_response(client_socketfd, workerobj);
		//if it's a head request, we don't want the logging thread to dequeue 2 fds
		//because the second one could be another head request that snuck into the queue quickly behind a first head request
		//that equals a big synchronization error!!
		//set arbitrary flag of -2 for head requests
		workerobj->requestfd = -2;
	}
	else {
		//else bad header
		dprintf(STDERR_FILENO, "Bad header message\n");
		error_response(400, workerobj);
		return;
	}
}

void* logging_thread(void* thread) {
	struct loggingObject* log = (struct loggingObject*)thread;
	int8_t headerfd = -1, requestfd = -1;
	uint8_t buffer[4096];
	uint8_t hex_buffer[4096];
	size_t totalwr = 0, totalrd, len;
	ssize_t read_sz, write_sz;

	while (1) {
		//take the logging queue lock
		int8_t ret;
		ret = pthread_mutex_lock(&log->lock);

		//if the queue is empty, sleep
		while (isEmpty(log->queue)) {
			ret = pthread_cond_wait(&log->condition_var, &log->lock);
		}

		//double check we have the lock, and dequeue the next 2 files placed on queue
		if (ret == 0) {
			headerfd = Dequeue(log->queue);
			requestfd = Dequeue(log->queue);
			pthread_mutex_unlock(&log->lock);
		}
		else {
			perror("Error with logging pthread_cond_wati()");
			continue;
		}
		//if the header file descriptor is < 0 then something went wrong in read_http_header()
		if (headerfd < 0) {
			dprintf(STDERR_FILENO, "Error: invalid header file descriptor dequeued for logging\n");
			continue;
		}
		if (requestfd == -1) {
			dprintf(STDERR_FILENO, "Error: invalid file descriptor dequeued for logging request body\n");
			continue;
		}
		if (requestfd == -2) {
			//we're logging a head request
			//trick logging into not logging the request body
			//by setting the requestfd to -1
			requestfd = -1;
		}

		struct stat stat;
		if (fstat(headerfd, &stat) != 0) {
			perror("Logging fstat");
		}
		len = stat.st_size;
		totalrd = 0;
		read_sz = 0;
		write_sz = 0;

		//first write header to log file
		//don't need to worry about filling the buffer because it's guaranteed to be 105 bytes at max
		if ((read_sz = pread(headerfd, buffer, len, 0)) < 0) {
			perror("Logging read header error");
		}
		write_sz = pwrite(log->logfd, buffer, read_sz, totalwr);
		if (write_sz == -1) {
			perror("Logging write header error");
			totalwr++;
		}
		totalwr += write_sz;

		//if request fd != -1 then it's a PUT/GET request
		//log the PUT/GET request contents
		if (requestfd != -1) {
			struct stat stat2;
			if (fstat(requestfd, &stat2) != 0) {
				perror("Logging fstat2");
			}
			size_t len_request = stat2.st_size;
			//reset the file offset
			lseek(requestfd, 0, SEEK_SET);
			//keep track of the bytes logged so it's reentrant if the buffer didn't capture the whole file
			size_t bytes_logged = 0;

			while (totalrd < len_request) {
				if ((read_sz = read(requestfd, buffer, 4096)) == -1) {
					perror("Logging pread");
				}
				if (read_sz == 0) {
					break;
				}
				else {
					totalrd += read_sz;
				}

				//loop through each character in the buffer and hex dump it to the log file
				for (uint16_t i = 0; i < read_sz; i++) {
					//if bytes logged % 20 = 0 then we've reached the end of a line limit
					if ((bytes_logged > 0) && (bytes_logged % 20 == 0)) {
						write_sz = pwrite(log->logfd, "\n", 1, totalwr);
						totalwr += write_sz;
					}
					//add the padding with the bytes logged count
					if (bytes_logged % 20 == 0) {
						sprintf((char*)hex_buffer, "%08zd", bytes_logged);
						write_sz = pwrite(log->logfd, hex_buffer, 8, totalwr);
						totalwr += write_sz;
					}
					//convert the current character to hex and write it to the file
					sprintf((char*)hex_buffer, " %02x", buffer[i]);
					write_sz = pwrite(log->logfd, hex_buffer, 3, totalwr);
					totalwr += write_sz;
					bytes_logged++;
				}

				//if the total bytes read = number of bytes in the request file
				//then we're at the last line but it won't % with 20 clean
				//so add a "\n" ourselves
				if (totalrd == len_request) {
					write_sz = pwrite(log->logfd, "\n", 1, totalwr);
					totalwr += write_sz;
				}
			}
		}

		//add the log termination signal
		char* end = "========\n";
		write_sz = pwrite(log->logfd, end, strlen(end), totalwr);
		totalwr += write_sz;
		//done with this iteration of logging thread
		close (headerfd);
		close(requestfd);
		pthread_mutex_lock(&log->entries_lock);
		log->entries++;
		pthread_mutex_unlock(&log->entries_lock);
	}
}

//beginning function passed to new pthreads
//synchronizes dequeueing a file descriptor
//then handles the http request
void* worker_thread_func(void* thread) {
	struct worker* w_thread = (struct worker*)thread;

	while (1) {
		int8_t ret;
		//take the lock to start dequeue proccess
		ret = pthread_mutex_lock(&w_thread->lock);

		//check if there are any fd in the queue
		//if the queue is empty, sleep (releases the lock)
		//once signaled awake, pthread_cond_wait() will take the lock back
		while (isEmpty(w_thread->queue)) {
			ret = pthread_cond_wait(&w_thread->condition_var, &w_thread->lock);
		}

		//double check pthread_cond_wait() returned successfully and we have the lock
		if (ret == 0) {
			w_thread->client_socketfd = Dequeue(w_thread->queue);
			pthread_mutex_unlock(&w_thread->lock);
		}
		else {
			perror("Error with pthread_cond_wait()");
			close(w_thread->client_socketfd);
			continue;
		}

		//reset status code inbetween loops so that process_request isn't skipped
		w_thread->message.status_code = 0;
		w_thread->message.content_length = 0;

		//being parsing header of request
		read_http_header(w_thread->client_socketfd, w_thread);

		//check if error code was set
		if(w_thread->message.status_code >= 400) {
			if (w_thread->log->logflag == 1) {
				//enqueue temp file fd & request fd to logging queue
				pthread_mutex_lock(&w_thread->log->lock);
				Enqueue(w_thread->log->queue, w_thread->tmplogfd);
				Enqueue(w_thread->log->queue, w_thread->requestfd);
				pthread_mutex_unlock(&w_thread->log->lock);

				//signal to start working
				pthread_cond_signal(&w_thread->log->condition_var);
			}
			close(w_thread->client_socketfd);
			//reset the requestfd on each loop
			w_thread->tmplogfd = -1;
			w_thread->requestfd = -1;
			continue;
		}

		process_request(w_thread->client_socketfd, w_thread);

		if (w_thread->log->logflag == 1) {
			//enqueue temp file fd & request fd to logging queue
			pthread_mutex_lock(&w_thread->log->lock);
			Enqueue(w_thread->log->queue, w_thread->tmplogfd);
			Enqueue(w_thread->log->queue, w_thread->requestfd);
			pthread_mutex_unlock(&w_thread->log->lock);

			//signal to start working
			pthread_cond_signal(&w_thread->log->condition_var);
		}

		//say that we are done
		close(w_thread->client_socketfd);
		//reset the requestfd on each loop
		w_thread->tmplogfd = -1;
		w_thread->requestfd = -1;
	}
}

int main(int argc, char **argv) {
	if (argc < 2) {
		dprintf(STDERR_FILENO, "Bad argument format\n");
		return EXIT_FAILURE;
	}
	char* port;
	char log_filename[200];
	int8_t opt;
	uint8_t nthreads = 4, logflag = 0;
	while ((opt = getopt(argc, argv, "N:l:")) != -1) {
		switch (opt) {
			case 'N':
				if(optarg) {
					nthreads = atoi(optarg);
				}
				break;
			case 'l':
				logflag = 1;
				//log_filename = optarg;
				strcpy(log_filename, optarg);
				break;
			case '?':
				if (optopt == 'l') {
					dprintf(STDERR_FILENO, "Option -%c requires a filename\n", optopt);
				}
				if (optopt == 'N') {
					dprintf(STDERR_FILENO, "Option -%c requires a thread count if flagged\n", optopt);
				}
				else {
					dprintf(STDERR_FILENO, "Uknown option character ");
				}
				return (EXIT_FAILURE);
			default:
				abort();
		}
	}
	if (argc - optind > 1) {
		dprintf(STDERR_FILENO, "Warning: Too many arguments, server may exhibit undetermined behavior\n");
	}
	if (optind < argc) {
		port = argv[optind];
	}
	else {
		dprintf(STDERR_FILENO, "Error: Server requires port number\n");
		return (EXIT_FAILURE);
	}
	if (nthreads == 0) {
		dprintf(STDERR_FILENO, "Error: Bad thread count argument. Defaulting to 4 threads\n");
		nthreads = 4;
	}

	//setup logging
	loggingObject log;
	int8_t logfd = -1;
	log.logfd = logfd;
	log.logflag = logflag;
	//if logging is enabled, get everything setup
	if (logflag == 1) {
		if ((logfd = open(log_filename, O_RDWR | O_CREAT | O_TRUNC, 0666)) == -1) {
			perror("Opening logfile");
		}
		//update file descriptor
		log.logfd = logfd;
		log.queue = newQueue();
		log.condition_var = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
		log.lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
		log.error_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
		log.entries_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
		log.errors = 0;
		log.entries = 0;

		if (pthread_create(&log.thread_id, NULL, &logging_thread, &log) != 0) {
			perror("Error creating logging thread");
			return (EXIT_FAILURE);
		}
	}

	//setup the array of worker threads
	worker workers[nthreads];

	//setup worker threads' struct objects
	//idea for worker struct came from Micael's section
	for (uint8_t i = 0; i < nthreads; i ++) {
		workers[i].id = i;
		workers[i].condition_var = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
		workers[i].log = &log;
		workers[i].tmplogfd = -1;
		workers[i].requestfd = -1;

		if (pthread_mutex_init(&workers[i].lock, NULL) != 0) {
			perror("Error initializing mutex");
			return (EXIT_FAILURE);
		}

		workers[i].queue = newQueue();

		if (pthread_create(&workers[i].worker_id, NULL, &worker_thread_func, &workers[i]) != 0) {
			perror("Error creating thread");
			return (EXIT_FAILURE);
		}
	}

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(atoi(port));
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	socklen_t addrlen = sizeof(server_addr);

	//create socket
	int server_socketfd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_socketfd < 0) {
		perror("Unable to build socket");
	}

	//configure server socket
	int enable = 1;

	//avoid: 'Bind: Address Already in Use' error
	int ret = setsockopt(server_socketfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

	//bind server address to new socket
	ret = bind(server_socketfd, (struct sockaddr*)&server_addr, addrlen);

	if (ret < 0) {
		perror("Binding error");
		return EXIT_FAILURE;
	}

	//listen for connection
	ret = listen(server_socketfd, 5);

	if (ret < 0) {
		perror("Listening error");
		return EXIT_FAILURE;
	}

	//connecting with a client
	struct sockaddr client_addr;
	socklen_t client_addrlen = sizeof(client_addr);

	uint8_t count = 0, target_thread = 0;
	while (1) {
		dprintf(STDOUT_FILENO, "[+] server is waiting...\n");

		//accept connection
		int client_socketfd = accept(server_socketfd, (struct sockaddr*)&client_addr, &client_addrlen);
		if(client_socketfd < 0) {
			perror("Error creating socket");
		}
		target_thread = count % nthreads;

		//enqueue socketfd to a worker thread queue
		//mutex lock their queue so they can't dequeue something at the same time
		pthread_mutex_lock(&workers[target_thread].lock);
		Enqueue(workers[target_thread].queue, client_socketfd);
		pthread_mutex_unlock(&workers[target_thread].lock);

		//signal to start working
		pthread_cond_signal(&workers[target_thread].condition_var);

		count++;
		dprintf(STDOUT_FILENO, "[+] dispatcher thread accepted connection and placed it in the queue\n");
	}

	//never going to execute but all heap memory deallocation
	for (uint8_t j = 0; j < nthreads; j++) {
		freeQueue(&workers[j].queue);
	}

	return (EXIT_SUCCESS);
}
