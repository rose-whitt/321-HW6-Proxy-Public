/*
 * COMP 321 Project 6: Web Proxy
 *
 * This program implements a multithreaded HTTP proxy.
 *
 * Harry Witt hlw2
 * Rose Whitt rew9
 *
 */

#include <assert.h>

#include "csapp.h"

#define NTHREADS 4
#define SBUFSIZE 16

struct conn_info
{
	int connfd; /* Client File Descriptor */
	struct sockaddr_in clientaddr; /* Client Socket Address */
};

struct sbuf_t
{

	/* Prethreading */
	struct conn_info *buf;	   /* Buffer array; array of file descriptors */
	int shared_cnt;			   /* Item (active requests?) count. */
	int n;					   /* Maximum number of slots */
	int front;				   /* buf[(front + 1) % n] is first item */
	int rear;				   /* buf[rear % n] is last item */
	pthread_mutex_t mutex;	   /* pthread mutex. */
	pthread_cond_t cond_empty; /* Wait on empty buffer. */
	pthread_cond_t cond_full;  /* Wait on full buffer. */
};

/* Prototypes */
static void
client_error(int fd, const char *cause, int err_num,
			 const char *short_msg, const char *long_msg);
static char *
create_log_entry(const struct sockaddr_in *sockaddr,
							  const char *uri, int size);
static int
parse_uri(const char *uri, char **hostnamep, char **portp,
					 char **pathnamep);

static int
open_listen(int port);

static int
open_client(char *hostname, char *port, int connfd,
	 char *request, struct sockaddr_in clientaddr, char *request_url);

static void
doit(struct conn_info connection);

static char *
read_requesthdrs(rio_t *rp, char *request);

static void *
start_routine(void *vargp);

static void
sbuf_init(struct sbuf_t *sp, int n);

static void
sbuf_insert(struct sbuf_t *sp, int connfd, 
						struct sockaddr_in clientaddr);

static struct conn_info
sbuf_remove(struct sbuf_t *sp);


/* Log file */
static FILE *logfd;

/* Shared buffer */
struct sbuf_t *sbuf;

/*
 * Requires:
 *   argv[1] is a string representing a TCP port number (in decimal).
 *
 * Effects:
 *   Opens a listening socket to listen for HTTP requests 
 *   and creates the threads prior to checking for requests.
 *   Accepts requests continuously and inserts them
 *   into the shared buffer.
 */
int 
main(int argc, char **argv)
{
	/* Initialization */
	char client_hostname[MAXLINE], 
		haddrp[INET_ADDRSTRLEN];
	pthread_t threads;
	int connfd, error, listenfd, i, port;
	struct sockaddr_in clientaddr;
	socklen_t clientlen;

	
	/* Initialize logging */
	logfd = fopen("proxy.log", "a+");
	if (logfd == NULL) {
		return -1;
	}

	/* Check the arguments. */
	if (argc != 2) {
		fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
		exit(0);
	}
	
	port = atoi(argv[1]);
	
	/* Listen for request */
	listenfd = open_listen(port);

	if (listenfd < 0) {
		unix_error("open_listen error");
	}

	/* Ignore SIGPIPE */
	Signal(SIGPIPE, SIG_IGN);

	/* Initialize the shared buffer */
	sbuf_init(sbuf, SBUFSIZE);
	
	/* Create the threads for pre-threading */
	for (i = 0; i < NTHREADS; i++) {
		
		Pthread_create(&threads, NULL, start_routine, NULL);
	}

	/* Continuously accept requests */
	while (1) {
		clientlen = sizeof(struct sockaddr_in);

		/* Accept the connection */
		connfd = accept(listenfd, (struct sockaddr *)&clientaddr,
						&clientlen);

		/* Get the client information */
		error = getnameinfo((struct sockaddr *)(&clientaddr), clientlen,
						client_hostname, sizeof(client_hostname), NULL, 0, 0);
						
		/* Handle error upon failed client info retrieval */
		if (error != 0) {
			fprintf(stderr, "ERROR: %s\n", gai_strerror(error));
			Close(connfd);
			continue;
		}

		Inet_ntop(AF_INET, &clientaddr.sin_addr, haddrp,
				  INET_ADDRSTRLEN);
		
		/* Insert the connection into the shared buffer. */
		sbuf_insert(sbuf, connfd, clientaddr);

	}

	/* Return success. */
	return (0);
}

