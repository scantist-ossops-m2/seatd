#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "backend.h"
#include "connection.h"
#include "libseat.h"
#include "list.h"
#include "log.h"
#include "protocol.h"

#ifdef BUILTIN_ENABLED
#include "poller.h"
#include "server.h"
#endif

const struct libseat_impl seatd_impl;
const struct libseat_impl builtin_impl;

struct pending_event {
	int opcode;
};

struct backend_seatd {
	struct libseat base;
	struct connection connection;
	struct libseat_seat_listener *seat_listener;
	void *seat_listener_data;
	struct list pending_events;

	char seat_name[MAX_SEAT_LEN];
};

static int set_nonblock(int fd) {
	int flags;
	if ((flags = fcntl(fd, F_GETFD)) == -1 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) {
		return -1;
	}
	if ((flags = fcntl(fd, F_GETFL)) == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
		return -1;
	}
	return 0;
}

static int seatd_connect(void) {
	union {
		struct sockaddr_un unix;
		struct sockaddr generic;
	} addr = {0};
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		return -1;
	}
	if (set_nonblock(fd) == -1) {
		close(fd);
		return -1;
	}
	char *path = getenv("SEATD_SOCK");
	if (path == NULL) {
		path = "/run/seatd.sock";
	}
	addr.unix.sun_family = AF_UNIX;
	strncpy(addr.unix.sun_path, path, sizeof addr.unix.sun_path);
	socklen_t size = offsetof(struct sockaddr_un, sun_path) + strlen(addr.unix.sun_path);
	if (connect(fd, &addr.generic, size) == -1) {
		close(fd);
		return -1;
	};
	return fd;
}

static struct backend_seatd *backend_seatd_from_libseat_backend(struct libseat *base) {
	assert(base);
#ifdef BUILTIN_ENABLED
	assert(base->impl == &seatd_impl || base->impl == &builtin_impl);
#else
	assert(base->impl == &seatd_impl);
#endif
	return (struct backend_seatd *)base;
}

static void handle_enable_seat(struct backend_seatd *backend) {
	if (backend->seat_listener != NULL && backend->seat_listener->enable_seat != NULL) {
		backend->seat_listener->enable_seat(&backend->base, backend->seat_listener_data);
	}
}

static void handle_disable_seat(struct backend_seatd *backend) {
	if (backend->seat_listener != NULL && backend->seat_listener->disable_seat != NULL) {
		backend->seat_listener->disable_seat(&backend->base, backend->seat_listener_data);
	}
}

static size_t read_header(struct connection *connection, uint16_t expected_opcode) {
	struct proto_header header;
	if (connection_get(connection, &header, sizeof header) == -1) {
		return SIZE_MAX;
	}
	if (header.opcode != expected_opcode) {
		connection_restore(connection, sizeof header);
		errno = EBADMSG;
		return SIZE_MAX;
	}

	return header.size;
}

static int queue_event(struct backend_seatd *backend, int opcode) {
	struct pending_event *ev = calloc(1, sizeof(struct pending_event));
	if (ev == NULL) {
		return -1;
	}

	ev->opcode = opcode;
	list_add(&backend->pending_events, ev);
	return 0;
}

static void execute_events(struct backend_seatd *backend) {
	while (backend->pending_events.length > 0) {
		struct pending_event *ev = list_pop_front(&backend->pending_events);
		int opcode = ev->opcode;
		free(ev);

		switch (opcode) {
		case SERVER_DISABLE_SEAT:
			handle_disable_seat(backend);
			break;
		case SERVER_ENABLE_SEAT:
			handle_enable_seat(backend);
			break;
		default:
			abort();
		}
	}
}

static int dispatch_pending(struct backend_seatd *backend, int *opcode) {
	int packets = 0;
	struct proto_header header;
	while (connection_get(&backend->connection, &header, sizeof header) != -1) {
		packets++;
		switch (header.opcode) {
		case SERVER_DISABLE_SEAT:
		case SERVER_ENABLE_SEAT:
			queue_event(backend, header.opcode);
			break;
		default:
			if (opcode != NULL &&
			    connection_pending(&backend->connection) >= header.size) {
				*opcode = header.opcode;
			}
			connection_restore(&backend->connection, sizeof header);
			return packets;
		}
	}
	return packets;
}

static int poll_connection(struct backend_seatd *backend, int timeout) {
	struct pollfd fd = {
		.fd = backend->connection.fd,
		.events = POLLIN,
	};

	if (poll(&fd, 1, timeout) == -1) {
		return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
	}

	if (fd.revents & (POLLERR | POLLHUP)) {
		errno = EPIPE;
		return -1;
	}

	int len = 0;
	if (fd.revents & POLLIN) {
		len = connection_read(&backend->connection);
		if (len == 0 || (len == -1 && errno != EAGAIN)) {
			return -1;
		}
	}

	return len;
}

