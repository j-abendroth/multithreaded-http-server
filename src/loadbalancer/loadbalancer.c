//-----------------------------------------------------------------------------
// loadbalancer.c
// Implementation file for the httpserver load balancer
// John Abendroth
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include "Queue.h"

#define BUFFER_SIZE 4096
#define POLL_TIME 1
#define WAIT_TIME_SECONDS 2
static const char* error_500 = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
static const char* hlthchk_probe = "GET /healthcheck HTTP/1.1\r\n\r\n";

typedef struct serverObject {
	uint8_t status_flag;
	uint16_t port;
	uint16_t entries;
	uint16_t errors;
} serverObject;

typedef struct healthcheckObject {
	uint8_t request_rate;
	int16_t optimal_server_port;
	pthread_t thread_id;
	uint8_t server_arr_sz;
	uint8_t request_count;
	pthread_cond_t request_var;
	pthread_mutex_t lock;
	pthread_mutex_t port_lock;
	sem_t semaphore;
	serverObject *server_arr;
} healthcheckObject;

typedef struct worker {
	uint8_t id;
	Queue queue;
	healthcheckObject* healthcheck;
	pthread_t worker_id;
	pthread_cond_t condition_var;
	pthread_mutex_t lock;
} worker;

/*
 * client_connect takes a port number and establishes a connection as a client.
 * connectport: port number of server to connect to
 * returns: valid socket if successful, -1 otherwise
 */
int16_t client_connect(uint16_t connectport) {
	int16_t connfd;
	struct sockaddr_in servaddr;

	connfd=socket(AF_INET,SOCK_STREAM,0);
	if (connfd < 0) {
		return -1;
	}
	memset(&servaddr, 0, sizeof servaddr);

	servaddr.sin_family=AF_INET;
	servaddr.sin_port=htons(connectport);

	/* For this assignment the IP address can be fixed */
	inet_pton(AF_INET,"127.0.0.1", &(servaddr.sin_addr));

	if(connect(connfd,(struct sockaddr *)&servaddr,sizeof(servaddr)) < 0) {
		return -1;
	}
	return connfd;
}

/*
 * server_listen takes a port number and creates a socket to listen on 
 * that port.
 * port: the port number to receive connections
 * returns: valid socket if successful, -1 otherwise
 */
int8_t server_listen(uint16_t port) {
	int8_t listenfd;
	int enable = 1;
	struct sockaddr_in servaddr;

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0) {
		perror("Unable to build listenfd socket");
		return -1;
	}
	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htons(INADDR_ANY);
	servaddr.sin_port = htons(port);

	if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
		perror("setsockopt");
		return -1;
	}
	if (bind(listenfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
		perror("bind");
		return -1;
	}
	if (listen(listenfd, 500) < 0) {
		perror("Listening error");
		return -1;
	}
	return listenfd;
}

/*
 * bridge_connections send up to 100 bytes from fromfd to tofd
 * fromfd, tofd: valid sockets
 * returns: number of bytes sent, 0 if connection closed, -1 on error
 */
int16_t bridge_connections(int16_t fromfd, int16_t tofd) {
	uint8_t recvline[BUFFER_SIZE];
	int16_t rec_sz = recv(fromfd, recvline, BUFFER_SIZE, 0);
	if (rec_sz < 0) {
		dprintf(STDERR_FILENO, "connection error receiving\n");
		return -1;
	} else if (rec_sz == 0) {
		//receive done, no more data to receieve
		return 0;
	}
	recvline[rec_sz] = '\0';
	rec_sz = send(tofd, recvline, rec_sz, 0);
	if (rec_sz < 0) {
		dprintf(STDERR_FILENO, "connection error sending\n");
		return -1;
	}
	return rec_sz;
}

/*
 * bridge_loop forwards all messages between both sockets until the connection
 * is interrupted. It also prints a message if both channels are idle.
 * sockfd1, sockfd2: valid sockets
 */