/*
 *	Requires:
 *		None.
 *	
 *	Effects:
 *		Continuously attempts to remove a connection from the
 *		shared buffer, then starts the proxy routine to write
 *		to the end server and transmit the response back to the client.
 */
void *
start_routine(void *vargp)
{
	(void)vargp;

	/* we should somehow be able to get client addr from connfd */
	Pthread_detach(pthread_self());
	while (1) {
		/* Remove a connection from the buffer */
		struct conn_info connfd_actual = sbuf_remove(sbuf);

		/* Delegate to doit routine */
		doit(connfd_actual);

		/* Close the client connection */
		Close(connfd_actual.connfd);
	}

	return (NULL);
}

/* 
 * Requires:
 *   sp is a pointer to the shared buffer.
 *
 * Effects:
 *   Atomically insert a connection into the shared buffer.
 */
void 
sbuf_insert(struct sbuf_t *sp, int connfd, struct sockaddr_in clientaddr)
{
	/* acquire lock */
	pthread_mutex_lock(&(sbuf->mutex));

	/* Check if full, wait*/
	while (sbuf->shared_cnt == sbuf->n) {
		pthread_cond_wait(&(sbuf->cond_empty), &(sbuf->mutex));
	}

	/* Insert connection into the buffer */
	sp->buf[(++sp->rear) % (sp->n)] = (struct conn_info){connfd, clientaddr};

	/* Update shared count variable. */
	sp->shared_cnt++;

	/* signal to the threads that they can remove and release lock */
	pthread_cond_broadcast(&sbuf->cond_full);
	pthread_mutex_unlock(&sbuf->mutex);
}

/* 
 * Requires:
 *   sp is a pointer to the shared buffer.
 *
 * Effects:
 *   Atomically remove and return a connection from the buffer.
 */
struct conn_info 
sbuf_remove(struct sbuf_t *sp)
{
	/* Acquire lock */
	pthread_mutex_lock(&sbuf->mutex);

	/* Check if empty, wait*/
	while (sbuf->shared_cnt == 0) {
		pthread_cond_wait(&(sbuf->cond_full), &(sbuf->mutex));
	}


	struct conn_info connection;
	int front = ++sp->front;
	int bottom = sp->n;
	int thing = (front) % (bottom);
	connection = (sp->buf[thing]); /* Remove item */

	/* Update count of active requests */
	sp->shared_cnt--;

	/* Signal main routine that it can insert connections and release lock */
	pthread_cond_broadcast(&sbuf->cond_empty);

	pthread_mutex_unlock(&sbuf->mutex);

	return (connection);
}

/* 
 * Requires:
 *   n is a positive integer. 
 *
 * Effects:
 *   Creates an empty, bounded, shared buffer with n slots
 *		that utilizing pre-threading
 */
void 
sbuf_init(struct sbuf_t *sp, int n)
{
	
	(void)sp;

	/* Allocates memory for the buffer */
	sbuf = Malloc(sizeof(struct sbuf_t));

	/* Set memory for array of connections */
	sbuf->buf = Calloc(n, sizeof(struct conn_info));

	/* Initialize the sbuf variables */
	sbuf->n = n; 
	sbuf->front = sbuf->rear = 0;
	sbuf->shared_cnt = 0;
	

	/* Initialize Pre-Threading */
	pthread_mutex_init(&sbuf->mutex, NULL);
	pthread_cond_init(&sbuf->cond_empty, NULL);
	pthread_cond_init(&sbuf->cond_full, NULL);

}

/* 
 * Requires:
 *   none. 
 *
 * Effects:
 *   Processes the client's request, and edits it to be appropriate
 *	 for the end server. Then, doit delegates the rest of the work to
 *	 open_client. 
 */
