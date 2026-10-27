/*
 * signal-tracker: turns the router on a servo turntable to find the best mobile signal.
 *
 * The turntable is an ESP8266/ESP32 running the signal-tracker firmware on a USB serial
 * port (115200 baud), one text command per line:
 *   <deg>   move there  -> "MOVING <from> -> <to>", then "AT <deg>" once stopped
 *   ?       status      -> "POS <deg> TARGET <deg> SPEED <ms/deg> MAX <travel>"
 * Bad commands get "ERR ...", and the firmware prints "Turntable ready." at power-up.
 * The travel (MAX) is set on the ESP itself ("m270") and kept in its flash.
 * Replies are not numbered, so they are not paired with commands: every line simply
 * updates what is known about the turntable, and "?" is sent periodically.
 *
 * The settings are in section 'general' of /etc/config/signal_tracker. There is one
 * turntable, so one modem is tracked: gsmd's gsm.modem<N> for option modem, 0 the primary
 * and 1 the secondary modem. While enabled, it is read every 2 s: signal quality (%) and
 * RSRP (dBm). A scan splits the travel into equal sectors (positions A, B, C, ...),
 * measures the chosen metric, signal quality or RSRP, at the centre of each, then turns
 * to the best one.
 * Exposes the ubus object "signal_tracker".
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/utils.h>
#include <libubus.h>
#include <uci.h>

#define MAX_POSITIONS 26 /* A..Z */
#define MAX_MODEMS 2     /* gsm.modem0, gsm.modem1 */
#define POLL_IDLE_MS 2000
#define POLL_MOVING_MS 250
#define SIGNAL_POLL_MS 2000
#define REOPEN_MS 3000
#define SILENCE_MS 5000 /* no line from the turntable for this long: not responding */
#define SAMPLE_INTERVAL_MS 1000
#define MOVE_SLACK_MS 5000

enum metric { METRIC_QUALITY, METRIC_RSRP, __METRIC_MAX };

/* As in option metric; also the names of the readings in the status. */
static const char *const metric_names[] = { "signal_quality", "rsrp" };
static const char *const metric_labels[] = { "signal quality", "RSRP" };
static const char *const modem_labels[] = { "primary modem", "secondary modem" };

struct config {
	bool enabled;       /* off: the turntable's serial port is released */
	char device[64];    /* "auto" or a /dev path */
	int modem;          /* read from gsmd's gsm.modem<modem>: 0 primary, 1 secondary */
	enum metric metric; /* what a scan maximises */
	int positions;
	int settle;  /* seconds to wait at a position before measuring */
	int samples; /* signal readings per position, 1 s apart */
};

/* One reading from gsmd; a value it did not report has its has[] flag false. */
struct signal {
	bool has[__METRIC_MAX];
	int value[__METRIC_MAX];
};

/* What the turntable last reported. */
struct turntable {
	bool known; /* a POS line arrived since the port was opened */
	int angle;  /* where the servo is now; it steps one degree at a time */
	int target;
	int speed;
	int travel; /* angles go from 0 to travel */
	bool moving;
	long long last_rx;
	char error[128];
};

enum scan_state { SCAN_IDLE, SCAN_RUNNING, SCAN_DONE, SCAN_CANCELLED, SCAN_FAILED };
enum scan_step { STEP_MOVE, STEP_MEASURE, STEP_RETURN };

struct result {
	int samples;            /* readings taken */
	int n[__METRIC_MAX];    /* readings that reported each metric */
	int sum[__METRIC_MAX];
};

struct scan {
	int modem; /* the modem it measures, as option modem */
	enum metric metric;
	enum scan_state state;
	enum scan_step step;
	int positions, travel, samples, settle; /* fixed when the scan starts */
	bool reverse; /* measure Z..A because the turntable is closer to that end */
	int measured;
	int current;  /* position being moved to or measured */
	int best;     /* -1 until a position has a value */
	struct result res[MAX_POSITIONS];
	char message[128];
};

static const char *const state_names[] = { "idle", "running", "done", "cancelled", "failed" };
static const char *const step_names[] = { "moving", "measuring", "returning" };

/* USB serial drivers of ESP boards; the modem's own ttyUSB ports use "option". */
static const char *const port_drivers[] = {
	"ch341-uart", "cp210x", "ftdi_sio", "pl2303", "cdc_acm", "usb_ch343",
};

