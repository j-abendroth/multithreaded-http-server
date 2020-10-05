# Load Balanced HTTP Server

#### A multithreaded HTTP Server with logging and healthcheck functionality

This was a final project for a Systems Design class at UC Santa Cruz. I designed the entire system from scratch, implementing a HTTP/1.1 server capable of responding to `GET`, `HEAD`, and `PUT` requests. It is written in pure C, focusing on POSIX system calls, and supports multithreading for concurrent connections to each server. I also implemented a load balancer to manage multiple instances of the HTTP Server, which ensures requests are processed quickly and equally among HTTP Servers. 

## How does it work?

#### HTTP Server:

The HTTP Server utilizes a worker thread idea, where I accept connections to the server on a main thread, and keep a queue of threads ready to process requests. When a connection is accepted, the main thread rotates through the available worker threads and dispatches connections to them in order. There is additionally a logging thread which is dedicated to reading a queue of temp log files written by worker threads, and hex dumping each request to the server to a main logging file. 

Operations supported by the HTTP Server are GET, HEAD, PUT, and a special GET request called healthcheck. When a GET request is processed for a file called "Healthcheck", the HTTP Server will return the client and number of requests and the number of errors processed. 

#### Load Balancer:

The Load Balancer is a multithreaded application that manages connections to from clients to a queue of HTTP Servers. The load balancer utilizes a single worker thread to repeat the process of accepting connections to the load balancer, like the HTTP Server, but once the worker thread has a connection placed in its queue, it forwards it to the currently best available HTTP Server as determined by the healthcheck thread. This healthcheck thread periodically probes each of the HTTP Servers the load balancer is managing, and keeps track of which server has the lowest failure response rate as returned by healthcheck GET requests. 

Additionally, both the HTTP Server and Load Balancer have documents explaining my design choices for them.

## How to run

#### HTTP Server:

To start run `make` in the directory to build the executable. Then to run, the HTTP Server takes 3 options:
* `[port number]`
* `-N [number of threads to run concurrently]`
* `-l [log file name]`

Options `-N` and `-l` are optional, and if not specified will default to 4 threads and no logging, respectively. A port number **must** be specified, however. Valid port numbers are 1024 and above. 

Examples of valid parameters:
* `./httpserver 8080 -N 4 -l log_file`
* `./httpserver 1024`
* `./httpserver -N 6 8081`

Once your HTTP Server is started, send requests using cURL or another HTTP/1.1 client. Example cURL commands:
* `curl http://localhost:8080/filename`
* `curl -T localfile.txt http://localhost:8080/filename`
* `curl -I http://localhost:8082/filename`
* `curl http://localhost:8080/healthcheck`

#### Load Balancer:

Again run `make` in the directory, or use the provided binaries for `loadbalancer` and `httpserver`. Spool up the number of `httpserver` instances you want load balanced, using the above directions. Once ready, the load balancer takes 4 parameters:

* [port number to receive connections on]
* [port number/s of httpserver instance/s]
* -N [number of parallel connections served at once]
* -R [how often to complete a healthcheck probe in number of requests]

Again, parameters can be entered in any order. However, the first mandatory port number entered will be the listening port i.e `./loadbalancer 1024 8080` means loadbalancer is listening on port `1024`, and sending connections to port `8080`. You must enter a port number to receieve connections on, and at least 1 port number of an `httpserver` instance. The number of parallel connections will default to 4 if nothing is entered, and healthcheck probes will default to 5 requests. Examples of loadbalancer options:

* `./loadbalancer 1024 8080 8082 -N 6 -R 2`
* `./loadbalancer 1024 8081 8082 8084 -R 10`
* `./loadbalancer 1024 8080 8081 8082 8083`

Once configured, treat the loadbalancer the same as an  `httpserver` instance from before, using cURL or other HTTP/1.1 clients. 
