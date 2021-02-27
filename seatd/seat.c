#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "client.h"
#include "drm.h"
#include "evdev.h"
#include "linked_list.h"
#include "log.h"
#include "protocol.h"
#include "seat.h"
#include "terminal.h"

static int seat_close_client(struct client *client);
static void vt_close(struct seat *seat);


struct seat *seat_create(const char *seat_name, bool vt_bound) {
	struct seat *seat = calloc(1, sizeof(struct seat));
	if (seat == NULL) {
		return NULL;
	}
	linked_list_init(&seat->clients);
	seat->vt_bound = vt_bound;
	seat->seat_name = strdup(seat_name);
	seat->cur_vt = 0;
	seat->cur_ttyfd = -1;
	if (seat->seat_name == NULL) {
		free(seat);
		return NULL;
	}
	if (vt_bound) {
		log_infof("created VT-bound seat '%s'", seat_name);
	} else {
		log_infof("created seat '%s'", seat_name);
	}
	return seat;
}

void seat_destroy(struct seat *seat) {
	assert(seat);
	while (!linked_list_empty(&seat->clients)) {
		struct client *client = (struct client *)seat->clients.next;
		assert(client->seat == seat);
		client_destroy(client);
	}
	vt_close(seat);
	linked_list_remove(&seat->link);
	free(seat->seat_name);
	free(seat);
}

static void seat_update_vt(struct seat *seat) {
	int tty0fd = terminal_open(0);
	if (tty0fd == -1) {
		log_errorf("unable to open tty0: %s", strerror(errno));
		return;
	}
	seat->cur_vt = terminal_current_vt(tty0fd);
	close(tty0fd);
}

static int vt_open(struct seat *seat, int vt) {
	assert(vt != -1);
	if (seat->cur_ttyfd != -1) {
		terminal_set_process_switching(seat->cur_ttyfd, true);
		close(seat->cur_ttyfd);
	}
	seat->cur_ttyfd = terminal_open(vt);
	if (seat->cur_ttyfd == -1) {
		log_errorf("could not open terminal for vt %d: %s", vt, strerror(errno));
		return -1;
	}

	terminal_set_process_switching(seat->cur_ttyfd, true);
	terminal_set_keyboard(seat->cur_ttyfd, false);
	terminal_set_graphics(seat->cur_ttyfd, true);
	return 0;
}

static void vt_close_fd(int fd) {
	terminal_set_process_switching(fd, true);
	terminal_set_keyboard(fd, true);
	terminal_set_graphics(fd, false);
}

static void vt_close(struct seat *seat) {
	if (seat->cur_ttyfd == -1) {
		return;
	}

	vt_close_fd(seat->cur_ttyfd);
	close(seat->cur_ttyfd);
	seat->cur_ttyfd = -1;
}

static int vt_close_num(int vt) {
	int ttyfd = terminal_open(vt);
	if (ttyfd == -1) {
		log_errorf("could not open terminal %s", strerror(errno));
		return -1;
	}
	vt_close_fd(ttyfd);
	close(ttyfd);
	return 0;
}

static int vt_switch(struct seat *seat, int vt) {
	int ttyfd = terminal_open(seat->cur_vt);
	if (ttyfd == -1) {
		log_errorf("could not open terminal: %s", strerror(errno));
		return -1;
	}
	terminal_set_process_switching(ttyfd, true);
	terminal_switch_vt(ttyfd, vt);
	close(ttyfd);
	return 0;
}

static int vt_ack(struct seat *seat, bool release) {
	int tty0fd = terminal_open(seat->cur_vt);
	if (tty0fd == -1) {
		log_errorf("unable to open tty0: %s", strerror(errno));
		return -1;
	}
	if (release) {
		terminal_ack_release(tty0fd);
	} else {
		terminal_ack_acquire(tty0fd);
	}
	close(tty0fd);
	return 0;
}

int seat_add_client(struct seat *seat, struct client *client) {
	assert(seat);
	assert(client);

	if (client->seat != NULL) {
		log_error("cannot add client: client is already a member of a seat");
		errno = EBUSY;
		return -1;
	}

	if (seat->vt_bound && seat->active_client != NULL) {
		log_error("cannot add client: seat is VT-bound and an active client already exists");
		errno = EBUSY;
		return -1;
	}

	if (client->session != -1) {
		log_error("cannot add client: client cannot be reused");
		errno = EINVAL;
		return -1;
	}

	if (seat->vt_bound) {
		seat_update_vt(seat);
		if (seat->cur_vt == -1) {
			log_error("could not determine VT for client");
			errno = EINVAL;
			return -1;
		}
		client->session = seat->cur_vt;
	} else {
		client->session = seat->session_cnt++;
	}

	client->seat = seat;
	log_infof("added client %d", client->session);

	return 0;
}