static struct config cfg;
static struct turntable tt;
static bool present[MAX_MODEMS]; /* gsmd has an object for the modem */
static struct signal live;       /* latest reading */
static int live_modem = -1;      /* the modem it is from; -1: none, read at the next status */
static struct scan last;         /* the running or last scan */
static struct scan *running;     /* &last while a scan is in progress */

/* A move without "async" gets its ubus reply once the turntable reports "AT <target>". */
static struct {
	bool active;
	int target;
	struct ubus_request_data req;
} waiting;

static struct uloop_fd port = { .fd = -1 };
static char port_path[64];
static char rx[128];
static size_t rx_len;
static struct uloop_timeout reopen_timer, poll_timer, scan_timer, move_timer, signal_timer;

static struct ubus_auto_conn conn;
static struct blob_buf b;

static void scan_end(enum scan_state state, const char *fmt, ...);
static void move_done(const char *error);

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Centre of sector i when the travel is split into equal sectors. */
static int position_angle(int i, int positions, int travel)
{
	return ((2 * i + 1) * travel + positions) / (2 * positions);
}

/* ------------------------------------------------------------------ configuration */

static const char *opt_str(struct uci_context *ctx, struct uci_section *s, const char *name)
{
	return s ? uci_lookup_option_string(ctx, s, name) : NULL;
}

static int opt_int(struct uci_context *ctx, struct uci_section *s, const char *name, int def,
		   int min, int max)
{
	const char *v = opt_str(ctx, s, name);
	int n = v ? atoi(v) : def;

	return n < min || n > max ? def : n;
}

/* Section 'general'; without one, the defaults. */
static void load_config(void)
{
	struct uci_context *ctx = uci_alloc_context();
	struct uci_package *pkg = NULL;
	struct uci_section *s = NULL;
	const char *v;

	if (ctx && uci_load(ctx, "signal_tracker", &pkg) == UCI_OK)
		s = uci_lookup_section(ctx, pkg, "general");
	cfg.enabled = opt_int(ctx, s, "enabled", 1, 0, 1);
	v = opt_str(ctx, s, "device");
	snprintf(cfg.device, sizeof(cfg.device), "%s", v && *v ? v : "auto");
	cfg.modem = opt_int(ctx, s, "modem", 0, 0, MAX_MODEMS - 1);
	/* 'signal_quality' or 'rsrp' */
	v = opt_str(ctx, s, "metric");
	cfg.metric = v && strcmp(v, "rsrp") == 0 ? METRIC_RSRP : METRIC_QUALITY;
	cfg.positions = opt_int(ctx, s, "positions", 8, 2, MAX_POSITIONS);
	cfg.settle = opt_int(ctx, s, "settle", 15, 0, 60);
	cfg.samples = opt_int(ctx, s, "samples", 5, 1, 10);
	if (ctx)
		uci_free_context(ctx);
}

/* ------------------------------------------------------------------ serial port */

static void set_error(const char *fmt, ...)
{
	char msg[sizeof(tt.error)];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (strcmp(msg, tt.error) != 0) /* log changes only, not every retry */
		syslog(LOG_WARNING, "%s", msg);
	snprintf(tt.error, sizeof(tt.error), "%s", msg);
}

static bool responding(void)
{
	return port.fd >= 0 && tt.known && now_ms() - tt.last_rx < SILENCE_MS;
}

static void close_port(const char *why)
{
	if (port.fd < 0)
		return;
	uloop_fd_delete(&port);
	close(port.fd);
	port.fd = -1;
	tt.known = false;
	tt.moving = false;
	set_error("%s: %s", port_path, why);
	if (running)
		scan_end(SCAN_FAILED, "Turntable disconnected");
	move_done("Turntable disconnected");
	uloop_timeout_set(&reopen_timer, REOPEN_MS);
}

