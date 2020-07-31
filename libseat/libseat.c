#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend.h"
#include "compiler.h"
#include "libseat.h"
#include "log.h"

extern const struct libseat_impl seatd_impl;
extern const struct libseat_impl logind_impl;
extern const struct libseat_impl builtin_impl;

static const struct named_backend impls[] = {
#ifdef SEATD_ENABLED
	{"seatd", &seatd_impl},
#endif
#ifdef LOGIND_ENABLED
	{"logind", &logind_impl},
#endif
#ifdef BUILTIN_ENABLED
	{"builtin", &builtin_impl},
#endif
	{NULL, NULL},
};

#if !defined(SEATD_ENABLED) && !defined(LOGIND_ENABLED) && !defined(BUILTIN_ENABLED)
#error At least one backend must be enabled
#endif

LIBSEAT_EXPORT struct libseat *libseat_open_seat(struct libseat_seat_listener *listener, void *data) {
	if (listener == NULL) {
		errno = EINVAL;
		return NULL;
	}

	char *loglevel = getenv("SEATD_LOGLEVEL");
	enum libseat_log_level level = LIBSEAT_SILENT;
	if (loglevel != NULL) {
		if (strcmp(loglevel, "silent") == 0) {
			level = LIBSEAT_SILENT;
		} else if (strcmp(loglevel, "info") == 0) {
			level = LIBSEAT_INFO;
		} else if (strcmp(loglevel, "debug") == 0) {
			level = LIBSEAT_DEBUG;
		}
	}
	libseat_log_init(level);

	char *backend_type = getenv("LIBSEAT_BACKEND");
	struct libseat *backend = NULL;
	for (const struct named_backend *iter = impls; iter->backend != NULL; iter++) {
		log_debugf("libseat_open_seat: trying backend '%s'", iter->name);
		if (backend_type != NULL && strcmp(backend_type, iter->name) != 0) {
			continue;
		}
		backend = iter->backend->open_seat(listener, data);
		if (backend != NULL) {
			log_infof("libseat_open_seat: seat opened with backend '%s'", iter->name);
			break;
		}
	}
	if (backend == NULL) {
		errno = ENOSYS;
	}
	return backend;
}

LIBSEAT_EXPORT int libseat_disable_seat(struct libseat *seat) {
	assert(seat && seat->impl);
	return seat->impl->disable_seat(seat);
}

LIBSEAT_EXPORT int libseat_close_seat(struct libseat *seat) {
	assert(seat && seat->impl);
	return seat->impl->close_seat(seat);
}

LIBSEAT_EXPORT const char *libseat_seat_name(struct libseat *seat) {
	assert(seat && seat->impl);
	return seat->impl->seat_name(seat);
}

LIBSEAT_EXPORT int libseat_open_device(struct libseat *seat, const char *path, int *fd) {
	assert(seat && seat->impl);
	return seat->impl->open_device(seat, path, fd);
}

LIBSEAT_EXPORT int libseat_close_device(struct libseat *seat, int device_id) {
	assert(seat && seat->impl);
	return seat->impl->close_device(seat, device_id);
}

LIBSEAT_EXPORT int libseat_get_fd(struct libseat *seat) {
	assert(seat && seat->impl);
	return seat->impl->get_fd(seat);
}

LIBSEAT_EXPORT int libseat_dispatch(struct libseat *seat, int timeout) {
	assert(seat && seat->impl);
	return seat->impl->dispatch(seat, timeout);
}

LIBSEAT_EXPORT int libseat_switch_session(struct libseat *seat, int session) {
	assert(seat && seat->impl);
	return seat->impl->switch_session(seat, session);
}