void bridge_loop(int16_t sockfd1, int16_t sockfd2, uint8_t *timeoutflag) {
	fd_set set;
	struct timeval timeout;

	int16_t fromfd, tofd;
	while(1) {
		// set for select usage must be initialized before each select call
		// set manages which file descriptors are being watched
		FD_ZERO (&set);
		FD_SET (sockfd1, &set);
		FD_SET (sockfd2, &set);

		// same for timeout
		// set max waiting time to the wait time constant
		// set to 2 because lower than 2 tends to have the server not be able to respond fast enough
		timeout.tv_sec = WAIT_TIME_SECONDS;
		timeout.tv_usec = 0;

		// select return the number of file descriptors ready for reading in set
		switch (select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
			case -1:
				dprintf(STDERR_FILENO, "Error during select, exiting\n");
				return;
			case 0:
				//both sockets are idle and timed out
				dprintf(STDERR_FILENO, "Error: server timed out and failed to respond\n");
				*timeoutflag = 1;
				return;
			default:
				if (FD_ISSET(sockfd1, &set)) {
					fromfd = sockfd1;
					tofd = sockfd2;
				} else if (FD_ISSET(sockfd2, &set)) {
					fromfd = sockfd2;
					tofd = sockfd1;
				} else {
					dprintf(STDERR_FILENO, "Error: this should be unreachable\n");
					return;
				}
		}
		if (bridge_connections(fromfd, tofd) <= 0) {
			return;
		}
	}
}

//mini breakout healper thread for healthcheck thread
//specifically for probing an individual server and updating its num of entries
void* hlthchkwrkr_func(void* thread) {
	struct serverObject* server = (struct serverObject*)thread;

	//connect to the assigned server as a client
	int connectfd;
	if ((connectfd = client_connect(server->port)) == -1) {
		dprintf(STDERR_FILENO, "Error establishing connection to server %d for healthcheck probe\n", server->port);
		server->status_flag = 0;
		close(connectfd);
		return NULL;
	}
	
	//setup select() parameters to watch to socket
	//idea from tutorialspoint.com/unix_system_calls/_newselect.htm
	fd_set set;
	struct timeval timeout;
	FD_ZERO(&set);
	FD_SET(connectfd, &set);
	timeout.tv_sec = WAIT_TIME_SECONDS;
	timeout.tv_usec = 0;

	//send a healthcheck probe to the server
	if ((write(connectfd, hlthchk_probe, strlen(hlthchk_probe))) < 0) {
		perror("hlthchkwrkr send()");
	}
	
	//watch the socket to see when it becomes ready for reading
	//if it takes longer than X seconds, timeout
	switch(select(FD_SETSIZE, &set, NULL, NULL, &timeout)) {
		case -1:
			perror("Error in select()");
			server->status_flag = 0;
			close(connectfd);
			return NULL;
		case 0:
			//server didn't respond within X seconds, mark as down
			server->status_flag = 0;
			close(connectfd);
			return NULL;
		default:
			if (FD_ISSET(connectfd, &set)) {
				//socket is ready to read from :)
			}
	}	

	//now know socket is ready to read from
	//receive the healthcheck data
	//keep receiving until recv returns 0 meaning no more content received
	uint8_t buffer[BUFFER_SIZE];
	ssize_t rec_sz = 0, totalrec = 0;
	rec_sz = recv(connectfd, buffer, BUFFER_SIZE, 0);
	totalrec = rec_sz;
	while (rec_sz > 0) {
		if ((rec_sz = recv(connectfd, buffer + totalrec, BUFFER_SIZE - totalrec, 0)) == -1) {
			dprintf(STDERR_FILENO, "Error receiving healthcheck from server %d: %s\n", server->port, strerror(errno));
			break;
		}
		totalrec += rec_sz;
	}
	buffer[totalrec] = '\0';

	//now parse the buffer for the errors and entries
	//if sscanf doesn't return the expected amount, mark the server as down
	uint16_t code, length, errors, entries;
	if (sscanf((char*)buffer,"HTTP/1.1 %hu OK\r\nContent-Length: %hu\r\n\r\n%hu\r\n%hu", &code, &length, &errors, &entries) != 4) {
		dprintf(STDERR_FILENO, "Invalid healthcheck response sent\n");
		server->status_flag = 0;
		close(connectfd);
		return NULL;
	}
	if (code != 200) {
		dprintf(STDERR_FILENO, "Bad response code received from healthcheck\n");
		server->status_flag = 0;
		close(connectfd);
		return NULL;
	}
	//make sure the status flag is back at 1 incase the server went down in the past
	server->status_flag = 1;
	server->errors = errors;
	server->entries = entries;
	close(connectfd);
	return NULL;
}

