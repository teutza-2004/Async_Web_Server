// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;
struct io_event events[1];

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	// Prepare the connection buffer to send the reply header.
	memset(conn->send_buffer, 0, BUFSIZ);
	sprintf(conn->send_buffer, "HTTP/1.1 200 OK\r\n"
								"Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
								"Server: Apache/2.2.9\r\n"
								"Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
								"Accept-Ranges: bytes\r\n"
								"Vary: Accept-Encoding\r\n"
								"Connection: close\r\n"
								"Content-Type: text/html\r\n"
								"Content-Length: %ld\r\n\r\n", conn->file_size);
	conn->send_len = strlen(conn->send_buffer);

	conn->state = STATE_SENDING_DATA;
}

static void connection_prepare_send_404(struct connection *conn)
{
	// Prepare the connection buffer to send the 404 header.
	memset(conn->send_buffer, 0, BUFSIZ);
	sprintf(conn->send_buffer, "HTTP/1.1 404 Not Found\r\n"
								"Date: Sun, 08 May 2011 09:26:16 GMT\r\n"
								"Server: Apache/2.2.9\r\n"
								"Last-Modified: Mon, 02 Aug 2010 17:55:28 GMT\r\n"
								"Accept-Ranges: bytes\r\n"
								"Vary: Accept-Encoding\r\n"
								"Connection: close\r\n"
								"Content-Type: text/html\r\n\r\n");
	conn->send_len = strlen(conn->send_buffer);
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	// If the path contain static, when is a static file same for dynamic.
	if (strstr(conn->request_path, "static") != NULL)
		return RESOURCE_TYPE_STATIC;

	if (strstr(conn->request_path, "dynamic") != NULL)
		return RESOURCE_TYPE_DYNAMIC;

	return RESOURCE_TYPE_NONE;
}


struct connection *connection_create(int sockfd)
{
	// Create a new conection and set memory to 0.
	struct connection *connect = calloc(1, sizeof(*connect));

	connect->sockfd = sockfd;
	return connect;
}

void connection_start_async_io(struct connection *conn)
{
	// If connection first time enter in this state, when intialize the IO
	if (conn->state == STATE_SENDING_HEADER) {
		ssize_t bytes_sent;
		int len_send = 0;

		conn->piocb[0] = &conn->iocb;
		conn->eventfd = eventfd(0, 0);
		io_set_eventfd(&conn->iocb, conn->eventfd);

		io_setup(1, &ctx);
		conn->ctx = ctx;
		connection_prepare_send_reply_header(conn);

		while (conn->send_len > 0) {
			bytes_sent = send(conn->sockfd, conn->send_buffer + len_send, conn->send_len, 0);

			len_send += bytes_sent;
			conn->send_len -= bytes_sent;
		}
	}

	// Read the file in buffer, if it excent the buffer size
	// it will read part by part until is done
	memset(conn->recv_buffer, 0, sizeof(char) * BUFSIZ);
	io_prep_pread(&conn->iocb, conn->fd, conn->recv_buffer, BUFSIZ, conn->file_pos);

	io_submit(conn->ctx, 1, &conn->piocb[0]);

	conn->state = STATE_ASYNC_ONGOING;
	w_epoll_update_ptr_inout(epollfd, conn->eventfd, conn);
}