int seat_remove_client(struct client *client) {
	assert(client);
	assert(client->seat);

	struct seat *seat = client->seat;
	if (seat->next_client == client) {
		seat->next_client = NULL;
	}

	while (!linked_list_empty(&client->devices)) {
		struct seat_device *device = (struct seat_device *)client->devices.next;
		seat_close_device(client, device);
	}

	seat_close_client(client);

	client->seat = NULL;
	log_infof("removed client %d", client->session);

	return 0;
}

struct seat_device *seat_find_device(struct client *client, int device_id) {
	assert(client);
	assert(client->seat);
	assert(device_id != 0);

	for (struct linked_list *elem = client->devices.next; elem != &client->devices;
	     elem = elem->next) {
		struct seat_device *seat_device = (struct seat_device *)elem;
		if (seat_device->device_id == device_id) {
			return seat_device;
		}
	}
	errno = ENOENT;
	return NULL;
}

struct seat_device *seat_open_device(struct client *client, const char *path) {
	assert(client);
	assert(client->seat);
	assert(strlen(path) > 0);
	struct seat *seat = client->seat;

	if (client->state != CLIENT_ACTIVE) {
		log_error("client is not active");
		errno = EPERM;
		return NULL;
	}
	assert(seat->active_client == client);

	char sanitized_path[PATH_MAX];
	if (realpath(path, sanitized_path) == NULL) {
		log_errorf("invalid path '%s': %s", path, strerror(errno));
		return NULL;
	}

	enum seat_device_type type;
	if (path_is_evdev(sanitized_path)) {
		type = SEAT_DEVICE_TYPE_EVDEV;
	} else if (path_is_drm(sanitized_path)) {
		type = SEAT_DEVICE_TYPE_DRM;
	} else {
		log_errorf("invalid path '%s'", sanitized_path);
		errno = ENOENT;
		return NULL;
	}

	int device_id = 1;
	size_t device_count = 0;
	struct seat_device *device = NULL;
	for (struct linked_list *elem = client->devices.next; elem != &client->devices;
	     elem = elem->next) {
		struct seat_device *old_device = (struct seat_device *)elem;

		if (strcmp(old_device->path, sanitized_path) == 0) {
			old_device->ref_cnt++;
			device = old_device;
			goto done;
		}

		if (old_device->device_id >= device_id) {
			device_id = old_device->device_id + 1;
		}
		device_count++;
	}

	if (device_count >= MAX_SEAT_DEVICES) {
		log_error("max seat devices exceeded");
		errno = EMFILE;
		return NULL;
	}