static int dispatch(struct backend_seatd *backend) {
	if (connection_flush(&backend->connection) == -1) {
		return -1;
	}
	int opcode = 0;
	while (dispatch_pending(backend, &opcode) == 0 && opcode == 0) {
		if (poll_connection(backend, -1) == -1) {
			return -1;
		}
	}
	return 0;
}

static void check_error(struct connection *connection) {
	struct proto_header header;
	if (connection_get(connection, &header, sizeof header) == -1) {
		return;
	}
	if (header.opcode != SERVER_ERROR) {
		errno = EBADMSG;
		return;
	}

	struct proto_server_error msg;
	if (connection_get(connection, &msg, sizeof msg) == -1) {
		return;
	}

	errno = msg.error_code;
}

static int get_fd(struct libseat *base) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);
	return backend->connection.fd;
}

static int dispatch_background(struct libseat *base, int timeout) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);
	int dispatched = dispatch_pending(backend, NULL);
	if (dispatched > 0) {
		// We don't want to block if we dispatched something, as the
		// caller might be waiting for the result. However, we'd also
		// like to read anything pending.
		timeout = 0;
	}
	int read = 0;
	if (timeout == 0) {
		read = connection_read(&backend->connection);
	} else {
		read = poll_connection(backend, timeout);
	}
	if (read > 0) {
		dispatched += dispatch_pending(backend, NULL);
	} else if (read == -1 && errno != EAGAIN) {
		return -1;
	}

	execute_events(backend);
	return dispatched;
}

static void destroy(struct backend_seatd *backend) {
	if (backend->connection.fd != -1) {
		close(backend->connection.fd);
		backend->connection.fd = -1;
	}
	connection_close_fds(&backend->connection);
	for (size_t idx = 0; idx < backend->pending_events.length; idx++) {
		free(backend->pending_events.items[idx]);
	}
	list_free(&backend->pending_events);
	free(backend);
}

static struct libseat *_open_seat(struct libseat_seat_listener *listener, void *data, int fd) {
	struct backend_seatd *backend = calloc(1, sizeof(struct backend_seatd));
	if (backend == NULL) {
		close(fd);
		return NULL;
	}
	backend->seat_listener = listener;
	backend->seat_listener_data = data;
	backend->connection.fd = fd;
	backend->base.impl = &seatd_impl;
	list_init(&backend->pending_events);

	struct proto_header header = {
		.opcode = CLIENT_OPEN_SEAT,
		.size = 0,
	};

	if (connection_put(&backend->connection, &header, sizeof header) == -1 ||
	    dispatch(backend) == -1) {
		destroy(backend);
		return NULL;
	}

	size_t size = read_header(&backend->connection, SERVER_SEAT_OPENED);
	if (size == SIZE_MAX) {
		check_error(&backend->connection);
		destroy(backend);
		return NULL;
	}

	struct proto_server_seat_opened rmsg;
	if (sizeof rmsg > size) {
		errno = EBADMSG;
		return NULL;
	}

	if (connection_get(&backend->connection, &rmsg, sizeof rmsg) == -1) {
		return NULL;
	};

	if (sizeof rmsg + rmsg.seat_name_len > size ||
	    rmsg.seat_name_len >= sizeof backend->seat_name) {
		errno = EBADMSG;
		return NULL;
	}

	if (connection_get(&backend->connection, backend->seat_name, rmsg.seat_name_len) == -1) {
		return NULL;
	};

	return &backend->base;
}

static struct libseat *open_seat(struct libseat_seat_listener *listener, void *data) {
	int fd = seatd_connect();
	if (fd == -1) {
		return NULL;
	}

	return _open_seat(listener, data, fd);
}

static int close_seat(struct libseat *base) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);

	struct proto_header header = {
		.opcode = CLIENT_CLOSE_SEAT,
		.size = 0,
	};

	if (connection_put(&backend->connection, &header, sizeof header) == -1 ||
	    dispatch(backend) == -1) {
		destroy(backend);
		return -1;
	}

	size_t size = read_header(&backend->connection, SERVER_SEAT_CLOSED);
	if (size == SIZE_MAX) {
		check_error(&backend->connection);
		destroy(backend);
		return -1;
	}

	destroy(backend);
	return 0;
}

static const char *seat_name(struct libseat *base) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);
	return backend->seat_name;
}