void connection_remove(struct connection *conn)
{
	// Remove the connection, close the socket and free the memory
	close(conn->sockfd);
	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void handle_new_connection(void)
{
	// Create a new connection
	int sockedfd;
	int flags;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in addr;
	struct connection *conn;

	sockedfd = accept(listenfd, (SSA *) &addr, &addrlen);

	// Set the socket to non-blocking mod and add to epoll
	flags = fcntl(sockedfd, F_GETFL, 0);
	fcntl(sockedfd, F_SETFL, flags | O_NONBLOCK);

	conn = connection_create(sockedfd);

	conn->state = STATE_INITIAL;
	w_epoll_add_ptr_in(epollfd, sockedfd, conn);

	// Init the http parser
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = (void *)conn;
}

void receive_data(struct connection *conn)
{
	// Receive the data from the socket, until is done
	ssize_t bytes_recv;

	do {
		bytes_recv = recv(conn->sockfd, conn->recv_buffer + conn->recv_len, BUFSIZ, 0);
		if (bytes_recv < 0 && errno == EAGAIN)
			break;
		conn->recv_len += bytes_recv;
	} while (bytes_recv > 0);


	conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	int i;
	int path_len = strlen(conn->request_path);
	struct stat st;

	// Remove the '/' from the path so we can open it
	for (i = 0; i < path_len; i++)
		conn->request_path[i] = conn->request_path[i + 1];

	conn->fd = open(conn->request_path, O_RDONLY);
	// If the file not exist, send the 404 error
	if (conn->fd < 0) {
		conn->state = STATE_SENDING_404;
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		return -1;
	}

	// Else get the file size and store it
	fstat(conn->fd, &st);
	conn->file_size = st.st_size;

	conn->state = STATE_SENDING_HEADER;
	w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	// Wait the IO to be done
	io_getevents(conn->ctx, 1, 1, events, NULL);

	conn->state = STATE_SENDING_DATA;
	w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
}

int parse_header(struct connection *conn)
{
	// Parse the HTTP header and extract the file path.
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};

	http_parser_execute(&conn->request_parser, &settings_on_path, conn->recv_buffer, conn->recv_len);
	conn->state = STATE_RECEIVING_DATA;
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	// Send the static file
	ssize_t bytes_sent;
	off_t offset = 0;
	int len_send = 0;
	int len_sendfile = 0;

	// Send the http message
	while (conn->send_len > 0) {
		bytes_sent = send(conn->sockfd, conn->send_buffer + len_send, conn->send_len, 0);

		len_send += bytes_sent;
		conn->send_len -= bytes_sent;
	}

	// Send the file
	while (len_sendfile < conn->file_size) {
		bytes_sent = sendfile(conn->sockfd, conn->fd, &offset, conn->file_size);
		if (bytes_sent < 0 && errno == EAGAIN)
			bytes_sent = 0;
		len_sendfile += bytes_sent;
	}

	conn->state = STATE_CONNECTION_CLOSED;
	w_epoll_add_ptr_out(epollfd, conn->sockfd, conn);

	return STATE_CONNECTION_CLOSED;
}

int connection_send_dynamic(struct connection *conn)
{
	ssize_t bytes_sent;
	int len_send = 0;

	// Get the data from the file and send it to the socket until is done
	conn->send_len = events[0].res;
	conn->file_pos += conn->send_len;

	while (conn->send_len > 0) {
		bytes_sent = send(conn->sockfd, conn->recv_buffer + len_send, conn->send_len, 0);

		len_send += bytes_sent;
		conn->send_len -= bytes_sent;
	}

	// Start a new IO operation if needed
	if (conn->file_pos < conn->file_size) {
		connection_start_async_io(conn);
	} else {
		conn->state = STATE_CONNECTION_CLOSED;
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	}

	return 0;
}


void handle_input(struct connection *conn)
{
	// Take the input from the socket and process it
	int rc;

	switch (conn->state) {
	case STATE_INITIAL:
		receive_data(conn);

	case STATE_REQUEST_RECEIVED:
		parse_header(conn);
		conn->res_type = connection_get_resource_type(conn);

	case STATE_RECEIVING_DATA:
		rc = connection_open_file(conn);
		if (conn->res_type == RESOURCE_TYPE_DYNAMIC && rc == 0)
			connection_start_async_io(conn);
		break;

	default:
		printf("shouldn't get here %d\n", conn->state);
	}
}

void handle_output(struct connection *conn)
{
	// Send data to socket dynamic or static when close the connection
	switch (conn->state) {
	case STATE_SENDING_404:
		connection_prepare_send_404(conn);
		connection_send_static(conn);
		break;

	case STATE_SENDING_HEADER:
		connection_prepare_send_reply_header(conn);

	case STATE_SENDING_DATA:
		if (conn->res_type != RESOURCE_TYPE_DYNAMIC)
			connection_send_static(conn);
		else
			connection_send_dynamic(conn);
		break;

	case STATE_CONNECTION_CLOSED:
		connection_remove(conn);
		break;

	case STATE_ASYNC_ONGOING:
		connection_complete_async_io(conn);
		break;

	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	// Check the event if is for input or output
	if (event == EPOLLIN)
		handle_input(conn);
	if (event == EPOLLOUT)
		handle_output(conn);
}

int main(void)
{
	// Create the epoll and socket for server
	epollfd = w_epoll_create();
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);

	w_epoll_add_fd_in(epollfd, listenfd);

	// Server main loop
	while (1) {
		struct epoll_event rev;

		// Wait for an event then find if is a new conection or need to be processed
		w_epoll_wait_infinite(epollfd, &rev);

		if (rev.data.fd == listenfd)
			handle_new_connection();
		else
			handle_client(rev.events, rev.data.ptr);
	}
	return 0;
}