static void send_cmd(const char *fmt, ...)
{
	char line[32];
	va_list ap;
	int n;

	if (port.fd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
	va_end(ap);
	line[n++] = '\n';
	if (write(port.fd, line, n) != n)
		close_port("write failed");
}

static void send_move(int angle)
{
	send_cmd("%d", angle);
	tt.moving = true;
	uloop_timeout_set(&poll_timer, POLL_MOVING_MS); /* follow the platform closely */
}

/* Time allowed for a move: the firmware steps one degree every <speed> ms. */
static int move_time_ms(int angle)
{
	return abs(tt.angle - angle) * tt.speed * 3 / 2 + MOVE_SLACK_MS;
}

static bool find_port(char *path, size_t len)
{
	DIR *d;
	struct dirent *e;
	bool found = false;

	if (strcmp(cfg.device, "auto") != 0) {
		snprintf(path, len, "%s", cfg.device);
		return true;
	}
	d = opendir("/sys/class/tty");
	if (!d)
		return false;
	while (!found && (e = readdir(d))) {
		char link[300], target[PATH_MAX];
		const char *driver;
		ssize_t n;

		if (strncmp(e->d_name, "ttyUSB", 6) && strncmp(e->d_name, "ttyACM", 6) &&
		    strncmp(e->d_name, "ttyCH343USB", 11))
			continue;
		snprintf(link, sizeof(link), "/sys/class/tty/%s/device/driver", e->d_name);
		n = readlink(link, target, sizeof(target) - 1);
		if (n <= 0)
			continue;
		target[n] = '\0';
		driver = strrchr(target, '/') ? strrchr(target, '/') + 1 : target;
		for (size_t i = 0; i < ARRAY_SIZE(port_drivers) && !found; i++) {
			if (strcmp(driver, port_drivers[i]) == 0) {
				snprintf(path, len, "/dev/%.32s", e->d_name);
				found = true;
			}
		}
	}
	closedir(d);
	return found;
}

static void handle_line(const char *line)
{
	int a, b, c, d;

	if (strstr(line, "Turntable ready")) {
		/* the firmware restarted and drove the servo to the middle of its travel */
		tt.known = false;
		if (running)
			scan_end(SCAN_FAILED, "Turntable restarted during the scan");
		move_done("Turntable restarted during the move");
		send_cmd("?");
	} else if (sscanf(line, "POS %d TARGET %d SPEED %d MAX %d", &a, &b, &c, &d) == 4) {
		tt.known = true;
		tt.angle = a;
		tt.target = b;
		tt.speed = c;
		tt.travel = d;
		tt.moving = a != b;
	} else if (sscanf(line, "MOVING %d -> %d", &a, &b) == 2) {
		tt.angle = a;
		tt.target = b;
		tt.moving = true;
		tt.error[0] = '\0';
	} else if (sscanf(line, "AT %d", &a) == 1) {
		tt.angle = tt.target = a;
		tt.moving = false;
		if (waiting.active && a == waiting.target)
			move_done(NULL);
		if (running && running->step != STEP_MEASURE &&
		    a == position_angle(running->current, running->positions, running->travel)) {
			if (running->step == STEP_RETURN) {
				scan_end(SCAN_DONE, "Turned to the best position %c (%d°)",
					 'A' + running->best, a);
			} else {
				running->step = STEP_MEASURE;
				uloop_timeout_set(&scan_timer, running->settle * 1000);
			}
		}
	} else if (strncmp(line, "ERR", 3) == 0) {
		set_error("Turntable: %s", line + 3 + strspn(line + 3, " "));
		if (running && running->step != STEP_MEASURE)
			scan_end(SCAN_FAILED, "%s", tt.error);
		move_done(tt.error);
	} else {
		return; /* boot ROM output or other noise */
	}
	tt.last_rx = now_ms();
}

static void on_port(struct uloop_fd *u, unsigned int events)
{
	char buf[256];
	ssize_t n;

	(void)events;
	while ((n = read(u->fd, buf, sizeof(buf))) > 0) {
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r') {
				rx[rx_len] = '\0';
				if (rx_len)
					handle_line(rx);
				rx_len = 0;
			} else if (rx_len < sizeof(rx) - 1) {
				rx[rx_len++] = buf[i];
			}
		}
		if (port.fd < 0)
			return; /* closed while handling a line */
	}
	if (n == 0 || (errno != EAGAIN && errno != EINTR) || u->eof || u->error)
		close_port("disconnected");
}