	int fd = open(sanitized_path, O_RDWR | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
	if (fd == -1) {
		log_errorf("could not open file: %s", strerror(errno));
		return NULL;
	}

	switch (type) {
	case SEAT_DEVICE_TYPE_DRM:
		if (drm_set_master(fd) == -1) {
			log_errorf("could not make device fd drm master: %s", strerror(errno));
		}
		break;
	case SEAT_DEVICE_TYPE_EVDEV:
		// Nothing to do here
		break;
	default:
		log_error("invalid seat device type");
		abort();
	}

	device = calloc(1, sizeof(struct seat_device));
	if (device == NULL) {
		log_errorf("could not alloc device for '%s': %s", sanitized_path, strerror(errno));
		close(fd);
		errno = ENOMEM;
		return NULL;
	}

	device->path = strdup(sanitized_path);
	if (device->path == NULL) {
		log_errorf("could not dup path for '%s': %s", sanitized_path, strerror(errno));
		close(fd);
		free(device);
		return NULL;
	}

	device->ref_cnt = 1;
	device->type = type;
	device->fd = fd;
	device->device_id = device_id;
	device->active = true;
	linked_list_insert(&client->devices, &device->link);

done:
	log_debugf("seat: '%s', client: %d, path: '%s', device_id: %d, ref_cnt: %d", seat->seat_name,
		   client->session, path, device_id, device->ref_cnt);

	return device;
}

static int seat_deactivate_device(struct seat_device *seat_device) {
	assert(seat_device && seat_device->fd > 0);

	if (!seat_device->active) {
		return 0;
	}
	switch (seat_device->type) {
	case SEAT_DEVICE_TYPE_DRM:
		if (drm_drop_master(seat_device->fd) == -1) {
			log_errorf("could not revoke drm master on device fd: %s", strerror(errno));
			return -1;
		}
		break;
	case SEAT_DEVICE_TYPE_EVDEV:
		if (evdev_revoke(seat_device->fd) == -1) {
			log_errorf("could not revoke evdev on device fd: %s", strerror(errno));
			return -1;
		}
		break;
	default:
		log_error("invalid seat device type");
		abort();
	}
	seat_device->active = false;
	return 0;
}

int seat_close_device(struct client *client, struct seat_device *seat_device) {
	assert(client);
	assert(client->seat);
	assert(seat_device && seat_device->fd != -1);

	log_debugf("seat: '%s', client: %d, path: '%s', device_id: %d, ref_cnt: %d",
		   client->seat->seat_name, client->session, seat_device->path, seat_device->device_id,
		   seat_device->ref_cnt);

	seat_device->ref_cnt--;
	if (seat_device->ref_cnt > 0) {
		return 0;
	}

	linked_list_remove(&seat_device->link);
	if (seat_device->fd != -1) {
		seat_deactivate_device(seat_device);
		close(seat_device->fd);
	}
	free(seat_device->path);
	free(seat_device);
	return 0;
}

static int seat_activate_device(struct client *client, struct seat_device *seat_device) {
	assert(client);
	assert(client->seat);
	assert(seat_device && seat_device->fd > 0);

	if (seat_device->active) {
		return 0;
	}
	switch (seat_device->type) {
	case SEAT_DEVICE_TYPE_DRM:
		if (drm_set_master(seat_device->fd) == -1) {
			log_errorf("could not make device fd drm master: %s", strerror(errno));
		}
		seat_device->active = true;
		break;
	case SEAT_DEVICE_TYPE_EVDEV:
		errno = EINVAL;
		return -1;
	default:
		log_error("invalid seat device type");
		abort();
	}

	return 0;
}

static int seat_activate(struct seat *seat) {
	assert(seat);

	if (seat->active_client != NULL) {
		return 0;
	}

	struct client *next_client = NULL;
	if (seat->next_client != NULL) {
		log_debug("activating next queued client");
		next_client = seat->next_client;
		seat->next_client = NULL;
	} else if (linked_list_empty(&seat->clients)) {
		log_info("no clients on seat to activate");
		return -1;
	} else if (seat->vt_bound && seat->cur_vt == -1) {
		return -1;
	} else if (seat->vt_bound) {
		for (struct linked_list *elem = seat->clients.next; elem != &seat->clients;
		     elem = elem->next) {
			struct client *client = (struct client *)elem;
			if (client->session == seat->cur_vt) {
				log_debugf("activating client belonging to VT %d", seat->cur_vt);
				next_client = client;
				goto done;
			}
		}

		log_infof("no clients belonging to VT %d to activate", seat->cur_vt);
		return -1;
	} else {
		log_debug("activating first client on seat");
		next_client = (struct client *)seat->clients.next;
	}

done:
	return seat_open_client(seat, next_client);
}

int seat_open_client(struct seat *seat, struct client *client) {
	assert(seat);
	assert(client);

	if (client->state != CLIENT_NEW && client->state != CLIENT_DISABLED) {
		log_errorf("could not enable client %d: client is not new or disabled",
				client->session);
		errno = EALREADY;
		return -1;
	}

	if (seat->active_client != NULL) {
		log_errorf("could not enable client %d: seat already has active client",
				client->session);
		errno = EBUSY;
		return -1;
	}

	if (seat->vt_bound && vt_open(seat, client->session) == -1) {
		log_errorf("could not open VT for client %d", client->session);
		goto error;
	}

	for (struct linked_list *elem = client->devices.next; elem != &client->devices;
	     elem = elem->next) {
		struct seat_device *device = (struct seat_device *)elem;
		if (seat_activate_device(client, device) == -1) {
			log_errorf("unable to activate '%s' for client %d: %s",
					device->path, client->session, strerror(errno));
		}
	}

	client->state = CLIENT_ACTIVE;
	seat->active_client = client;
	if (client_send_enable_seat(client) == -1) {
		log_errorf("could not send enable signal to client %d", client->session);
		goto error;
	}

	log_infof("enabled client %d", client->session);
	return 0;

error:
	if (seat->vt_bound) {
		vt_close(seat);
	}
	return -1;
}

static int seat_close_client(struct client *client) {
	assert(client);
	assert(client->seat);

	struct seat *seat = client->seat;

	while (!linked_list_empty(&client->devices)) {
		struct seat_device *device = (struct seat_device *)client->devices.next;
		if (seat_close_device(client, device) == -1) {
			log_errorf("unable to close '%s': %s", device->path, strerror(errno));
		}
	}

	bool was_current = seat->active_client == client;
	if (was_current) {
		seat->active_client = NULL;
		seat_activate(seat);
	}

	if (seat->vt_bound) {
		if (was_current && seat->active_client == NULL) {
			// This client was current, but there were no clients
			// waiting to take this VT, so clean it up.
			log_debug("closing active VT");
			vt_close(seat);
		} else if (!was_current && client->state != CLIENT_CLOSED) {
			// This client was not current, but as the client was
			// running, we need to clean up the VT.
			log_debug("closing inactive VT");
			vt_close_num(client->session);
		}
	}

	client->state = CLIENT_CLOSED;
	log_infof("closed client %d", client->session);

	return 0;
}

static int seat_disable_client(struct client *client) {
	assert(client);
	assert(client->seat);

	struct seat *seat = client->seat;

	if (client->state != CLIENT_ACTIVE) {
		log_error("client not active");
		errno = EBUSY;
		return -1;
	}
	assert(seat->active_client = client);

	// We *deactivate* all remaining fds. These may later be reactivated.
	// The reason we cannot just close them is that certain device fds, such
	// as for DRM, must maintain the exact same file description for their
	// contexts to remain valid.
	for (struct linked_list *elem = client->devices.next; elem != &client->devices;
	     elem = elem->next) {
		struct seat_device *device = (struct seat_device *)elem;
		if (seat_deactivate_device(device) == -1) {
			log_errorf("unable to deactivate '%s': %s", device->path, strerror(errno));
		}
	}

	client->state = CLIENT_PENDING_DISABLE;
	if (client_send_disable_seat(seat->active_client) == -1) {
		log_error("could not send disable event");
		return -1;
	}

	log_infof("disabling client %d", client->session);
	return 0;
}

int seat_ack_disable_client(struct client *client) {
	assert(client);
	assert(client->seat);

	struct seat *seat = client->seat;
	if (client->state != CLIENT_PENDING_DISABLE) {
		log_error("client not pending disable");
		errno = EBUSY;
		return -1;
	}

	client->state = CLIENT_DISABLED;
	log_infof("disabled client %d", client->session);

	if (seat->active_client != client) {
		return 0;
	}

	seat->active_client = NULL;
	seat_activate(seat);

	// If we're VT-bound, we've either de-activated a client on a foreign
	// VT, in which case we need to do nothing, or disabled the current VT,
	// in which case seat_activate would just immediately re-enable it.
	return 0;
}

int seat_set_next_session(struct client *client, int session) {
	assert(client);
	assert(client->seat);

	struct seat *seat = client->seat;

	if (client->state != CLIENT_ACTIVE) {
		log_error("client is not active");
		errno = EPERM;
		return -1;
	}
	assert(seat->active_client == client);

	if (session <= 0) {
		log_errorf("invalid session value: %d", session);
		errno = EINVAL;
		return -1;
	}

	if (session == client->session) {
		log_info("requested session is already active");
		return 0;
	}

	if (seat->next_client != NULL) {
		log_info("switch is already queued");
		return 0;
	}

	if (seat->vt_bound) {
		log_infof("switching to VT %d from VT %d", session, seat->cur_vt);
		if (vt_switch(seat, session) == -1) {
			log_error("could not switch VT");
			return -1;
		}
		return 0;
	}

	struct client *target = NULL;
	for (struct linked_list *elem = seat->clients.next; elem != &seat->clients;
	     elem = elem->next) {
		struct client *c = (struct client *)elem;
		if (c->session == session) {
			target = c;
			break;
		}
	}

	if (target == NULL) {
		log_error("no valid switch available");
		errno = EINVAL;
		return -1;
	}

	log_infof("queuing switch client with session %d", session);
	seat->next_client = target;
	seat_disable_client(seat->active_client);
	return 0;
}

int seat_vt_activate(struct seat *seat) {
	assert(seat);
	if (!seat->vt_bound) {
		log_debug("VT activation on non VT-bound seat, ignoring");
		return -1;
	}
	seat_update_vt(seat);
	log_debug("activating VT");
	vt_ack(seat, false);
	if (seat->active_client == NULL) {
		seat_activate(seat);
	}
	return 0;
}

int seat_vt_release(struct seat *seat) {
	assert(seat);
	if (!seat->vt_bound) {
		log_debug("VT release request on non VT-bound seat, ignoring");
		return -1;
	}
	seat_update_vt(seat);

	log_debug("releasing VT");
	if (seat->active_client != NULL) {
		seat_disable_client(seat->active_client);
	}

	vt_ack(seat, true);
	seat->cur_vt = -1;
	return 0;
}