static void 
doit(struct conn_info connection)
{
	/* Initialization of Variables */
	struct sockaddr_in *clientaddr;
	char *hostnamep, *portp, *pathnamep, *request_url, *buf;
	rio_t rio;
	char method[MAXLINE], request[MAXBUF], version[MAXLINE],
		 temp_buf[MAXLINE];
	int fd, bufSize, temp_size;

	/* Allocate memory for buffer */
	buf = malloc(MAXLINE + 1);

	/* Declare connection */
	fd = connection.connfd;
	clientaddr = &connection.clientaddr;

	/* Initialize empty read buffer */
	rio_readinitb(&rio, fd);

	/* Reads the request line and copies to buffer */
	bufSize = rio_readlineb(&rio, buf, MAXLINE);

	/* Handle a request longer than MAXLINE */
	if (bufSize == MAXLINE - 1) {
		temp_size = bufSize;
		while(temp_buf[temp_size - 1] != '\n') {
			if ((temp_size = rio_readlineb(&rio, temp_buf, MAXLINE)) == -1) {
				free(buf);
				fprintf(stdout, "rio_readlineb Error: Connection closed!\n");
				return;
			}
			bufSize += temp_size;
			buf = realloc(buf, MAXLINE + 1);
			sprintf(buf, "%s%s", buf, temp_buf);
		}

	}
	
	/* Malloc space for the Request URL */
	request_url = malloc(bufSize);

	/* Format Buf */
	sscanf(buf, "%s %s %s", method, request_url, version);
	
	/* If request is not a GET request */
	if (strstr(method, "GET") == NULL) {
		client_error(fd, "Request is not a GET request.", 400, "Bad Request",
		 "The given request does not contain a 'GET'. Try again!");
		free(buf);
		free(request_url);
	/* Read the HTTP request line by line */
	} else if (parse_uri(request_url, &hostnamep, &portp, &pathnamep) == -1) {
		client_error(fd, "Requested file cannot be found.", 404, "Not found",
			 "The requested file does not exist.");
		free(buf);
		free(request_url);
	} else {
		/* Format client request */
		sprintf(request, "GET %s %s\r\n", pathnamep, version);

		/* Read the request headers */
		read_requesthdrs(&rio, request);

		/* Open the client and continue to connect to the server */
		open_client(hostnamep, portp, fd, request, *clientaddr, request_url);

		/* Free */
		free(hostnamep);
		free(portp);
		free(pathnamep);
		free(buf);
		free(request_url);
	}

	
}


/* 
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Reads the headers of a HTTP request and uses them to build
 *	 the reformatted request string.
 */
static char *
read_requesthdrs(rio_t *rp, char *request)
{
	/* Initialize buffer */
	char buf[MAXLINE];

	/* Read to the buffer */
	rio_readlineb(rp, buf, MAXLINE);

	/* Iterate until new line */
	while (strcmp(buf, "\r\n")) {

		/* Append headers that don't include "connection" */
		if (strstr(buf, "Connection") == NULL) {
			sprintf(request, "%s%s", request, buf);
		}
		/* Read the next line */
		rio_readlineb(rp, buf, MAXLINE);
	}

	/* specify a closed connection */
	sprintf(request, "%sConnection: close\r\n", request);

	return (NULL);
}

/*
 *	Requires:
 *		port is an unused TCP port number.
 *
 *	Effects:
 *		Opens and returns a listening socket on the specified port. Returns -1
 *		and sets errno on a Unix error.
 *
 */
static int
open_listen(int port)
{

	int listenfd, optval;
	struct sockaddr_in serveraddr;
	

	/* Set listenfd to a newly created stream socket. */
	if ((listenfd = socket(PF_INET, SOCK_STREAM, 0)) == -1)
		return (-1);
	/* Eliminate "Address already in use" error from bind(). */
	optval = 1;
	if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
				   (const void *)&optval, sizeof(int)) == -1)
		return (-1);
	memset(&serveraddr, 0, sizeof(serveraddr));

	/*
	 * Set the IP address in serveraddr to the special ANY IP address, and
	 * set the port to the input port.  Be careful to ensure that the IP
	 * address and port are in network byte order.
	 */
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_port = htons((unsigned short)port);
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

	/* Use bind() to set the address of listenfd to be serveraddr. */
	if (bind(listenfd, (SA *)&serveraddr, sizeof(serveraddr)) == -1)
		return (-1);

	/*
	 * Use listen() to ready the socket for accepting connection requests.
	 * Set the backlog to 8.
	 */
	if (listen(listenfd, 8) == -1)
		return (-1);

	return (listenfd);
}

/*
 * Requires:
 *   hostname points to a string representing a host name, and port points to
 *   a string representing a TCP port number.
 *
 * Effects:
 *   Opens a TCP connection to the server at <hostname, port>, and returns a
 *   file descriptor ready for reading and writing.  Returns -1 and sets
 *   errno on a Unix error.  Returns -2 on a DNS (getaddrinfo) error.
 */
