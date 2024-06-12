#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdarg.h>

#ifdef debugging
#include <assert.h>
#endif

int decimal_size(int i) {
	int size = 1;
	while (i != 0) {
		i /= 10;
		size++;
	}
	return size;
}

int formatted_message_size(char *format, va_list arguments) {
	int size = 0;
	for (int i = 0; format[i]; i++) {
		switch (format[i]) {
		case '%': {
			switch (format[i + 1]) {
			case 's': {
				size += strlen(va_arg(arguments, char *));
				i++;
				break;
			}
			case 'd': {
				size += decimal_size(va_arg(arguments, int));
				i++;
				break;
			}
			}
			break;
		}
		default:
			size++;
			break;
		}
	}
	return size;
}

void *try_malloc(int size) {
	void *result = malloc(size);
	if (result == 0) {
		write(2, "Fatal error\n", sizeof "Fatal error\n" - 1);
		exit(1);
	}
	return result;
}

static char *format_message_given_arguments_list(char *format, va_list arguments) {
	va_list copy;
	va_copy(copy, arguments);
	int size = formatted_message_size(format, copy);
	char *message = try_malloc(size + 1);
	vsprintf(message, format, arguments);
	va_end(copy);
	return message;
}

static char *format_message(char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	char *result = format_message_given_arguments_list(format, arguments);
	va_end(arguments);
	return result;
}

static ssize_t try(int i){
	if (i == -1){
#ifdef debugging
		char *message = format_message("Fatal error: %s\n", strerror(errno));
		int message_size = strlen(message);
		write(2, message, message_size);
		free(message);
#else
		write(2, "Fatal error\n", sizeof "Fatal error\n" - 1);
#endif
		exit(1);
	}
	return i;
}

static int try_in_debug(int i) {
#ifdef debugging
	if (i == -1){
		char *message = format_message("debug: error: %s\n", strerror(errno));
		int message_size = strlen(message);
		write(2, message, message_size);
		free(message);
		exit(1);
	}
#endif
	return i;
}

typedef struct list {
	void *data;
	struct list *next;
} list;

list *make_list(void *data) {
	list *l = try_malloc(sizeof(list));
	l->data = data;
	l->next = 0;
	return l;
}

typedef struct queue {
	list *front;
	list *back;
} queue;

void enqueue(queue *q, list *l) {
	if (q->back)
		q->back->next = l;
	else
		q->front = l;
	q->back = l;
}

list *dequeue(queue *q) {
	list *result = q->front;
	q->front = q->front->next;
	if (q->front == 0)
		q->back = 0;
	return result;
}

static fd_set reads[1], readees[1], writes[1], writees[1], errors[1], errorees[1];
static int number, listener, i;

typedef struct {
	char *data;
	int size;
	int written;
} line;

line *make_line(char *message, int size) {
	line *l = try_malloc(sizeof(line));
	*l = (line) {
		.data = message,
		.size = size,
	};
	return l;
}

char *make_char(char c) {
	char *result = try_malloc(1);
	*result = c;
	return result;
}

static struct client {
	queue chars_in;
	queue lines_out;
	_Bool live;
	int number;
} clients[FD_SETSIZE], *c;

char *clone(char *string, int size) {
	char *result = try_malloc(size + 1);
	result[size] = 0;
	for (int i = 0; i < size; i++)
		result[i] = string[i];
	return result;
}

static void notify(char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	char *message = format_message_given_arguments_list(format, arguments);
	int message_size = strlen(message);
	for (int j = 0; j < FD_SETSIZE; j++)
		if (j != i && j != listener && clients[j].live) {
			enqueue(
				&clients[j].lines_out,
				make_list(make_line(clone(message, message_size), message_size))
			);
		}
	free(message);
	va_end(arguments);
}

static void client(void) {
	if (FD_ISSET(i, reads)) {
		char buffer[(2 << 10) + 1];
		int size = try_in_debug(recv(i, buffer, sizeof buffer - 1, 0));
		if (size == 0) {
			try_in_debug(close(i));
			FD_CLR(i, readees);
			FD_CLR(i, writees);
			FD_CLR(i, errorees);
			c->live = 0;
			notify("server: client %d left\n", c->number);
			{
				queue *q = &c->chars_in;
				while (q->front) {
					free(q->front->data);
					free(dequeue(q));
				}
			}
			{
				queue *q = &c->lines_out;
				while (q->front) {
					line *l = (line *)q->front->data;
					free(l->data);
					free(l);
					free(dequeue(q));
				}
			}
		}
		else
			for (int i = 0; i < size; i++) {
				enqueue(&c->chars_in, make_list(make_char(buffer[i])));
				if (buffer[i] == '\n') {
					queue *q = &c->chars_in;
					int size = 0;
					for (list *l = q->front; l; l = l->next)
						size++;
					char *line = try_malloc(size + 1);
					line[size] = 0;
					for (int i = 0; i < size; i++) {
						list *l = dequeue(q);
						char *ch = l->data;
						line[i] = *ch;
						free(l->data);
						free(l);
					}
					notify("client %d: %s", c->number, line);
					free(line);
				}
			}
	}
	if (FD_ISSET(i, writes)) {
		list *front = c->lines_out.front;
		if (front) {
			line *l = front->data;
			int sent = try_in_debug(send(i, l->data + l->written, l->size - l->written, 0));
			l->written += sent;
			if (l->written == l->size) {
				free(l->data);
				free(l);
				free(dequeue(&c->lines_out));
			}
		}
	}
	if (FD_ISSET(i, errors))
#ifdef debugging
		assert(0)
#endif
		;
}

static void server(void) {
	if (FD_ISSET(i, reads)) {
		i = try_in_debug(
			accept(
				listener,
				(struct sockaddr *)&(struct sockaddr_in){ 0 },
				&(socklen_t){ sizeof(struct sockaddr_in) }
			)
		);
		FD_SET(i, readees);
		FD_SET(i, writees);
		FD_SET(i, errorees);
		c = clients + i;
		c->live = 1;
		c->number = number;
		number++;
		notify("server: client %d joined\n", c->number);
	}
	if (FD_ISSET(i, writes))
#ifdef debugging
		assert(0)
#endif
		;
	if (FD_ISSET(i, errors))
#ifdef debugging
		assert(0)
#endif
		;
}

static void events(void) {
	*reads = *readees;
	*writes = *writees;
	*errors = *errorees;
	try_in_debug(select(FD_SETSIZE, reads, writes, errors, 0));
	for (i = 0; i < FD_SETSIZE; i++) {
		c = clients + i;
		if (i == listener)
			server();
		else
			client();
	}
}

int main(){
	listener = try(socket(AF_INET, SOCK_STREAM, 0));
#ifdef debugging
	try(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)));
#endif
	try(bind(
		listener,
		(struct sockaddr *)&(struct sockaddr_in){
			.sin_family = AF_INET,
			.sin_port = htons(8000),
			.sin_addr = htonl((127 << 24) + 0 + 0 + 1),
		},
		sizeof(struct sockaddr_in)
	));
	try(listen(listener, SOMAXCONN));
	FD_SET(listener, readees);
	FD_SET(listener, writees);
	FD_SET(listener, errorees);
	for (;;)
		events();
}