static void open_port(struct uloop_timeout *t)
{
	struct termios tio;
	int bits = TIOCM_DTR | TIOCM_RTS;
	int fd;

	load_config();
	if (!cfg.enabled)
		return;
	if (!find_port(port_path, sizeof(port_path))) {
		set_error("No turntable found on USB (connect the ESP board or set the device)");
		uloop_timeout_set(t, REOPEN_MS);
		return;
	}
	fd = open(port_path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0 || tcgetattr(fd, &tio) != 0) {
		set_error("Cannot open %s: %s", port_path, strerror(errno));
		if (fd >= 0)
			close(fd);
		uloop_timeout_set(t, REOPEN_MS);
		return;
	}
	cfmakeraw(&tio);
	tio.c_cflag |= CLOCAL | CREAD;
	tio.c_cflag &= ~(HUPCL | CRTSCTS);
	cfsetispeed(&tio, B115200);
	cfsetospeed(&tio, B115200);
	tcsetattr(fd, TCSANOW, &tio);
	/* dev boards wire DTR/RTS to the ESP's reset and boot pins: release both */
	ioctl(fd, TIOCMBIC, &bits);
	tcflush(fd, TCIOFLUSH);

	port.fd = fd;
	port.cb = on_port;
	uloop_fd_add(&port, ULOOP_READ);
	rx_len = 0;
	tt.error[0] = '\0';
	syslog(LOG_INFO, "opened %s", port_path);
	send_cmd("?");
}

static void on_poll(struct uloop_timeout *t)
{
	send_cmd("?");
	uloop_timeout_set(t, tt.moving ? POLL_MOVING_MS : POLL_IDLE_MS);
}

/* Starts or stops using the turntable after "enabled" was changed. */
static void apply_enabled(void)
{
	if (cfg.enabled) {
		if (port.fd < 0 && !reopen_timer.pending)
			uloop_timeout_set(&reopen_timer, 0);
		return;
	}
	if (running)
		scan_end(SCAN_CANCELLED, "Signal tracker was disabled");
	move_done("Signal tracker was disabled");
	live_modem = -1; /* not read while disabled */
	if (port.fd >= 0) {
		close_port("released, signal tracker disabled");
		syslog(LOG_INFO, "disabled");
	}
	uloop_timeout_cancel(&reopen_timer);
	tt.error[0] = '\0';
}

/* ------------------------------------------------------------------ signal */

/* gsmd's object for a modem: gsm.modem0 is the primary modem, gsm.modem1 the secondary. */
static bool modem_object(int modem, uint32_t *id)
{
	char name[16];

	snprintf(name, sizeof(name), "gsm.modem%d", modem);
	return ubus_lookup_id(&conn.ctx, name, id) == 0;
}

static void find_modems(void)
{
	uint32_t id;

	for (int i = 0; i < MAX_MODEMS; i++)
		present[i] = modem_object(i, &id);
}

/* Values outside the 3GPP reporting ranges are gsmd's "not available" markers. */
static void take(struct ubus_request *req, struct blob_attr *msg, const char *name,
		 enum metric m, int lo, int hi)
{
	const struct blobmsg_policy policy = { name, BLOBMSG_TYPE_INT32 };
	struct signal *sig = req->priv;
	struct blob_attr *a;
	int v;

	blobmsg_parse(&policy, 1, &a, blob_data(msg), blob_len(msg));
	if (!a)
		return;
	v = (int32_t)blobmsg_get_u32(a);
	if (v >= lo && v <= hi) {
		sig->has[m] = true;
		sig->value[m] = v;
	}
}

/* { "signal_quality_value": 62 }, a percentage */
static void on_quality(struct ubus_request *req, int type, struct blob_attr *msg)
{
	(void)type;
	take(req, msg, "signal_quality_value", METRIC_QUALITY, 0, 100);
}

/* { "net_mode": "LTE", "rssi": -48, "rsrp": -86, "sinr": 12, "rsrq": -17, ... } */
static void on_query(struct ubus_request *req, int type, struct blob_attr *msg)
{
	(void)type;
	take(req, msg, "rsrp", METRIC_RSRP, -156, -31);
}

/* Reads a modem's signal from gsmd into live (cached values, a few ms per call). */
static void read_signal(int modem)
{
	uint32_t id;

	memset(&live, 0, sizeof(live));
	live_modem = modem;
	if (!modem_object(modem, &id))
		return;
	ubus_invoke(&conn.ctx, id, "get_signal_quality_value", NULL, on_quality, &live, 1000);
	ubus_invoke(&conn.ctx, id, "get_signal_query", NULL, on_query, &live, 1000);
}

/* The modem being measured: the scanned one during a scan. */
static int active_modem(void)
{
	return running ? running->modem : cfg.modem;
}

