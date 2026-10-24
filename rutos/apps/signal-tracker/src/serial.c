#include "serial.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <libubox/utils.h>

#define LOG_WARN_LEVEL 4
#define LOG_INFO_LEVEL 6

static void slog(struct serial_port *sp, int level, const char *msg)
{
	if (sp->log)
		sp->log(level, msg);
}

static bool read_word(const char *path, char *out, size_t len)
{
	FILE *f = fopen(path, "r");
	bool ok;

	if (!f)
		return false;
	ok = fgets(out, (int)len, f) != NULL;
	fclose(f);
	if (!ok)
		return false;
	out[strcspn(out, "\r\n ")] = '\0';
	return out[0] != '\0';
}

/* Resolves /sys/class/tty/<name>/device and walks up to the USB device with idVendor. */
static bool usb_id_for_tty(const char *name, char *id, size_t len)
{
	char link[96], dir[PATH_MAX], file[PATH_MAX + 16], vid[16], pid[16];

	snprintf(link, sizeof(link), "/sys/class/tty/%s/device", name);
	if (!realpath(link, dir))
		return false;

	for (int depth = 0; depth < 6; depth++) {
		char *slash;

		snprintf(file, sizeof(file), "%s/idVendor", dir);
		if (read_word(file, vid, sizeof(vid))) {
			snprintf(file, sizeof(file), "%s/idProduct", dir);
			if (!read_word(file, pid, sizeof(pid)))
				return false;
			snprintf(id, len, "%s:%s", vid, pid);
			return true;
		}
		slash = strrchr(dir, '/');
		if (!slash || slash == dir)
			break;
		*slash = '\0';
	}
	return false;
}

static int find_device(const struct st_config *cfg, char *path, size_t len, char *err,
		       size_t errlen)
{
	DIR *d;
	struct dirent *e;
	int matches = 0;

	if (strcmp(cfg->device, "auto") != 0) {
		snprintf(path, len, "%s", cfg->device);
		return 0;
	}

	d = opendir("/sys/class/tty");
	if (!d) {
		snprintf(err, errlen, "Cannot list /sys/class/tty: %s", strerror(errno));
		return -1;
	}
	while ((e = readdir(d))) {
		char id[24];

		if (strncmp(e->d_name, "ttyUSB", 6) != 0 && strncmp(e->d_name, "ttyACM", 6) != 0)
			continue;
		if (!usb_id_for_tty(e->d_name, id, sizeof(id)))
			continue;
		for (int i = 0; i < cfg->n_usb_ids; i++) {
			if (strcasecmp(id, cfg->usb_ids[i]) == 0) {
				if (matches++ == 0)
					snprintf(path, len, "/dev/%s", e->d_name);
				break;
			}
		}
	}
	closedir(d);

	if (matches == 0) {
		snprintf(err, errlen, "No ESP32 USB serial device found (check cable and usb_id list)");
		return -1;
	}
	if (matches > 1) {
		snprintf(err, errlen,
			 "%d matching USB serial devices found; set option 'device' explicitly",
			 matches);
		return -1;
	}
	return 0;
}

