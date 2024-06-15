#include <unistd.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>

static void die(void) {
	char message[] = "Fatal error\n";
	write(2, message, sizeof message - 1);
	exit(1);
}

typedef struct {
	char *data;
	int size;
} string;

typedef struct {
	string string;
	int written;
} pending_write;

typedef struct list {
	union {
		char character;
		pending_write pending_write;
	};
	struct list *next;
} list;

static void *allocate(int size) {
	void *result = malloc(size);
	if (result == 0)
		die();
	return result;
}

list *new_pending_write_list(string s) {
	list *l = allocate(sizeof(list));
	*l = (list) { .pending_write = (pending_write) { .string = s } };
	return l;
}

list *new_character_list(char c) {
	list *l = allocate(sizeof(list));
	*l = (list) { .character = c };
	return l;
}

typedef struct {
	list *front;
	list *back;
} queue;

static void enqueue(queue *q, list *l) {
	if (q->back)
		q->back->next = l;
	else
		q->front = l;
	q->back = l;
}

static list *dequeue(queue *q) {
	list *result = q->front;
	q->front = q->front->next;
	if (q->front == 0)
		q->back = 0;
	return result;
}

static struct client {
	_Bool live;
	int number;
	queue characters;
	queue pending_writes;
} clients[FD_SETSIZE], *client;

static string clone(string s) {
	char *data = allocate(s.size + 1);
	data[s.size] = 0;
	for (int i = 0; i < s.size; i++)
		data[i] = s.data[i];
	return (string) {
		.data = data,
		.size = s.size,
	};
}

static int size(char *s) {
	int result = 0;
	for (;;) {
		if (s[result] == 0)
			return result;
		result++;
	}
}

int get_formatted_int_size(int i) {
	int result = 1;
	while (i != 0) {
		i /= 10;
		result++;
	}
	return result;
}

static int get_formatted_size(char *format, va_list arguments) {
	int result = 0;
	for (int i = 0; format[i]; i++)
		switch (format[i]) {
			default:
				result++;
				break;
			case '%':
				switch (format[i + 1]) {
					case 'd':
						result += get_formatted_int_size(va_arg(arguments, int));
						i++;
						break;
					case 's':
						result += size(va_arg(arguments, char *));
						i++;
						break;
					default:
						die();
				}
		}
	return result;
}

static string do_format(char *format, va_list arguments) {
	va_list copy;
	va_copy(copy, arguments);
	int size = get_formatted_size(format, copy);
	char *data = allocate(size + 1);
	size = vsprintf(data, format, arguments);
	printf("formatted: %s", data);
	va_end(copy);
	return (string) {
		.data = data,
		.size = size,
	};
}

static int listening_socket, total_visitors, i;

static void notify(char *format, ...) {
	va_list arguments;
	va_start(arguments, format);
	string s = do_format(format, arguments);
	for (int j = 0; j < FD_SETSIZE; j++)
		if (j != listening_socket && j != i && clients[j].live) {
			enqueue(
				&clients[j].pending_writes,
				new_pending_write_list(clone(s))
			);
		}
	va_end(arguments);
	free(s.data);
}

static fd_set may_need_to_read_from, may_need_to_write_to, can_read_from, can_write_to;

static void do_listening_socket(void) {
	if (FD_ISSET(i, &can_read_from)) {
		int new = accept(
			i,
			(struct sockaddr *)&(struct sockaddr_storage) { 0 },
			&(socklen_t) { sizeof(struct sockaddr_storage) }
		);
		if (new == -1)
			return;
		client = clients + new;
		*client = (struct client) {
			.live = 1,
			.number = total_visitors,
		};
		total_visitors++;
		i = new;
		notify("server: client %d joined\n", client->number);
		FD_SET(new, &may_need_to_read_from);
		FD_SET(new, &may_need_to_write_to);
	}
}

static void do_client_socket(void) {
	if (FD_ISSET(i, &can_read_from)) {
		char buffer[2 << 10];
		int read = recv(i, buffer, sizeof buffer, 0);
		if (read == -1)
			return;
		if (read == 0) {
			client->live = 0;
			close(i);
			notify("server: client %d left\n", client->number);
			FD_CLR(i, &may_need_to_read_from);
			FD_CLR(i, &may_need_to_write_to);
			for (queue *q = &client->pending_writes; q->front; free(dequeue(q)))
				free(q->front->pending_write.string.data);
			for (queue *q = &client->characters; q->front; free(dequeue(q)))
				;
		} else {
			queue *q = &client->characters;
			for (int r = 0; r < read; r++) {
				enqueue(q, new_character_list(buffer[r]));
				if (buffer[r] == '\n') {
					int size = 0;
					for (list *l = q->front; l; l = l->next)
						size++;
					char *data = allocate(size + 1);
					data[size] = 0;
					for (int s = 0; s < size; s++) {
						list *l = dequeue(q);
						data[s] = l->character;
						free(l);
					}
					printf("data: %s", data);
					notify("client %d: %s", client->number, data);
					free(data);
				}
			}
		}
	}
	if (FD_ISSET(i, &can_write_to)) {
		queue *q = &client->pending_writes;
		if (q->front) {
			pending_write *pw = &q->front->pending_write;
			int written = send(
				i, pw->string.data + pw->written, pw->string.size - pw->written, 0
			);
			if (written == -1)
				return;
			pw->written += written;
			if (pw->written == pw->string.size) {
				free(pw->string.data);
				free(dequeue(q));
			}
		}
	}
}

static void do_select(void) {
	can_read_from = may_need_to_read_from;
	can_write_to = may_need_to_write_to;
	select(FD_SETSIZE, &can_read_from, &can_write_to, 0, 0);
	for (i = 0; i < FD_SETSIZE; i++)
		if (i == listening_socket)
			do_listening_socket();
		else {
			client = clients + i;
			do_client_socket();
		}
}

int main(void) {
	listening_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (listening_socket == -1)
		die();
#ifdef debugging
	if (setsockopt(listening_socket, SOL_SOCKET, SO_REUSEADDR, &(int) { 1 }, sizeof(int)) == -1)
		die();
#endif
	if (bind(
		listening_socket,
		(struct sockaddr *)&(struct sockaddr_in) {
			.sin_family = AF_INET,
			.sin_addr = htonl((127 << 24) + 0 + 0 + 1),
			.sin_port = htons(8000),
		},
		sizeof(struct sockaddr_in)
	) == -1)
		die();
	if (listen(listening_socket, SOMAXCONN) == -1)
		die();
	FD_SET(listening_socket, &may_need_to_read_from);
	for (;;)
		do_select();
}