//healthcheck thread specifically for running the initial healthcheck probe
//made to ensure worker threads can't process a request without an initial healthcheck
void healthcheck_first_probe(struct healthcheckObject* healthcheck) {
	pthread_t thread_arr[healthcheck->server_arr_sz];
	//create threads to poll their respective servers
	for (uint8_t i = 0; i < healthcheck->server_arr_sz; i++) {
		(void) pthread_create(&thread_arr[i], NULL, &hlthchkwrkr_func, &healthcheck->server_arr[i]);
	}

	for (uint8_t i = 0; i < healthcheck->server_arr_sz; i++) {
		(void) pthread_join(thread_arr[i], NULL);
	}

	//find server with least load
	int8_t min_index = -1;
	int16_t min_entries = -1;
	for (uint8_t i = 0; i < healthcheck->server_arr_sz; i++) {
		//if server[i] is marked as down, don't even consider it
		if (healthcheck->server_arr[i].status_flag == 1) {
			//if min_entries is still undefined, check if server[i] is marked operational
			//if server[i] is up, set min entries equal to server[i]'s entries
			if (min_entries == -1) {
				min_entries = healthcheck->server_arr[i].entries;
				min_index = i;
			}
			//else run comparison on server[i] entires to min_entries
			else {
				if (healthcheck->server_arr[i].entries < min_entries) {
					min_entries = healthcheck->server_arr[i].entries;
					min_index = i;
				}
				else if (healthcheck->server_arr[i].entries == min_entries) {
					float success_rate_subi = 1-((float)healthcheck->server_arr[i].errors/(float)healthcheck->server_arr[i].entries);
					float success_rate_min_index = 1-((float)healthcheck->server_arr[min_index].errors/(float)healthcheck->server_arr[min_index].entries);
					if (success_rate_subi > success_rate_min_index) {
						min_entries = healthcheck->server_arr[i].entries;
						min_index = i;
					}
					else {
						//do nothing
						//we'll pick a server "at random" by letting whoever is currently marked stay that way
					}
				}
			}
		}
	}
	//if all servers are marked as down and we failed to get a min_entries
	//set the global min server port to arbitrary -1
	pthread_mutex_lock(&healthcheck->port_lock);
	if (min_entries == -1) {
		healthcheck->optimal_server_port = -1;
	}
	else {
		healthcheck->optimal_server_port = healthcheck->server_arr[min_index].port;
	}
	pthread_mutex_unlock(&healthcheck->port_lock);

	return;
}

//main healthcheck thread
//probes servers and finds min num of entires among them
void* healthcheck_thread(void* thread) {
	struct healthcheckObject* healthcheck = (struct healthcheckObject*)thread;
	//semaphore should be already decremented
	//do a first probe of the servers then unlock the semaphore and let the loadbalancer accept requests
	healthcheck_first_probe(healthcheck);
	sem_post(&healthcheck->semaphore);

	pthread_t thread_arr[healthcheck->server_arr_sz];
	while(1) {
		struct timespec ts;
		struct timeval tp;

		//take the lock
		int8_t ret;
		ret = pthread_mutex_lock(&healthcheck->lock);
		//setup the absolute time for pthread_cond_timedwait
		//taken from ibm.com/support/knowledgecenter/ssw_ibm_i_73/apis/users_77.htm
		gettimeofday(&tp, NULL);
		ts.tv_sec = tp.tv_sec;
		ts.tv_nsec = tp.tv_usec * 1000;
		ts.tv_sec += POLL_TIME;

		//check if the number of requests since last health probe has reached the amount to trigger a new healthcheck probe
		//if not, sleep for either the defined amount of time or until main() signals that the request amount has been reached
		while (healthcheck->request_count < healthcheck->request_rate) {
			ret = pthread_cond_timedwait(&healthcheck->request_var, &healthcheck->lock, &ts);
			if (ret == ETIMEDOUT) {
				break;
			}
		}
		//release the lock because we're not actually needing to lock on any shared memory
		(void) pthread_mutex_unlock(&healthcheck->lock);
		if (ret < 0) {
			if (ret != ETIMEDOUT) {
				perror("Error with pthread_cond_timedwait()");
				continue;
			}
		}

		//create threads to poll their respective servers
		for (uint8_t i = 0; i < healthcheck->server_arr_sz; i++) {
			(void) pthread_create(&thread_arr[i], NULL, &hlthchkwrkr_func, &healthcheck->server_arr[i]);
		}

		for (uint8_t i = 0; i < healthcheck->server_arr_sz; i++) {
			(void) pthread_join(thread_arr[i], NULL);
		}

		//find server with least load
		int8_t min_index = -1;
		int16_t min_entries = -1;
		for (uint8_t i = 0; i < healthcheck->server_arr_sz; i++) {
			//if server[i] is marked as down, don't even consider it
			if (healthcheck->server_arr[i].status_flag == 1) {
				//if min_entries is still undefined, check if server[i] is marked operational
				//if server[i] is up, set min entries equal to server[i]'s entries
				if (min_entries == -1) {
					min_entries = healthcheck->server_arr[i].entries;
					min_index = i;
				}
				//else run comparison on server[i] entires to min_entries
				else {
					if (healthcheck->server_arr[i].entries < min_entries) {
						min_entries = healthcheck->server_arr[i].entries;
						min_index = i;
					}
					else if (healthcheck->server_arr[i].entries == min_entries) {
						float success_rate_subi = 1-((float)healthcheck->server_arr[i].errors/(float)healthcheck->server_arr[i].entries);
						float success_rate_min_index = 1-((float)healthcheck->server_arr[min_index].errors/(float)healthcheck->server_arr[min_index].entries);
						if (success_rate_subi > success_rate_min_index) {
							min_entries = healthcheck->server_arr[i].entries;
							min_index = i;
						}
						else {
							//do nothing
							//we'll pick a server "at random" by letting whoever is currently marked stay that way
						}
					}
				}
			}
		}
		//if all servers are marked as down and we failed to get a min_entries
		//set the global min server port to arbitrary -1
		pthread_mutex_lock(&healthcheck->port_lock);
		if (min_entries == -1) {
			healthcheck->optimal_server_port = -1;
		}
		else {
			healthcheck->optimal_server_port = healthcheck->server_arr[min_index].port;
		}
		pthread_mutex_unlock(&healthcheck->port_lock);
		//reset request count after healthcheck probe
		healthcheck->request_count = 0;
	}
}

