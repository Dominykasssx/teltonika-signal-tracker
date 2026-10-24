#include "gsm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ev.h"

static int spawn(struct gsm_sampler *g, const char *option)
{
	const char *argv[6];
	int fds[2], argc = 0;
	pid_t pid;

	argv[argc++] = "gsmctl";
	if (g->cfg->modem[0]) {
		argv[argc++] = "-O";
		argv[argc++] = g->cfg->modem;
	}
	argv[argc++] = option;
	argv[argc] = NULL;

	if (pipe2(fds, O_CLOEXEC) != 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		int null = open("/dev/null", O_RDWR);

		dup2(fds[1], STDOUT_FILENO);
		if (null >= 0) {
			dup2(null, STDIN_FILENO);
			dup2(null, STDERR_FILENO);
		}
		execv(GSMCTL_PATH, (char *const *)argv);
		_exit(127);
	}

	close(fds[1]);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	g->len = 0;
	g->proc_done = false;
	g->out_done = false;
	g->timed_out = false;
	g->out.fd = fds[0];
	uloop_fd_add(&g->out, ULOOP_READ);
	g->proc.pid = pid;
	uloop_process_add(&g->proc);
	uloop_timeout_set(&g->timeout, GSMCTL_TIMEOUT_MS);
	return 0;
}

static void finish(struct gsm_sampler *g)
{
	g->running = false;
	g->sample.time_ms = ev_now();
	g->done(g->ctx, &g->sample);
}

static bool exited_ok(const struct gsm_sampler *g)
{
	return !g->timed_out && WIFEXITED(g->status) && WEXITSTATUS(g->status) == 0;
}

static void stage_complete(struct gsm_sampler *g)
{
	uloop_timeout_cancel(&g->timeout);
	g->buf[g->len] = '\0';

	if (g->stage == 0) {
		int found = 0;

		if (g->timed_out) {
			snprintf(g->sample.error, sizeof(g->sample.error), "gsmctl -q timed out");
		} else if (!exited_ok(g)) {
			snprintf(g->sample.error, sizeof(g->sample.error), "gsmctl -q failed (status %d)",
				 WIFEXITED(g->status) ? WEXITSTATUS(g->status) : -1);
		} else {
			g->sample.ok = true;
			found = signal_parse_levels(g->buf, &g->sample);
			if (found == 0)
				snprintf(g->sample.error, sizeof(g->sample.error),
					 "Modem reported no usable signal metrics");
		}
		if (found > 0) {
			g->stage = 1;
			if (spawn(g, "-C") == 0)
				return;
		}
		finish(g);
		return;
	}

	/* A missing cell id is not fatal; the sample just cannot detect cell changes. */
	if (exited_ok(g))
		signal_parse_cell(g->buf, &g->sample);
	finish(g);
}

static void drain(struct gsm_sampler *g)
{
	for (;;) {
		ssize_t n;

		if (g->len + 1 >= sizeof(g->buf)) {
			char sink[256];

			n = read(g->out.fd, sink, sizeof(sink)); /* discard excess output */
		} else {
			n = read(g->out.fd, g->buf + g->len, sizeof(g->buf) - 1 - g->len);
			if (n > 0)
				g->len += (size_t)n;
		}
		if (n > 0)
			continue;
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		/* EOF or error: output is complete */
		uloop_fd_delete(&g->out);
		close(g->out.fd);
		g->out_done = true;
		return;
	}
}

static void on_out(struct uloop_fd *u, unsigned int events)
{
	struct gsm_sampler *g = container_of(u, struct gsm_sampler, out);

	(void)events;
	drain(g);
	if (g->out_done && g->proc_done)
		stage_complete(g);
}

static void on_exit_cb(struct uloop_process *p, int status)
{
	struct gsm_sampler *g = container_of(p, struct gsm_sampler, proc);

	g->status = status;
	g->proc_done = true;
	if (!g->out_done)
		drain(g);
	if (g->out_done)
		stage_complete(g);
}

static void on_timeout(struct uloop_timeout *t)
{
	struct gsm_sampler *g = container_of(t, struct gsm_sampler, timeout);

	/* the exit callback completes the stage once the killed process is reaped */
	g->timed_out = true;
	if (!g->proc_done)
		kill(g->proc.pid, SIGKILL);
	if (!g->out_done) {
		uloop_fd_delete(&g->out);
		close(g->out.fd);
		g->out_done = true;
	}
	if (g->proc_done)
		stage_complete(g);
}

void gsm_sampler_init(struct gsm_sampler *g, const struct st_config *cfg,
		      void (*done)(void *ctx, const struct signal_sample *s), void *ctx)
{
	memset(g, 0, sizeof(*g));
	g->cfg = cfg;
	g->done = done;
	g->ctx = ctx;
	g->out.cb = on_out;
	g->proc.cb = on_exit_cb;
	g->timeout.cb = on_timeout;
}

int gsm_sampler_start(void *sampler)
{
	struct gsm_sampler *g = sampler;

	if (g->running)
		return -1;
	signal_sample_clear(&g->sample);
	g->stage = 0;
	if (spawn(g, "-q") != 0)
		return -1;
	g->running = true;
	return 0;
}