static int
open_client(char *hostname, char *port, int connfd, char *request, 
			struct sockaddr_in clientaddr, char *request_url)
{
	struct addrinfo *ai, hints, *listp;
	int fd, error, n;
	rio_t rio;
	char buf[MAXLINE];

	/* Initialize the hints that should be passed to getaddrinfo(). */
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;  /* Open a connection ... */
	hints.ai_flags = AI_NUMERICSERV; /* ... using a numeric port arg. */
	hints.ai_flags |= AI_ADDRCONFIG; /* Recommended for connections */

	/*
	 * Use getaddrinfo() to get the server's address list (&listp).  On
	 * failure, return -2.
	 */
	error = getaddrinfo(hostname, port, &hints, &listp);
	if (error != 0) {
		fprintf(stderr, "getaddrinfo failed (%s:%s): %s\n",
				hostname, port, gai_strerror(error));
		return (-2);
	}


	/*
	 * Iterate over the address list for one that can be successfully
	 * connected to.
	 */
	for (ai = listp; ai != NULL; ai = ai->ai_next)
	{
		/*
		 * Create a new socket using ai's family, socktype, and
		 * protocol, and assign its descriptor to fd.  On failure,
		 * continue to the next ai.
		 */
		if ((fd = socket(ai->ai_family, ai->ai_socktype,
						 ai->ai_protocol)) == -1)
			continue;

		/*
		 * Try connecting to the server with ai's addr and addrlen.
		 * Break out of the loop if connect() succeeds.
		 */
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) != -1)
			break;

		/*
		 * Connect() failed.  Close the descriptor and continue to
		 * the next ai.
		 */
		if (close(fd) == -1)
			unix_error("close");
	}

	/* Print request to standard output */
	fprintf(stdout, "%s\n", request);


	rio_readinitb(&rio, fd);

	/* Write the request to the end server */
	rio_writen(fd, request, strlen(request));
	rio_writen(fd, "\r\n", 2);


	/* As the response is read from the server,
	   write the response back to the client and
	   keep track of the size of the response */
	int size_server = 0;
	while ((n = rio_readlineb(&rio, buf, MAXLINE)) != 0)
	{
		rio_writen(connfd, buf, n);
		size_server += strlen(buf);
	}

	/* Write to proxy log file */

	/* Create log entry upon receiving request */
	char *log_entry = create_log_entry(&clientaddr, request_url, size_server);

	/* Include new line character at the end of log entry */
	sprintf(log_entry, "%s\n", log_entry);

	/* Write and flush to the log file */
	fprintf(logfd, log_entry);
	fflush(logfd);

	/* Clean up.  Avoid memory leaks! */
	freeaddrinfo(listp);
	free(log_entry);
	Close(fd);


	if (ai == NULL) {
		/* All connection attempts failed. */
		return (-1);
	} else {
		/* The last connect succeeded. */
		return (fd);
	}
}

/*
 * Requires:
 *   The parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Given a URI from an HTTP proxy GET request (i.e., a URL), extract the
 *   host name, port, and path name.  Create strings containing the host name,
 *   port, and path name, and return them through the parameters "hostnamep",
 *   "portp", "pathnamep", respectively.  (The caller must free the memory
 *   storing these strings.)  Return -1 if there are any problems and 0
 *   otherwise.
 */
static int
parse_uri(const char *uri, char **hostnamep, char **portp, char **pathnamep)
{
	const char *pathname_begin, *port_begin, *port_end;

	if (strncasecmp(uri, "http://", 7) != 0)
		return (-1);

	/* Extract the host name. */
	const char *host_begin = uri + 7;
	const char *host_end = strpbrk(host_begin, ":/ \r\n");
	if (host_end == NULL)
		host_end = host_begin + strlen(host_begin);
	int len = host_end - host_begin;
	char *hostname = Malloc(len + 1);
	strncpy(hostname, host_begin, len);
	hostname[len] = '\0';
	*hostnamep = hostname;

	/* Look for a port number.  If none is found, use port 80. */
	if (*host_end == ':') {
		port_begin = host_end + 1;
		port_end = strpbrk(port_begin, "/ \r\n");
		if (port_end == NULL)
			port_end = port_begin + strlen(port_begin);
		len = port_end - port_begin;
	} else {
		port_begin = "80";
		port_end = host_end;
		len = 2;
	}
	char *port = Malloc(len + 1);
	strncpy(port, port_begin, len);
	port[len] = '\0';
	*portp = port;

	/* Extract the path. */
	if (*port_end == '/') {
		pathname_begin = port_end;
		const char *pathname_end = strpbrk(pathname_begin, " \r\n");
		if (pathname_end == NULL)
			pathname_end = pathname_begin + strlen(pathname_begin);
		len = pathname_end - pathname_begin;
	} else {
		pathname_begin = "/";
		len = 1;
	}
	
	char *pathname = Malloc(len + 1);
	strncpy(pathname, pathname_begin, len);
	pathname[len] = '\0';
	*pathnamep = pathname;

	return (0);
}