//main worker thread
//dequeues client connection and runs a bridge loop on it
void* worker_thread_func(void* thread) {
	struct worker* w_thread = (struct worker*)thread;

	while (1) {
		int16_t client_socketfd = -1;
		int8_t ret;
		//take the lock to start dequeue process
		ret = pthread_mutex_lock(&w_thread->lock);

		//check if there are any fd in the queue
		//if the queue is empty, sleep (releases the lock)
		//once signaled awake, pthread_cond_wait() will take the lock back
		while (isEmpty(w_thread->queue)) {
			ret = pthread_cond_wait(&w_thread->condition_var, &w_thread->lock);
		}

		//double check pthread_cond_wait() returned successfully and we have the lock
		if (ret == 0) {
			client_socketfd = Dequeue(w_thread->queue);
			pthread_mutex_unlock(&w_thread->lock);
		}
		else {
			perror("Error with pthread_cond_wait()");
			close(client_socketfd);
			continue;
		}
		if (client_socketfd == -1) {
			dprintf(STDERR_FILENO, "Error with dequeue\n");
			continue;
		}

		//get the current optimal server port and connect to it as a client
		//if the optimal server port is -1 right now, all servers were down at last probe
		int16_t server_port;
		pthread_mutex_lock(&w_thread->healthcheck->port_lock);
		server_port = w_thread->healthcheck->optimal_server_port;
		pthread_mutex_unlock(&w_thread->healthcheck->port_lock);
		if (server_port == -1) {
			dprintf(STDERR_FILENO, "Error: All servers were down at last healthcheck probe\n");
			send(client_socketfd, error_500, strlen(error_500), 0);
			close(client_socketfd);
			continue;
		}
		int16_t serverconnfd;
		if ((serverconnfd = client_connect(server_port)) == -1) {
			dprintf(STDERR_FILENO, "Error: failed to connect to server as client\n");
			send(client_socketfd, error_500, strlen(error_500), 0);
			close(client_socketfd);
			continue;
		}
		uint8_t timeoutflag = 0;
		bridge_loop(client_socketfd, serverconnfd, &timeoutflag);
		if (timeoutflag == 1) {
			send(client_socketfd, error_500, strlen(error_500), 0);
		}
		close(client_socketfd);
		close(serverconnfd);
	}
}