static void on_signal_timer(struct uloop_timeout *t)
{
	load_config(); /* also notices "enabled" set with uci from the shell */
	apply_enabled();
	find_modems();
	if (cfg.enabled)
		read_signal(active_modem());
	uloop_timeout_set(t, SIGNAL_POLL_MS);
}

/* ------------------------------------------------------------------ scan */

static void scan_end(enum scan_state state, const char *fmt, ...)
{
	struct scan *s = running;
	va_list ap;

	if (!s)
		return;
	running = NULL;
	va_start(ap, fmt);
	vsnprintf(s->message, sizeof(s->message), fmt, ap);
	va_end(ap);
	s->state = state;
	uloop_timeout_cancel(&scan_timer);
	syslog(state == SCAN_FAILED ? LOG_WARNING : LOG_INFO, "scan %s: %s", state_names[state],
	       s->message);
}

static void scan_move(int position)
{
	int angle = position_angle(position, running->positions, running->travel);

	running->current = position;
	uloop_timeout_set(&scan_timer, move_time_ms(angle));
	send_move(angle);
}

static void on_scan_timer(struct uloop_timeout *t)
{
	struct scan *s = running;
	struct result *r;
	enum metric m;

	if (!s)
		return;
	if (s->step != STEP_MEASURE) {
		scan_end(SCAN_FAILED, "Turntable did not reach %d°",
			 position_angle(s->current, s->positions, s->travel));
		return;
	}

	/* both metrics are recorded, so the results show either */
	read_signal(s->modem);
	r = &s->res[s->current];
	r->samples++;
	for (m = 0; m < __METRIC_MAX; m++) {
		if (live.has[m]) {
			r->n[m]++;
			r->sum[m] += live.value[m];
		}
	}
	if (r->samples < s->samples) {
		uloop_timeout_set(t, SAMPLE_INTERVAL_MS);
		return;
	}

	/* compare averages as sum/n without dividing */
	m = s->metric;
	if (r->n[m] && (s->best < 0 || (long long)r->sum[m] * s->res[s->best].n[m] >
					      (long long)s->res[s->best].sum[m] * r->n[m]))
		s->best = s->current;
	if (++s->measured < s->positions) {
		scan_move(s->reverse ? s->positions - 1 - s->measured : s->measured);
		return;
	}
	if (s->best < 0) {
		scan_end(SCAN_FAILED, "Could not read the %s of the %s", metric_labels[m],
			 modem_labels[s->modem]);
		return;
	}
	s->step = STEP_RETURN;
	scan_move(s->best);
}

/* Scans with the configured modem, metric and positions. */
static const char *scan_start(void)
{
	struct scan *s = &last;
	uint32_t id;

	load_config();
	if (!cfg.enabled)
		return "Signal tracker is disabled";
	if (running)
		return "A scan is already running";
	if (!responding())
		return "Turntable is not connected";
	if (!modem_object(cfg.modem, &id))
		return cfg.modem ? "The secondary modem is not available" :
				   "The primary modem is not available";

	move_done("Interrupted by a scan");
	memset(s, 0, sizeof(*s));
	s->modem = cfg.modem;
	s->metric = cfg.metric;
	s->state = SCAN_RUNNING;
	s->step = STEP_MOVE;
	s->positions = cfg.positions;
	s->travel = tt.travel;
	s->samples = cfg.samples;
	s->settle = cfg.settle;
	s->best = -1;
	/* start from the end the turntable is closer to */
	s->reverse = tt.angle > s->travel / 2;
	running = s;
	syslog(LOG_INFO, "scan started: %s of the %s at %d positions over %d°",
	       metric_labels[s->metric], modem_labels[s->modem], s->positions, s->travel);
	scan_move(s->reverse ? s->positions - 1 : 0);
	return NULL;
}

/* ------------------------------------------------------------------ ubus */

static void add_mean(const char *name, int sum, int n)
{
	if (n)
		blobmsg_add_double(&b, name, round(10.0 * sum / n) / 10);
}

static void add_turntable(void)
{
	void *t = blobmsg_open_table(&b, "turntable");
	const char *state = !cfg.enabled  ? "disabled" :
			    port.fd < 0   ? "disconnected" :
			    responding() ? "connected" : "not responding";

	blobmsg_add_string(&b, "state", state);
	if (port.fd >= 0)
		blobmsg_add_string(&b, "device", port_path);
	if (tt.error[0])
		blobmsg_add_string(&b, "error", tt.error);
	if (tt.known) {
		blobmsg_add_u32(&b, "angle", tt.angle);
		blobmsg_add_u32(&b, "target", tt.target);
		blobmsg_add_u8(&b, "moving", tt.moving);
		blobmsg_add_u32(&b, "travel", tt.travel);
	}
	blobmsg_close_table(&b, t);
}

