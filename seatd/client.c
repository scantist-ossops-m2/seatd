#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <sys/ucred.h>
#include <sys/un.h>
#endif

#include "client.h"
#include "linked_list.h"
#include "log.h"
#include "poller.h"
#include "protocol.h"
#include "seat.h"
#include "server.h"
#include "terminal.h"

static int get_peer(int fd, pid_t *pid, uid_t *uid, gid_t *gid) {
#if defined(__linux__)
	struct ucred cred;
	socklen_t len = sizeof cred;
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == -1) {
		return -1;
	}
	*pid = cred.pid;
	*uid = cred.uid;
	*gid = cred.gid;
	return 0;
#elif defined(__FreeBSD__)
	struct xucred cred;
	socklen_t len = sizeof cred;
	if (getsockopt(fd, SOL_SOCKET, LOCAL_PEERCRED, &cred, &len) == -1) {
		return -1;
	}
	*pid = -1;
	*uid = cred.cr_uid;
	*gid = cred.cr_ngroups > 0 ? cred.cr_groups[0] : -1;
	return 0;
#else
	return -1;
#endif
}

struct client *client_create(struct server *server, int client_fd) {
	uid_t uid;
	gid_t gid;
	pid_t pid;

	if (get_peer(client_fd, &pid, &uid, &gid) == -1) {
		return NULL;
	}

	struct client *client = calloc(1, sizeof(struct client));
	if (client == NULL) {
		return NULL;
	}

	client->uid = uid;
	client->gid = gid;
	client->pid = pid;
	client->server = server;
	client->connection.fd = client_fd;
	linked_list_init(&client->devices);
	return client;
}

void client_kill(struct client *client) {
	assert(client);
	if (client->connection.fd != -1) {
		shutdown(client->connection.fd, SHUT_RDWR);
		close(client->connection.fd);
		client->connection.fd = -1;
	};
	if (client->seat != NULL) {
		seat_remove_client(client);
		client->seat = NULL;
	}
}

void client_destroy(struct client *client) {
	assert(client);
	client->server = NULL;
	if (client->seat != NULL) {
		// This should also close and remove all devices
		seat_remove_client(client);
		client->seat = NULL;
	}
	if (client->event_source != NULL) {
		event_source_fd_destroy(client->event_source);
		client->event_source = NULL;
	}
	if (client->connection.fd != -1) {
		shutdown(client->connection.fd, SHUT_RDWR);
		close(client->connection.fd);
		client->connection.fd = -1;
	}
	connection_close_fds(&client->connection);
	assert(linked_list_empty(&client->devices));
	free(client);
}

static int client_flush(struct client *client) {
	int ret = connection_flush(&client->connection);
	if (ret == -1 && errno == EAGAIN) {
		event_source_fd_update(client->event_source, EVENT_READABLE | EVENT_WRITABLE);
	} else if (ret == -1) {
		return -1;
	}
	return 0;
}

static int client_send_error(struct client *client, int error_code) {
	struct proto_server_error errmsg = {
		.error_code = error_code,
	};
	struct proto_header errheader = {
		.opcode = SERVER_ERROR,
		.size = sizeof errmsg,
	};

	if (connection_put(&client->connection, &errheader, sizeof errheader) == -1 ||
	    connection_put(&client->connection, &errmsg, sizeof errmsg)) {
		log_error("could not send error to client");
		return -1;
	}
	return 0;
}

static char *client_get_seat_name(struct client *client) {
	(void)client;
	// TODO: Look up seat for session.
	return "seat0";
}

static int handle_open_seat(struct client *client) {
	char *seat_name = client_get_seat_name(client);
	if (seat_name == NULL) {
		log_error("could not get name of target seat");
		return -1;
	}

	struct seat *seat = server_get_seat(client->server, seat_name);
	if (seat == NULL) {
		log_error("unable to find seat by name");
		return -1;
	}

	if (seat_add_client(seat, client) == -1) {
		log_errorf("unable to add client to target seat: %s", strerror(errno));
		return -1;
	}

	size_t seat_name_len = strlen(seat_name);

	struct proto_server_seat_opened rmsg = {
		.seat_name_len = (uint16_t)seat_name_len,
	};
	struct proto_header header = {
		.opcode = SERVER_SEAT_OPENED,
		.size = sizeof rmsg + seat_name_len,
	};

	if (connection_put(&client->connection, &header, sizeof header) == -1 ||
	    connection_put(&client->connection, &rmsg, sizeof rmsg) == -1 ||
	    connection_put(&client->connection, seat_name, seat_name_len) == -1) {
		log_errorf("unable to write response: %s", strerror(errno));
		return -1;
	}

	seat_open_client(seat, client);
	return 0;
}

