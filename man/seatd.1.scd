seatd(1)

# NAME

seatd - A seat management daemon

# SYNOPSIS

*seatd* [options]

# OPTIONS

*-h*
	Show help message and quit.

*-u <user>*
	User to own the seatd socket.

*-g <group>*
	Group to own the seatd socket.

*-s <path>*
	Where to create the seatd socket. Defaults to `/run/seatd.sock`.

*-v*
	Show the version number and quit.

# DESCRIPTION

seatd provides central seat management, mediating access to shared resources
such as displays and input devices in a multi-session, multi-seat environment.

seatd operates over a UNIX domain socket, with *libseat* providing the
client-side of the protocol.

# ENVIRONMENT

[[ *VARIABLE*
:[ *VALUES*
:< *DESCRIPTION*
|  SEATD_SOCK
:  File path
:  Informs libseat of the socket location, needed if it differs from `/run/seatd.sock`
|  LIBSEAT_LOGLEVEL
:  silent, error, info, debug
:  Sets the libseat log level. Defaults to "silent"
|  SEATD_LOGLEVEL
:  silent, error, info, debug
:  Sets the seatd log level. Defaults to "error"

# SEE ALSO

The libseat library, *<libseat.h>*

# AUTHORS

Maintained by Kenny Levinsen <contact@kl.wtf>. For more information about seatd
development, see https://sr.ht/~kennylevinsen/seatd.