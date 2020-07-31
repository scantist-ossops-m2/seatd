#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libseat.h"

static void handle_enable(struct libseat *backend, void *data) {
	(void)backend;
	int *active = (int *)data;
	(*active)++;
}

static void handle_disable(struct libseat *backend, void *data) {
	(void)backend;
	int *active = (int *)data;
	(*active)--;

	libseat_disable_seat(backend);
}

int main(int argc, char *argv[]) {
	(void)argc;
	(void)argv;

	int active = 0;
	struct libseat_seat_listener listener = {
		.enable_seat = handle_enable,
		.disable_seat = handle_disable,
	};
	struct libseat *backend = libseat_open_seat(&listener, &active);
	fprintf(stderr, "libseat_open_seat(listener: %p, userdata: %p) = %p\n", (void *)&listener,
		(void *)&active, (void *)backend);
	if (backend == NULL) {
		fprintf(stderr, "libseat_open_seat() failed: %s\n", strerror(errno));
		return -1;
	}

	while (active == 0) {
		fprintf(stderr, "waiting for activation...\n");
		libseat_dispatch(backend, -1);
	}
	fprintf(stderr, "active!\n");

	int fd, device;
	device = libseat_open_device(backend, "/dev/dri/card0", &fd);
	fprintf(stderr, "libseat_open_device(backend: %p, path: %s, fd: %p) = %d\n",
		(void *)backend, "/dev/dri/card0", (void *)&fd, device);
	if (device == -1) {
		fprintf(stderr, "libseat_open_device() failed: %s\n", strerror(errno));
		libseat_close_seat(backend);
		return 1;
	}

	close(fd);
	libseat_close_device(backend, device);
	libseat_close_seat(backend);
	return 0;
}