static int handle_close_seat(struct client *client) {
	if (client->seat == NULL) {
		log_error("protocol error: no seat associated with client");
		return -1;
	}

	if (seat_remove_client(client) == -1) {
		log_error("unable to remove client from seat");
		return -1;
	}

	struct proto_header header = {
		.opcode = SERVER_SEAT_CLOSED,
		.size = 0,
	};

	if (connection_put(&client->connection, &header, sizeof header) == -1) {
		log_errorf("unable to write response: %s", strerror(errno));
		return -1;
	}

	return 0;
}

static int handle_open_device(struct client *client, char *path) {
	if (client->seat == NULL) {
		log_error("protocol error: no seat associated with client");
		return -1;
	}

	struct seat_device *device = seat_open_device(client, path);
	if (device == NULL) {
		log_errorf("could not open device: %s", strerror(errno));
		goto fail;
	}

	int dupfd = dup(device->fd);
	if (dupfd == -1) {
		log_errorf("could not dup fd: %s", strerror(errno));
		seat_close_device(client, device);
		goto fail;
	}

	if (connection_put_fd(&client->connection, dupfd) == -1) {
		log_errorf("unable to queue fd for sending: %s", strerror(errno));
		return -1;
	}

	struct proto_server_device_opened msg = {
		.device_id = device->device_id,
	};
	struct proto_header header = {
		.opcode = SERVER_DEVICE_OPENED,
		.size = sizeof msg,
	};

	if (connection_put(&client->connection, &header, sizeof header) == -1 ||
	    connection_put(&client->connection, &msg, sizeof msg)) {
		log_errorf("unable to write response: %s", strerror(errno));
		return -1;
	}

	return 0;

fail:
	return client_send_error(client, errno);
}

static int handle_close_device(struct client *client, int device_id) {
	if (client->seat == NULL) {
		log_error("protocol error: no seat associated with client");
		return -1;
	}

	struct seat_device *device = seat_find_device(client, device_id);
	if (device == NULL) {
		log_error("no such device");
		errno = EBADF;
		goto fail;
	}

	if (seat_close_device(client, device) == -1) {
		log_errorf("could not close device: %s", strerror(errno));
		goto fail;
	}

	struct proto_server_device_closed msg = {
		.device_id = device_id,
	};
	struct proto_header header = {
		.opcode = SERVER_DEVICE_CLOSED,
		.size = sizeof msg,
	};

	if (connection_put(&client->connection, &header, sizeof header) == -1 ||
	    connection_put(&client->connection, &msg, sizeof msg)) {
		log_errorf("unable to write response: %s", strerror(errno));
		return -1;
	}

	return 0;

fail:
	return client_send_error(client, errno);
}

static int handle_switch_session(struct client *client, int session) {
	if (client->seat == NULL) {
		log_error("protocol error: no seat associated with client");
		return -1;
	}

	if (seat_set_next_session(client, session) == -1) {
		goto error;
	}

	return 0;

error:
	return client_send_error(client, errno);
}

static int handle_disable_seat(struct client *client) {
	if (client->seat == NULL) {
		log_error("protocol error: no seat associated with client");
		return -1;
	}

	struct seat *seat = client->seat;
	if (seat->active_client != client) {
		log_info("client is not currently active");
		errno = EPERM;
		goto error;
	}

	if (seat_ack_disable_client(client) == -1) {
		goto error;
	}

	return 0;

error:
	return client_send_error(client, errno);
}