/* "signal": { "signal_quality": 62, "rsrp": -86 }, without the values gsmd did not report */
static void add_signal(void)
{
	void *t = blobmsg_open_table(&b, "signal");

	for (int m = 0; m < __METRIC_MAX; m++) {
		if (live.has[m])
			blobmsg_add_u32(&b, metric_names[m], live.value[m]);
	}
	blobmsg_close_table(&b, t);
}

static void add_scan(const struct scan *s)
{
	void *t = blobmsg_open_table(&b, "scan"), *a;

	blobmsg_add_string(&b, "state", state_names[s->state]);
	if (s->state != SCAN_IDLE) {
		blobmsg_add_u32(&b, "modem", s->modem);
		blobmsg_add_string(&b, "metric", metric_names[s->metric]);
		blobmsg_add_u32(&b, "positions", s->positions);
		blobmsg_add_u32(&b, "travel", s->travel);
		blobmsg_add_u32(&b, "measured", s->measured);
		if (s == running) {
			blobmsg_add_string(&b, "step", step_names[s->step]);
			blobmsg_add_u32(&b, "current", s->current);
		}
		if (s->best >= 0)
			blobmsg_add_u32(&b, "best", s->best);
		if (s->message[0])
			blobmsg_add_string(&b, "message", s->message);

		a = blobmsg_open_array(&b, "results");
		for (int i = 0; i < s->positions; i++) {
			const struct result *r = &s->res[i];
			void *e;

			if (!r->samples)
				continue;
			e = blobmsg_open_table(&b, NULL);
			blobmsg_add_u32(&b, "position", i);
			blobmsg_add_u32(&b, "angle", position_angle(i, s->positions, s->travel));
			blobmsg_add_u32(&b, "samples", r->samples);
			for (int m = 0; m < __METRIC_MAX; m++)
				add_mean(metric_names[m], r->sum[m], r->n[m]);
			blobmsg_close_table(&b, e);
		}
		blobmsg_close_array(&b, a);
	}
	blobmsg_close_table(&b, t);
}

/* "modems": [0, 1], the modems gsmd has objects for */
static void add_modems(void)
{
	void *a = blobmsg_open_array(&b, "modems");

	for (int i = 0; i < MAX_MODEMS; i++) {
		if (present[i])
			blobmsg_add_u32(&b, NULL, i);
	}
	blobmsg_close_array(&b, a);
}

static int reply(struct ubus_context *ctx, struct ubus_request_data *req, const char *error)
{
	blob_buf_init(&b, 0);
	if (error)
		blobmsg_add_string(&b, "error", error);
	ubus_send_reply(ctx, req, b.head);
	return UBUS_STATUS_OK;
}

static int m_status(struct ubus_context *ctx, struct ubus_object *obj,
		    struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj, (void)method, (void)msg;
	load_config(); /* settings saved in the WebUI apply without a restart */
	apply_enabled();
	find_modems();
	if (cfg.enabled && live_modem != active_modem())
		read_signal(active_modem()); /* the modem was changed in the settings */
	blob_buf_init(&b, 0);
	blobmsg_add_u8(&b, "enabled", cfg.enabled);
	blobmsg_add_u32(&b, "modem", active_modem());
	add_modems();
	blobmsg_add_string(&b, "metric", metric_names[cfg.metric]);
	blobmsg_add_u32(&b, "positions", cfg.positions);
	add_turntable();
	if (cfg.enabled)
		add_signal();
	add_scan(&last);
	ubus_send_reply(ctx, req, b.head);
	return UBUS_STATUS_OK;
}

/* Replies to a waiting move: { "angle": <target> } once there, or { "error": ... }. */
static void move_done(const char *error)
{
	if (!waiting.active)
		return;
	waiting.active = false;
	uloop_timeout_cancel(&move_timer);
	blob_buf_init(&b, 0);
	if (error)
		blobmsg_add_string(&b, "error", error);
	else
		blobmsg_add_u32(&b, "angle", waiting.target);
	ubus_send_reply(&conn.ctx, &waiting.req, b.head);
	ubus_complete_deferred_request(&conn.ctx, &waiting.req, UBUS_STATUS_OK);
}