/*
 * Requires:
 *   The parameter "sockaddr" must point to a valid sockaddr_in structure.  The
 *   parameter "uri" must point to a properly NUL-terminated string.
 *
 * Effects:
 *   Returns a string containing a properly formatted log entry.  This log
 *   entry is based upon the socket address of the requesting client
 *   ("sockaddr"), the URI from the request ("uri"), and the size in bytes of
 *   the response from the server ("size").
 */
static char *
create_log_entry(const struct sockaddr_in *sockaddr, const char *uri, int size)
{
	struct tm result;

	/*
	 * Create a large enough array of characters to store a log entry.
	 * Although the length of the URI can exceed MAXLINE, the combined
	 * lengths of the other fields and separators cannot.
	 */
	const size_t log_maxlen = MAXLINE + strlen(uri);
	char *const log_str = Malloc(log_maxlen + 1);

	/* Get a formatted time string. */
	time_t now = time(NULL);
	int log_strlen = strftime(log_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z: ",
							  localtime_r(&now, &result));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form.
	 */
	Inet_ntop(AF_INET, &sockaddr->sin_addr, &log_str[log_strlen],
			  INET_ADDRSTRLEN);
	log_strlen += strlen(&log_str[log_strlen]);

	/*
	 * Assert that the time and IP address fields occupy less than half of
	 * the space that is reserved for the non-URI fields.
	 */
	assert(log_strlen < MAXLINE / 2);

	/*
	 * Add the URI and response size onto the end of the log entry.
	 */
	snprintf(&log_str[log_strlen], log_maxlen - log_strlen, " %s %d", uri,
			 size);

	return (log_str);
}

/*
 * Requires:
 *   The parameter "fd" must be an open socket that is connected to the client.
 *   The parameters "cause", "short_msg", and "long_msg" must point to properly 
 *   NUL-terminated strings that describe the reason why the HTTP transaction
 *   failed.  The string "short_msg" may not exceed 32 characters in length,
 *   and the string "long_msg" may not exceed 80 characters in length.
 *
 * Effects:
 *   Constructs an HTML page describing the reason why the HTTP transaction
 *   failed, and writes an HTTP/1.0 response containing that page as the
 *   content.  The cause appearing in the HTML page is truncated if the
 *   string "cause" exceeds 2048 characters in length.
 */
static void
client_error(int fd, const char *cause, int err_num, const char *short_msg,
			 const char *long_msg)
{
	char body[MAXBUF], headers[MAXBUF], truncated_cause[2049];

	assert(strlen(short_msg) <= 32);
	assert(strlen(long_msg) <= 80);
	/* Ensure that "body" is much larger than "truncated_cause". */
	assert(sizeof(truncated_cause) < MAXBUF / 2);

	/*
	 * Create a truncated "cause" string so that the response body will not
	 * exceed MAXBUF.
	 */
	strncpy(truncated_cause, cause, sizeof(truncated_cause) - 1);
	truncated_cause[sizeof(truncated_cause) - 1] = '\0';

	/* Build the HTTP response body. */
	snprintf(body, MAXBUF,
			 "<html><title>Proxy Error</title><body bgcolor="
			 "ffffff"
			 ">\r\n"
			 "%d: %s\r\n"
			 "<p>%s: %s\r\n"
			 "<hr><em>The COMP 321 Web proxy</em>\r\n",
			 err_num, short_msg, long_msg, truncated_cause);

	/* Build the HTTP response headers. */
	snprintf(headers, MAXBUF,
			 "HTTP/1.0 %d %s\r\n"
			 "Content -type: text/html\r\n"
			 "Content-length: %d\r\n"
			 "\r\n",
			 err_num, short_msg, (int)strlen(body));

	/* Write the HTTP response. */
	if (rio_writen(fd, headers, strlen(headers)) != -1)
		rio_writen(fd, body, strlen(body));
}