static int open_port(const char *path, char *err, size_t len)
{
	struct termios tio;
	int bits = TIOCM_DTR | TIOCM_RTS;
	int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

	if (fd < 0) {
		snprintf(err, len, "Cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	/* Keep other cooperating processes (and a second daemon instance) off the port. */
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		snprintf(err, len, "%s is in use by another process", path);
		close(fd);
		return -1;
	}
	ioctl(fd, TIOCEXCL);

	if (tcgetattr(fd, &tio) != 0) {
		snprintf(err, len, "%s is not a serial port: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~(HUPCL | CRTSCTS | CSTOPB);
	tio.c_cc[VMIN] = 0;
	tio.c_cc[VTIME] = 0;
	cfsetispeed(&tio, SERIAL_BAUD);
	cfsetospeed(&tio, SERIAL_BAUD);
	if (tcsetattr(fd, TCSANOW, &tio) != 0) {
		snprintf(err, len, "Cannot configure %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	/* ESP32 dev boards wire DTR/RTS to EN/GPIO0; release both so the chip runs normally. */
	ioctl(fd, TIOCMBIC, &bits);
	tcflush(fd, TCIOFLUSH);
	return fd;
}

static void close_port(struct serial_port *sp, const char *reason, bool notify_link)
{
	if (sp->open) {
		uloop_fd_delete(&sp->ufd);
		close(sp->ufd.fd);
		sp->open = false;
	}
	if (reason)
		snprintf(sp->error, sizeof(sp->error), "%s", reason);
	if (notify_link)
		esp_link_disconnected(sp->link, reason);
	uloop_timeout_set(&sp->retry, SERIAL_RETRY_MS);
}

static void on_fd(struct uloop_fd *u, unsigned int events)
{
	struct serial_port *sp = container_of(u, struct serial_port, ufd);
	char buf[256];

	(void)events;
	for (;;) {
		ssize_t n = read(u->fd, buf, sizeof(buf));

		if (n > 0) {
			esp_link_feed(sp->link, buf, (size_t)n);
			if (!sp->open)
				return; /* the link reset the port while handling the data */
			continue;
		}
		if (n == 0) {
			close_port(sp, "Serial device disconnected", true);
			return;
		}
		if (errno == EINTR)
			continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			break;
		close_port(sp, "Serial device read error", true);
		return;
	}
	if (u->eof || u->error)
		close_port(sp, "Serial device disconnected", true);
}

static void try_open(struct uloop_timeout *t)
{
	struct serial_port *sp = container_of(t, struct serial_port, retry);
	char path[64], err[160], msg[200];
	int fd;

	if (sp->open)
		return;
	if (find_device(sp->cfg, path, sizeof(path), err, sizeof(err)) != 0 ||
	    (fd = open_port(path, err, sizeof(err))) < 0) {
		if (strcmp(err, sp->error) != 0) {
			slog(sp, LOG_WARN_LEVEL, err); /* log changes only, not every retry */
			snprintf(sp->error, sizeof(sp->error), "%s", err);
		}
		uloop_timeout_set(&sp->retry, SERIAL_RETRY_MS);
		return;
	}

	sp->ufd.fd = fd;
	sp->ufd.cb = on_fd;
	uloop_fd_add(&sp->ufd, ULOOP_READ);
	sp->open = true;
	sp->error[0] = '\0';
	snprintf(sp->path, sizeof(sp->path), "%s", path);
	snprintf(msg, sizeof(msg), "opened %s", path);
	slog(sp, LOG_INFO_LEVEL, msg);
	esp_link_connected(sp->link);
}

static int serial_write(void *io, const char *buf, size_t len)
{
	struct serial_port *sp = io;
	size_t off = 0;
	int waits = 0;

	if (!sp->open)
		return -1;
	while (off < len) {
		ssize_t n = write(sp->ufd.fd, buf + off, len - off);

		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits++ < 5) {
			struct pollfd p = { .fd = sp->ufd.fd, .events = POLLOUT };

			poll(&p, 1, 20);
			continue;
		}
		return -1;
	}
	return 0;
}

static void serial_reset(void *io)
{
	close_port(io, NULL, false);
}

const struct esp_link_ops serial_ops = {
	.write = serial_write,
	.reset = serial_reset,
};

void serial_init(struct serial_port *sp, struct esp_link *link, const struct st_config *cfg)
{
	memset(sp, 0, sizeof(*sp));
	sp->link = link;
	sp->cfg = cfg;
	sp->retry.cb = try_open;
}

void serial_start(struct serial_port *sp)
{
	try_open(&sp->retry);
}

void serial_restart(struct serial_port *sp)
{
	if (sp->open)
		close_port(sp, "Reconnecting after configuration change", true);
	uloop_timeout_cancel(&sp->retry);
	sp->error[0] = '\0';
	try_open(&sp->retry);
}

void serial_shutdown(struct serial_port *sp)
{
	uloop_timeout_cancel(&sp->retry);
	if (sp->open) {
		uloop_fd_delete(&sp->ufd);
		close(sp->ufd.fd);
		sp->open = false;
	}
}