int main(int argc, char** argv) {
	if (argc < 3) {
		dprintf(STDERR_FILENO, "Bad argument format\n");
		return (EXIT_FAILURE);
	}
	int8_t opt;
	uint8_t parallel_connections = 4, request_refresh_period = 5;
	uint16_t listening_port;
	while ((opt = getopt(argc, argv, "N:R:")) != -1) {
		switch (opt) {
			case 'N':
				if(optarg) {
					parallel_connections = atoi(optarg);
				}
				break;
			case 'R':
				if(optarg) {
					request_refresh_period = atoi(optarg);
				}
				break;
			case '?':
				if (optopt == 'N') {
					dprintf(STDERR_FILENO, "Option -%c requires a number of parallel connections\n", optopt);
				}
				if (optopt == 'R') {
					dprintf(STDERR_FILENO, "Option -%c requires a health check refresh period number\n", optopt);
				}
				else {
					dprintf(STDERR_FILENO, "Unknown option character\n");
				}
				return (EXIT_FAILURE);     
			default:
				abort();     
		}
	}
	if (argc - optind < 2) {
		dprintf(STDERR_FILENO, "Error: Load balancer requires a port number to listen on and at least 1 server port number\n");
		return(EXIT_FAILURE);
	}
	//listening port is first mandatory argument
	listening_port = atoi(argv[optind]);
	optind++;

	//setup the array of server objects
	uint8_t port_arr_sz = argc-optind;
	serverObject *server_arr = malloc(port_arr_sz * sizeof(struct serverObject));
	for (uint8_t i = 0; i < port_arr_sz; i++) {
		server_arr[i].port = atoi(argv[optind]);
		//set status flag to 1 for operational
		server_arr[i].status_flag = 1;
		server_arr[i].entries = 0;
		server_arr[i].errors = 0;
		optind++;
	}

	//setup the healthcheck object and its thread
	//decrement the semaphore as soon as possible to prevent synchronization issues with healthcheck probe
	healthcheckObject healthcheck;
	sem_init(&healthcheck.semaphore, 0, 1);
	sem_wait(&healthcheck.semaphore);
	healthcheck.server_arr = server_arr;
	healthcheck.server_arr_sz = port_arr_sz;
	healthcheck.request_rate = request_refresh_period;
	healthcheck.optimal_server_port = 0;
	healthcheck.request_count = 0;
	healthcheck.request_var = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	healthcheck.lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	healthcheck.port_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	if (pthread_create(&healthcheck.thread_id, NULL, &healthcheck_thread, &healthcheck) != 0) {
		perror("Error creating healthcheck thread");
		return (EXIT_FAILURE);
	}

	//setup the array of worker threads
	worker workers[parallel_connections];

	//setup worker threads' struct objects
	//set to N flag for number of parallel connections
	for (uint8_t j = 0; j < parallel_connections; j++) {
		workers[j].id = j;
		workers[j].queue = newQueue();
		workers[j].healthcheck = &healthcheck;
		workers[j].condition_var = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
		workers[j].lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;

		if (pthread_create(&workers[j].worker_id, NULL, &worker_thread_func, &workers[j]) != 0) {
			perror("Error creating thread");
			return (EXIT_FAILURE);
		}
	}

	int16_t listen_socketfd;
	if ((listen_socketfd = server_listen(listening_port)) < 0) {
		dprintf(STDERR_FILENO, "failed listening\n");
		return(EXIT_FAILURE);
	}

	//connecting with a client
	struct sockaddr client_addr;
	socklen_t client_addrlen = sizeof(client_addr);

	//wait for the first healthcheck probe to complete before accepting connections
	sem_wait(&healthcheck.semaphore);

	uint8_t count = 0, target_thread = 0;
	while (1) {
		//accept connection
		int16_t client_socketfd;
		if ((client_socketfd = accept(listen_socketfd, (struct sockaddr*)&client_addr, &client_addrlen)) < 0) {
			dprintf(STDERR_FILENO, "failed accepting\n");
			continue;
		}

		target_thread = count % parallel_connections;

		//enqueue socketfd to a worker thread queue
		//mutex lock their queue so they can't dequeue something at the same time
		pthread_mutex_lock(&workers[target_thread].lock);
		Enqueue(workers[target_thread].queue, client_socketfd);
		pthread_mutex_unlock(&workers[target_thread].lock);

		//signal to start working
		pthread_cond_signal(&workers[target_thread].condition_var);

		//if request count is equal to R, time to healthcheck probe
		healthcheck.request_count++;
		if(healthcheck.request_count == healthcheck.request_rate) {
			pthread_cond_signal(&healthcheck.request_var);
		}

		count++;
	}

	//never going to execute but all heap memory deallocation
	for (uint8_t j = 0; j < parallel_connections; j++) {
		freeQueue(&workers[j].queue);
	}
	free(server_arr);

	return (EXIT_SUCCESS);
}