static int client_handle_opcode(struct client *client, uint16_t opcode, size_t size) {
	int res = 0;
	switch (opcode) {
	case CLIENT_OPEN_SEAT: {
		if (size != 0) {
			log_error("protocol error: invalid open_seat message");
			return -1;
		}
		res = handle_open_seat(client);
		break;
	}
	case CLIENT_CLOSE_SEAT: {
		if (size != 0) {
			log_error("protocol error: invalid close_seat message");
			return -1;
		}
		res = handle_close_seat(client);
		break;
	}
	case CLIENT_OPEN_DEVICE: {
		char path[MAX_PATH_LEN];
		struct proto_client_open_device msg;
		if (sizeof msg > size || connection_get(&client->connection, &msg, sizeof msg) == -1 ||
		    sizeof msg + msg.path_len > size || msg.path_len > MAX_PATH_LEN) {
			log_error("protocol error: invalid open_device message");
			return -1;
		}
		if (connection_get(&client->connection, path, msg.path_len) == -1) {
			log_error("protocol error: invalid open_device message");
			return -1;
		}

		res = handle_open_device(client, path);
		break;
	}
	case CLIENT_CLOSE_DEVICE: {
		struct proto_client_close_device msg;
		if (sizeof msg > size || connection_get(&client->connection, &msg, sizeof msg) == -1) {
			log_error("protocol error: invalid close_device message");
			return -1;
		}

		res = handle_close_device(client, msg.device_id);
		break;
	}
	case CLIENT_SWITCH_SESSION: {
		struct proto_client_switch_session msg;
		if (sizeof msg > size || connection_get(&client->connection, &msg, sizeof msg) == -1) {
			log_error("protocol error: invalid switch_session message");
			return -1;
		}

		res = handle_switch_session(client, msg.session);
		break;
	}
	case CLIENT_DISABLE_SEAT: {
		if (size != 0) {
			log_error("protocol error: invalid disable_seat message");
			return -1;
		}
		res = handle_disable_seat(client);
		break;
	}
	default:
		log_errorf("protocol error: unknown opcode: %d", opcode);
		res = -1;
		break;
	}
	if (res != -1) {
		res = client_flush(client);
	}
	return res;
}

int client_send_disable_seat(struct client *client) {
	struct proto_header header = {
		.opcode = SERVER_DISABLE_SEAT,
		.size = 0,
	};
	if (connection_put(&client->connection, &header, sizeof header) == -1 ||
	    connection_flush(&client->connection) == -1) {
		log_error("unable to send event");
		return -1;
	}
	return 0;
}

int client_send_enable_seat(struct client *client) {
	struct proto_header header = {
		.opcode = SERVER_ENABLE_SEAT,
		.size = 0,
	};
	if (connection_put(&client->connection, &header, sizeof header) == -1 ||
	    connection_flush(&client->connection) == -1) {
		log_error("unable to send event");
		return -1;
	}
	return 0;
}

int client_handle_connection(int fd, uint32_t mask, void *data) {
	(void)fd;

	struct client *client = data;
	if (mask & EVENT_ERROR) {
		log_error("connection error");
		goto fail;
	}
	if (mask & EVENT_HANGUP) {
		log_info("client disconnected");
		goto fail;
	}

	if (mask & EVENT_WRITABLE) {
		int len = connection_flush(&client->connection);
		if (len == -1 && errno != EAGAIN) {
			log_error("could not flush client connection");
			goto fail;
		} else if (len >= 0) {
			event_source_fd_update(client->event_source, EVENT_READABLE);
		}
	}

	if (mask & EVENT_READABLE) {
		int len = connection_read(&client->connection);
		if (len == 0 || (len == -1 && errno != EAGAIN)) {
			log_error("could not read client connection");
			goto fail;
		}

		struct proto_header header;
		while (connection_get(&client->connection, &header, sizeof header) != -1) {
			if (connection_pending(&client->connection) < header.size) {
				connection_restore(&client->connection, sizeof header);
				break;
			}
			if (client_handle_opcode(client, header.opcode, header.size) == -1) {
				goto fail;
			}
		}
	}

	return 0;

fail:
	client_destroy(client);
	return -1;
}

int client_get_session(const struct client *client) {
	if (client->seat == NULL || client->seat->active_client != client) {
		return -1;
	}
	if (client->seat->vt_bound) {
		return client->seat->active_client->seat_vt;
	}
	// TODO: Store some session sequence
	abort();
	return -1;
}