static int open_device(struct libseat *base, const char *path, int *fd) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);

	size_t pathlen = strlen(path) + 1;
	if (pathlen > MAX_PATH_LEN) {
		errno = EINVAL;
		return -1;
	}

	struct proto_client_open_device msg = {
		.path_len = (uint16_t)pathlen,
	};
	struct proto_header header = {
		.opcode = CLIENT_OPEN_DEVICE,
		.size = sizeof msg + pathlen,
	};

	if (connection_put(&backend->connection, &header, sizeof header) == -1 ||
	    connection_put(&backend->connection, &msg, sizeof msg) == -1 ||
	    connection_put(&backend->connection, path, pathlen) == -1 || dispatch(backend) == -1) {
		return -1;
	}

	size_t size = read_header(&backend->connection, SERVER_DEVICE_OPENED);
	if (size == SIZE_MAX) {
		check_error(&backend->connection);
		return -1;
	}

	struct proto_server_device_opened rmsg;
	if (sizeof rmsg > size) {
		errno = EBADMSG;
		return -1;
	}
	if (connection_get(&backend->connection, &rmsg, sizeof rmsg) == -1) {
		return -1;
	}

	int received_fd = connection_get_fd(&backend->connection);
	if (received_fd == -1) {
		return -1;
	}

	*fd = received_fd;
	return rmsg.device_id;
}

static int close_device(struct libseat *base, int device_id) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);
	if (device_id < 0) {
		errno = EINVAL;
		return -1;
	}

	struct proto_client_close_device msg = {
		.device_id = device_id,
	};
	struct proto_header header = {
		.opcode = CLIENT_CLOSE_DEVICE,
		.size = sizeof msg,
	};

	if (connection_put(&backend->connection, &header, sizeof header) == -1 ||
	    connection_put(&backend->connection, &msg, sizeof msg) == -1 || dispatch(backend) == -1) {
		return -1;
	}

	size_t size = read_header(&backend->connection, SERVER_DEVICE_CLOSED);
	if (size == SIZE_MAX) {
		check_error(&backend->connection);
		return -1;
	}

	struct proto_server_device_closed rmsg;
	if (sizeof rmsg > size) {
		errno = EBADMSG;
		return -1;
	}
	if (connection_get(&backend->connection, &rmsg, sizeof rmsg) == -1) {
		return -1;
	}
	if (rmsg.device_id != device_id) {
		errno = EBADMSG;
		return -1;
	}

	return 0;
}

static int switch_session(struct libseat *base, int session) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);
	if (session < 0) {
		return -1;
	}

	struct proto_client_switch_session msg = {
		.session = session,
	};
	struct proto_header header = {
		.opcode = CLIENT_SWITCH_SESSION,
		.size = sizeof msg,
	};

	if (connection_put(&backend->connection, &header, sizeof header) == -1 ||
	    connection_put(&backend->connection, &msg, sizeof msg) == -1 ||
	    connection_flush(&backend->connection) == -1) {
		return -1;
	}

	return 0;
}

static int disable_seat(struct libseat *base) {
	struct backend_seatd *backend = backend_seatd_from_libseat_backend(base);
	struct proto_header header = {
		.opcode = CLIENT_DISABLE_SEAT,
		.size = 0,
	};

	if (connection_put(&backend->connection, &header, sizeof header) == -1 ||
	    connection_flush(&backend->connection) == -1) {
		return -1;
	}

	return 0;
}

const struct libseat_impl seatd_impl = {
	.open_seat = open_seat,
	.disable_seat = disable_seat,
	.close_seat = close_seat,
	.seat_name = seat_name,
	.open_device = open_device,
	.close_device = close_device,
	.switch_session = switch_session,
	.get_fd = get_fd,
	.dispatch = dispatch_background,
};

#ifdef BUILTIN_ENABLED
static struct libseat *builtin_open_seat(struct libseat_seat_listener *listener, void *data) {
	int fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == -1) {
		return NULL;
	}

	pid_t pid = fork();
	if (pid == -1) {
		close(fds[0]);
		close(fds[1]);
		return NULL;
	} else if (pid == 0) {
		int fd = fds[0];
		struct server *server = server_create();
		if (server == NULL) {
			close(fd);
			exit(1);
		}
		if (server_add_client(server, fd) == -1) {
			exit(1);
		}
		while (server->running) {
			if (poller_poll(server->poller) == -1) {
				exit(1);
			}
		}
		close(fd);
		exit(0);
	} else {
		int fd = fds[1];
		return _open_seat(listener, data, fd);
	}
}

const struct libseat_impl builtin_impl = {
	.open_seat = builtin_open_seat,
	.disable_seat = disable_seat,
	.close_seat = close_seat,
	.seat_name = seat_name,
	.open_device = open_device,
	.close_device = close_device,
	.switch_session = switch_session,
	.get_fd = get_fd,
	.dispatch = dispatch_background,
};
#endif