static void on_move_timeout(struct uloop_timeout *t)
{
	char msg[64];

	(void)t;
	snprintf(msg, sizeof(msg), "Turntable did not reach %d°", waiting.target);
	move_done(msg);
}

enum { MOVE_ANGLE, MOVE_ASYNC, __MOVE_MAX };

static const struct blobmsg_policy move_policy[__MOVE_MAX] = {
	[MOVE_ANGLE] = { .name = "angle", .type = BLOBMSG_TYPE_INT32 },
	[MOVE_ASYNC] = { .name = "async", .type = BLOBMSG_TYPE_BOOL },
};

static int m_move(struct ubus_context *ctx, struct ubus_object *obj,
		  struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	struct blob_attr *tb[__MOVE_MAX];
	int angle, timeout;

	(void)obj, (void)method;
	blobmsg_parse(move_policy, __MOVE_MAX, tb, blob_data(msg), blob_len(msg));
	if (!tb[MOVE_ANGLE])
		return reply(ctx, req, "Angle must be a whole number");
	angle = (int32_t)blobmsg_get_u32(tb[MOVE_ANGLE]);
	if (!cfg.enabled)
		return reply(ctx, req, "Signal tracker is disabled");
	if (running)
		return reply(ctx, req, "Stop the scan first");
	if (!responding())
		return reply(ctx, req, "Turntable is not connected");
	if (angle < 0 || angle > tt.travel)
		return reply(ctx, req, "Angle is outside the turntable travel");

	move_done("Interrupted by another move");
	timeout = move_time_ms(angle);
	send_move(angle);
	if (port.fd < 0)
		return reply(ctx, req, "Turntable disconnected");
	if (tb[MOVE_ASYNC] && blobmsg_get_bool(tb[MOVE_ASYNC]))
		return reply(ctx, req, NULL);

	waiting.active = true;
	waiting.target = angle;
	ubus_defer_request(ctx, req, &waiting.req);
	uloop_timeout_set(&move_timer, timeout);
	return UBUS_STATUS_OK;
}

static int m_scan_stop(struct ubus_context *ctx, struct ubus_object *obj,
		       struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj, (void)method, (void)msg;
	/* a move already sent still finishes; nothing new is started */
	if (running)
		scan_end(SCAN_CANCELLED, "Stopped by user");
	return reply(ctx, req, NULL);
}

static int m_scan_start(struct ubus_context *ctx, struct ubus_object *obj,
			struct ubus_request_data *req, const char *method, struct blob_attr *msg)
{
	(void)obj, (void)method, (void)msg;
	return reply(ctx, req, scan_start());
}

static const struct ubus_method methods[] = {
	UBUS_METHOD_NOARG("status", m_status),
	UBUS_METHOD("move", m_move, move_policy),
	UBUS_METHOD_NOARG("scan_start", m_scan_start),
	UBUS_METHOD_NOARG("scan_stop", m_scan_stop),
};

static struct ubus_object_type obj_type = UBUS_OBJECT_TYPE("signal_tracker", methods);

static struct ubus_object obj = {
	.name = "signal_tracker",
	.type = &obj_type,
	.methods = methods,
	.n_methods = ARRAY_SIZE(methods),
};

static void on_ubus_connect(struct ubus_context *ctx)
{
	if (ubus_add_object(ctx, &obj) != 0)
		syslog(LOG_ERR, "failed to register the ubus object");
}

/* ------------------------------------------------------------------ main */

int main(void)
{
	openlog("signal-tracker", 0, LOG_DAEMON);
	signal(SIGPIPE, SIG_IGN);
	uloop_init();

	reopen_timer.cb = open_port;
	poll_timer.cb = on_poll;
	scan_timer.cb = on_scan_timer;
	move_timer.cb = on_move_timeout;
	signal_timer.cb = on_signal_timer;
	open_port(&reopen_timer);
	uloop_timeout_set(&poll_timer, POLL_IDLE_MS);
	uloop_timeout_set(&signal_timer, SIGNAL_POLL_MS);

	conn.cb = on_ubus_connect;
	ubus_auto_connect(&conn);
	uloop_run();

	ubus_auto_shutdown(&conn);
	if (port.fd >= 0)
		close(port.fd);
	uloop_done();
	return 0;
}
