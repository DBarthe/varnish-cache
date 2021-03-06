/*-
 * Copyright (c) 2008-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef VTEST_WITH_VTC_VARNISH

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <fcntl.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vtc.h"

#include "vapi/vsc.h"
#include "vapi/vsl.h"
#include "vapi/vsm.h"
#include "vcli.h"
#include "vjsn.h"
#include "vre.h"
#include "vsub.h"
#include "vtcp.h"
#include "vtim.h"

struct varnish {
	unsigned		magic;
#define VARNISH_MAGIC		0x208cd8e3
	char			*name;
	struct vtclog		*vl;
	VTAILQ_ENTRY(varnish)	list;

	struct vsb		*args;
	int			fds[4];
	pid_t			pid;

	double			syntax;

	pthread_t		tp;
	pthread_t		tp_vsl;

	int			expect_exit;

	int			cli_fd;
	int			vcl_nbr;
	char			*workdir;
	char			*jail;
	char			*proto;

	struct vsm		*vsm_vsl;
	struct vsm		*vsm_vsc;
	struct vsc		*vsc;
	int			has_a_arg;

	unsigned		vsl_tag_count[256];

	volatile int		vsl_rec;
	volatile int		vsl_idle;
};

#define NONSENSE	"%XJEIFLH|)Xspa8P"

static VTAILQ_HEAD(, varnish)	varnishes =
    VTAILQ_HEAD_INITIALIZER(varnishes);

/**********************************************************************
 * Ask a question over CLI
 */

static enum VCLI_status_e
varnish_ask_cli(const struct varnish *v, const char *cmd, char **repl)
{
	int i;
	unsigned retval;
	char *r;

	if (cmd != NULL) {
		vtc_dump(v->vl, 4, "CLI TX", cmd, -1);
		i = write(v->cli_fd, cmd, strlen(cmd));
		if (i != strlen(cmd) && !vtc_stop)
			vtc_fatal(v->vl, "CLI write failed (%s) = %u %s",
			    cmd, errno, strerror(errno));
		i = write(v->cli_fd, "\n", 1);
		if (i != 1 && !vtc_stop)
			vtc_fatal(v->vl, "CLI write failed (%s) = %u %s",
			    cmd, errno, strerror(errno));
	}
	i = VCLI_ReadResult(v->cli_fd, &retval, &r, vtc_maxdur);
	if (i != 0 && !vtc_stop)
		vtc_fatal(v->vl, "CLI failed (%s) = %d %u %s",
		    cmd, i, retval, r);
	vtc_log(v->vl, 3, "CLI RX  %u", retval);
	vtc_dump(v->vl, 4, "CLI RX", r, -1);
	if (repl != NULL)
		*repl = r;
	else
		free(r);
	return ((enum VCLI_status_e)retval);
}

/**********************************************************************
 *
 */

static void
wait_stopped(const struct varnish *v)
{
	char *r = NULL;
	enum VCLI_status_e st;

	vtc_log(v->vl, 3, "wait-stopped");
	while (1) {
		st = varnish_ask_cli(v, "status", &r);
		if (st != CLIS_OK)
			vtc_fatal(v->vl,
			    "CLI status command failed: %u %s", st, r);
		if (!strcmp(r, "Child in state stopped")) {
			free(r);
			break;
		}
		free(r);
		r = NULL;
		(void)usleep(200000);
	}
}
/**********************************************************************
 *
 */

static void
wait_running(const struct varnish *v)
{
	char *r = NULL;
	enum VCLI_status_e st;

	while (1) {
		vtc_log(v->vl, 3, "wait-running");
		st = varnish_ask_cli(v, "status", &r);
		if (st != CLIS_OK)
			vtc_fatal(v->vl,
			    "CLI status command failed: %u %s", st, r);
		if (!strcmp(r, "Child in state stopped"))
			vtc_fatal(v->vl,
			    "Child stopped before running: %u %s", st, r);
		if (!strcmp(r, "Child in state running")) {
			st = varnish_ask_cli(v, "debug.listen_address", &r);
			if (st != CLIS_OK)
				vtc_fatal(v->vl,
					  "CLI status command failed: %u %s",
					  st, r);
			free(r);
			break;
		}
		free(r);
		r = NULL;
		(void)usleep(200000);
	}
}

/**********************************************************************
 * Varnishlog gatherer thread
 */

static void
vsl_catchup(const struct varnish *v)
{
	int vsl_idle;

	vsl_idle = v->vsl_idle;
	while (!vtc_error && vsl_idle == v->vsl_idle)
		VTIM_sleep(0.1);
}

static void *
varnishlog_thread(void *priv)
{
	struct varnish *v;
	struct VSL_data *vsl;
	struct vsm *vsm;
	struct VSL_cursor *c;
	enum VSL_tag_e tag;
	uint32_t vxid;
	unsigned len;
	const char *tagname, *data;
	int type, i, opt;
	struct vsb *vsb = NULL;

	CAST_OBJ_NOTNULL(v, priv, VARNISH_MAGIC);

	vsl = VSL_New();
	AN(vsl);
	vsm = v->vsm_vsl;

	c = NULL;
	opt = 0;
	while (v->fds[1] > 0 || c != NULL) {	//lint !e845 bug in flint
		if (c == NULL) {
			if (vtc_error)
				break;
			VTIM_sleep(0.1);
			(void)VSM_Status(vsm);
			c = VSL_CursorVSM(vsl, vsm, opt);
			if (c == NULL) {
				vtc_log(v->vl, 3, "vsl|%s", VSL_Error(vsl));
				VSL_ResetError(vsl);
				continue;
			}
		}
		AN(c);

		opt = VSL_COPT_TAIL;

		while (1) {
			i = VSL_Next(c);
			if (i != 1)
				break;

			v->vsl_rec = 1;

			tag = VSL_TAG(c->rec.ptr);
			vxid = VSL_ID(c->rec.ptr);
			if (tag == SLT__Batch)
				continue;
			tagname = VSL_tags[tag];
			len = VSL_LEN(c->rec.ptr);
			type = VSL_CLIENT(c->rec.ptr) ?
			    'c' : VSL_BACKEND(c->rec.ptr) ?
			    'b' : '-';
			data = VSL_CDATA(c->rec.ptr);
			v->vsl_tag_count[tag]++;
			if (VSL_tagflags[tag] & SLT_F_BINARY) {
				if (vsb == NULL)
					vsb = VSB_new_auto();
				VSB_clear(vsb);
				VSB_quote(vsb, data, len, VSB_QUOTE_HEX);
				AZ(VSB_finish(vsb));
				/* +2 to skip "0x" */
				vtc_log(v->vl, 4, "vsl| %10u %-15s %c [%s]",
				    vxid, tagname, type, VSB_data(vsb) + 2);
			} else {
				vtc_log(v->vl, 4, "vsl| %10u %-15s %c %.*s",
				    vxid, tagname, type, (int)len, data);
			}
		}
		if (i == 0) {
			/* Nothing to do but wait */
			v->vsl_idle++;
			if (!(VSM_Status(vsm) & VSM_WRK_RUNNING)) {
				/* Abandoned - try reconnect */
				VSL_DeleteCursor(c);
				c = NULL;
			} else {
				VTIM_sleep(0.1);
			}
		} else if (i == -2) {
			/* Abandoned - try reconnect */
			VSL_DeleteCursor(c);
			c = NULL;
		} else
			break;
	}

	if (c)
		VSL_DeleteCursor(c);
	VSL_Delete(vsl);
	if (vsb != NULL)
		VSB_destroy(&vsb);

	return (NULL);
}

/**********************************************************************
 * Allocate and initialize a varnish
 */

static struct varnish *
varnish_new(const char *name)
{
	struct varnish *v;
	struct vsb *vsb;
	char buf[1024];

	ALLOC_OBJ(v, VARNISH_MAGIC);
	AN(v);
	REPLACE(v->name, name);

	REPLACE(v->jail, "");

	v->vl = vtc_logopen(name);
	AN(v->vl);

	vsb = macro_expandf(v->vl, "${tmpdir}/%s", name);
	AN(vsb);
	v->workdir = strdup(VSB_data(vsb));
	AN(v->workdir);
	VSB_destroy(&vsb);

	bprintf(buf, "rm -rf %s ; mkdir -p %s", v->workdir, v->workdir);
	AZ(system(buf));

	v->args = VSB_new_auto();

	v->cli_fd = -1;
	VTAILQ_INSERT_TAIL(&varnishes, v, list);


	return (v);
}

/**********************************************************************
 * Delete a varnish instance
 */

static void
varnish_delete(struct varnish *v)
{

	CHECK_OBJ_NOTNULL(v, VARNISH_MAGIC);
	vtc_logclose(v->vl);
	free(v->name);
	free(v->workdir);
	if (v->vsc != NULL)
		VSC_Destroy(&v->vsc, v->vsm_vsc);
	if (v->vsm_vsc != NULL)
		VSM_Destroy(&v->vsm_vsc);
	if (v->vsm_vsl != NULL)
		VSM_Destroy(&v->vsm_vsl);

	/*
	 * We do not delete the workdir, it may contain stuff people
	 * want (coredumps, shmlog/stats etc), and trying to divine
	 * "may want" is just too much trouble.  Leave it around and
	 * nuke it at the start of the next test-run.
	 */

	/* XXX: MEMLEAK (?) */
	FREE_OBJ(v);
}

/**********************************************************************
 * Varnish listener
 */

static void *
varnish_thread(void *priv)
{
	struct varnish *v;

	CAST_OBJ_NOTNULL(v, priv, VARNISH_MAGIC);
	return (vtc_record(v->vl, v->fds[0], NULL));
}

/**********************************************************************
 * Launch a Varnish
 */

static void
varnish_launch(struct varnish *v)
{
	struct vsb *vsb, *vsb1;
	int i, nfd;
	char abuf[128], pbuf[128];
	struct pollfd fd[2];
	enum VCLI_status_e u;
	const char *err;
	char *r = NULL;

	/* Create listener socket */
	v->cli_fd = VTCP_listen_on("127.0.0.1:0", NULL, 1, &err);
	if (err != NULL)
		vtc_fatal(v->vl, "Create CLI listen socket failed: %s", err);
	assert(v->cli_fd > 0);
	VTCP_myname(v->cli_fd, abuf, sizeof abuf, pbuf, sizeof pbuf);

	AZ(VSB_finish(v->args));
	vtc_log(v->vl, 2, "Launch");
	vsb = VSB_new_auto();
	AN(vsb);
	VSB_printf(vsb, "cd ${pwd} &&");
	VSB_printf(vsb, " exec varnishd %s -d -n %s",
	    v->jail, v->workdir);
	VSB_cat(vsb, VSB_data(params_vsb));
	if (vtc_witness)
		VSB_cat(vsb, " -p debug=+witness");
	if (leave_temp) {
		VSB_cat(vsb, " -p debug=+vcl_keep");
		VSB_cat(vsb, " -p debug=+vmod_so_keep");
		VSB_cat(vsb, " -p debug=+vsm_keep");
	}
	VSB_printf(vsb, " -l 2m");
	VSB_printf(vsb, " -p auto_restart=off");
	VSB_printf(vsb, " -p syslog_cli_traffic=off");
	VSB_printf(vsb, " -p sigsegv_handler=on");
	VSB_printf(vsb, " -p thread_pool_min=10");
	VSB_printf(vsb, " -p debug=+vtc_mode");
	if (!v->has_a_arg) {
		VSB_printf(vsb, " -a '%s'", "127.0.0.1:0");
		if (v->proto != NULL)
			VSB_printf(vsb, ",%s", v->proto);
	}
	VSB_printf(vsb, " -M '%s %s'", abuf, pbuf);
	VSB_printf(vsb, " -P %s/varnishd.pid", v->workdir);
	if (vmod_path != NULL)
		VSB_printf(vsb, " -p vmod_path=%s", vmod_path);
	VSB_printf(vsb, " %s", VSB_data(v->args));
	AZ(VSB_finish(vsb));
	vtc_log(v->vl, 3, "CMD: %s", VSB_data(vsb));
	vsb1 = macro_expand(v->vl, VSB_data(vsb));
	AN(vsb1);
	VSB_destroy(&vsb);
	vsb = vsb1;
	vtc_log(v->vl, 3, "CMD: %s", VSB_data(vsb));
	AZ(pipe(&v->fds[0]));
	AZ(pipe(&v->fds[2]));
	v->pid = fork();
	assert(v->pid >= 0);
	if (v->pid == 0) {
		AZ(dup2(v->fds[0], 0));
		assert(dup2(v->fds[3], 1) == 1);
		assert(dup2(1, 2) == 2);
		closefd(&v->fds[0]);
		closefd(&v->fds[1]);
		closefd(&v->fds[2]);
		closefd(&v->fds[3]);
		VSUB_closefrom(STDERR_FILENO + 1);
		AZ(execl("/bin/sh", "/bin/sh", "-c", VSB_data(vsb), (char*)0));
		exit(1);
	} else {
		vtc_log(v->vl, 3, "PID: %ld", (long)v->pid);
		macro_def(v->vl, v->name, "pid", "%ld", (long)v->pid);
		macro_def(v->vl, v->name, "name", "%s", v->workdir);
	}
	closefd(&v->fds[0]);
	closefd(&v->fds[3]);
	v->fds[0] = v->fds[2];
	v->fds[2] = v->fds[3] = -1;
	VSB_destroy(&vsb);
	AZ(pthread_create(&v->tp, NULL, varnish_thread, v));

	/* Wait for the varnish to call home */
	memset(fd, 0, sizeof fd);
	fd[0].fd = v->cli_fd;
	fd[0].events = POLLIN;
	fd[1].fd = v->fds[1];
	fd[1].events = POLLIN;
	i = poll(fd, 2, vtc_maxdur * 1000 / 3);
	vtc_log(v->vl, 4, "CLIPOLL %d 0x%x 0x%x",
	    i, fd[0].revents, fd[1].revents);
	if (i == 0)
		vtc_fatal(v->vl, "FAIL timeout waiting for CLI connection");
	if (fd[1].revents & POLLHUP)
		vtc_fatal(v->vl, "FAIL debug pipe closed");
	if (!(fd[0].revents & POLLIN))
		vtc_fatal(v->vl, "FAIL CLI connection wait failure");
	nfd = accept(v->cli_fd, NULL, NULL);
	if (nfd < 0) {
		closefd(&v->cli_fd);
		vtc_fatal(v->vl, "FAIL no CLI connection accepted");
	}

	closefd(&v->cli_fd);
	v->cli_fd = nfd;

	vtc_log(v->vl, 3, "CLI connection fd = %d", v->cli_fd);
	assert(v->cli_fd >= 0);

	/* Receive the banner or auth response */
	u = varnish_ask_cli(v, NULL, &r);
	if (vtc_error)
		return;
	if (u != CLIS_AUTH)
		vtc_fatal(v->vl, "CLI auth demand expected: %u %s", u, r);

	bprintf(abuf, "%s/_.secret", v->workdir);
	nfd = open(abuf, O_RDONLY);
	assert(nfd >= 0);

	assert(sizeof abuf >= CLI_AUTH_RESPONSE_LEN + 7);
	strcpy(abuf, "auth ");
	VCLI_AuthResponse(nfd, r, abuf + 5);
	closefd(&nfd);
	free(r);
	r = NULL;
	strcat(abuf, "\n");

	u = varnish_ask_cli(v, abuf, &r);
	if (vtc_error)
		return;
	if (u != CLIS_OK)
		vtc_fatal(v->vl, "CLI auth command failed: %u %s", u, r);
	free(r);

	v->vsm_vsc = VSM_New();
	AN(v->vsm_vsc);
	v->vsc = VSC_New();
	AN(v->vsc);
	assert(VSM_Arg(v->vsm_vsc, 'n', v->workdir) > 0);
	AZ(VSM_Attach(v->vsm_vsc, -1));

	v->vsm_vsl = VSM_New();
	assert(VSM_Arg(v->vsm_vsl, 'n', v->workdir) > 0);
	AZ(VSM_Attach(v->vsm_vsl, -1));

	AZ(pthread_create(&v->tp_vsl, NULL, varnishlog_thread, v));
}

/**********************************************************************
 * Start a Varnish
 */

static void
varnish_start(struct varnish *v)
{
	enum VCLI_status_e u;
	char *resp = NULL, *h, *p;

	if (v->cli_fd < 0)
		varnish_launch(v);
	if (vtc_error)
		return;
	vtc_log(v->vl, 2, "Start");
	u = varnish_ask_cli(v, "start", &resp);
	if (vtc_error)
		return;
	if (u != CLIS_OK)
		vtc_fatal(v->vl, "CLI start command failed: %u %s", u, resp);
	wait_running(v);
	free(resp);
	resp = NULL;
	u = varnish_ask_cli(v, "debug.xid 999", &resp);
	if (vtc_error)
		return;
	if (u != CLIS_OK)
		vtc_fatal(v->vl, "CLI debug.xid command failed: %u %s",
		    u, resp);
	free(resp);
	resp = NULL;
	u = varnish_ask_cli(v, "debug.listen_address", &resp);
	if (vtc_error)
		return;
	if (u != CLIS_OK)
		vtc_fatal(v->vl,
		    "CLI debug.listen_address command failed: %u %s", u, resp);
	h = resp;
	p = strchr(h, '\n');
	if (p != NULL)
		*p = '\0';
	p = strchr(h, ' ');
	AN(p);
	*p++ = '\0';
	vtc_log(v->vl, 2, "Listen on %s %s", h, p);
	macro_def(v->vl, v->name, "addr", "%s", h);
	macro_def(v->vl, v->name, "port", "%s", p);
	macro_def(v->vl, v->name, "sock", "%s %s", h, p);
	free(resp);
	/* Wait for vsl logging to get underway */
	while (v->vsl_rec == 0)
		VTIM_sleep(.1);
}

/**********************************************************************
 * Stop a Varnish
 */

static void
varnish_stop(struct varnish *v)
{
	if (v->cli_fd < 0)
		varnish_launch(v);
	if (vtc_error)
		return;
	vtc_log(v->vl, 2, "Stop");
	(void)varnish_ask_cli(v, "stop", NULL);
	wait_stopped(v);
}

/**********************************************************************
 * Cleanup
 */

static void
varnish_cleanup(struct varnish *v)
{
	void *p;

	/* Close the CLI connection */
	closefd(&v->cli_fd);

	/* Close the STDIN connection. */
	closefd(&v->fds[1]);

	/* Wait until STDOUT+STDERR closes */
	AZ(pthread_join(v->tp, &p));
	closefd(&v->fds[0]);

	/* Pick up the VSL thread */
	AZ(pthread_join(v->tp_vsl, &p));

	vtc_wait4(v->vl, v->pid, v->expect_exit, 0, 0);
	v->pid = 0;
}

/**********************************************************************
 * Wait for a Varnish
 */

static void
varnish_wait(struct varnish *v)
{
	if (v->cli_fd < 0)
		return;

	vtc_log(v->vl, 2, "Wait");

	if (!vtc_error) {
		/* Do a backend.list to log if child is still running */
		(void)varnish_ask_cli(v, "backend.list", NULL);
	}

	/* Then stop it */
	varnish_stop(v);

	if (varnish_ask_cli(v, "panic.clear", NULL) != CLIS_CANT)
		vtc_fatal(v->vl, "Unexpected panic");

	varnish_cleanup(v);
}


/**********************************************************************
 * Ask a CLI JSON question
 */

static void
varnish_cli_json(struct varnish *v, const char *cli)
{
	enum VCLI_status_e u;
	char *resp = NULL;
	const char *errptr;
	struct vjsn *vj;

	if (v->cli_fd < 0)
		varnish_launch(v);
	if (vtc_error)
		return;
	u = varnish_ask_cli(v, cli, &resp);
	vtc_log(v->vl, 2, "CLI %03u <%s>", u, cli);
	if (u != CLIS_OK)
		vtc_fatal(v->vl,
		    "FAIL CLI response %u expected %u", u, CLIS_OK);
	vj = vjsn_parse(resp, &errptr);
	if (vj == NULL)
		vtc_fatal(v->vl, "FAIL CLI, not good JSON: %s", errptr);
	vjsn_delete(&vj);
	free(resp);
}

/**********************************************************************
 * Ask a CLI question
 */

static void
varnish_cli(struct varnish *v, const char *cli, unsigned exp, const char *re)
{
	enum VCLI_status_e u;
	vre_t *vre = NULL;
	char *resp = NULL;
	const char *errptr;
	int err;

	if (re != NULL) {
		vre = VRE_compile(re, 0, &errptr, &err);
		if (vre == NULL)
			vtc_fatal(v->vl, "Illegal regexp");
	}
	if (v->cli_fd < 0)
		varnish_launch(v);
	if (vtc_error) {
		if (vre != NULL)
			VRE_free(&vre);
		return;
	}
	u = varnish_ask_cli(v, cli, &resp);
	vtc_log(v->vl, 2, "CLI %03u <%s>", u, cli);
	if (exp != 0 && exp != (unsigned)u)
		vtc_fatal(v->vl, "FAIL CLI response %u expected %u", u, exp);
	if (vre != NULL) {
		err = VRE_exec(vre, resp, strlen(resp), 0, 0, NULL, 0, NULL);
		if (err < 1)
			vtc_fatal(v->vl, "Expect failed (%d)", err);
		VRE_free(&vre);
	}
	free(resp);
}

/**********************************************************************
 * Load a VCL program
 */

static void
varnish_vcl(struct varnish *v, const char *vcl, int fail, char **resp)
{
	struct vsb *vsb;
	enum VCLI_status_e u;

	if (v->cli_fd < 0)
		varnish_launch(v);
	if (vtc_error)
		return;
	vsb = VSB_new_auto();
	AN(vsb);

	VSB_printf(vsb, "vcl.inline vcl%d << %s\nvcl %.1f;\n%s\n%s\n",
	    ++v->vcl_nbr, NONSENSE, v->syntax, vcl, NONSENSE);
	AZ(VSB_finish(vsb));

	u = varnish_ask_cli(v, VSB_data(vsb), resp);
	if (u == CLIS_OK) {
		VSB_clear(vsb);
		VSB_printf(vsb, "vcl.use vcl%d", v->vcl_nbr);
		AZ(VSB_finish(vsb));
		u = varnish_ask_cli(v, VSB_data(vsb), NULL);
	}
	if (u == CLIS_OK && fail) {
		VSB_destroy(&vsb);
		vtc_fatal(v->vl, "VCL compilation succeeded expected failure");
	} else if (u != CLIS_OK && !fail) {
		VSB_destroy(&vsb);
		vtc_fatal(v->vl, "VCL compilation failed expected success");
	} else if (fail)
		vtc_log(v->vl, 2, "VCL compilation failed (as expected)");
	VSB_destroy(&vsb);
}

/**********************************************************************
 * Load a VCL program prefixed by backend decls for our servers
 */

static void
varnish_vclbackend(struct varnish *v, const char *vcl)
{
	struct vsb *vsb, *vsb2;
	enum VCLI_status_e u;

	if (v->cli_fd < 0)
		varnish_launch(v);
	if (vtc_error)
		return;
	vsb = VSB_new_auto();
	AN(vsb);

	vsb2 = VSB_new_auto();
	AN(vsb2);

	VSB_printf(vsb2, "vcl %.1f;\n", v->syntax);

	cmd_server_gen_vcl(vsb2);

	AZ(VSB_finish(vsb2));

	VSB_printf(vsb, "vcl.inline vcl%d << %s\n%s\n%s\n%s\n",
	    ++v->vcl_nbr, NONSENSE, VSB_data(vsb2), vcl, NONSENSE);
	AZ(VSB_finish(vsb));

	u = varnish_ask_cli(v, VSB_data(vsb), NULL);
	if (u != CLIS_OK) {
		VSB_destroy(&vsb);
		VSB_destroy(&vsb2);
		vtc_fatal(v->vl, "FAIL VCL does not compile");
	}
	VSB_clear(vsb);
	VSB_printf(vsb, "vcl.use vcl%d", v->vcl_nbr);
	AZ(VSB_finish(vsb));
	u = varnish_ask_cli(v, VSB_data(vsb), NULL);
	assert(u == CLIS_OK);
	VSB_destroy(&vsb);
	VSB_destroy(&vsb2);
}

/**********************************************************************
 */

struct dump_priv {
	const char *arg;
	const struct varnish *v;
};

static int
do_stat_dump_cb(void *priv, const struct VSC_point * const pt)
{
	const struct varnish *v;
	struct dump_priv *dp;
	uint64_t u;

	if (pt == NULL)
		return (0);
	dp = priv;
	v = dp->v;

	if (strcmp(pt->ctype, "uint64_t"))
		return (0);
	u = *pt->ptr;

	if (strcmp(dp->arg, "*")) {
		if (fnmatch(dp->arg, pt->name, 0))
			return (0);
	}

	vtc_log(v->vl, 4, "VSC %s %ju",  pt->name, (uintmax_t)u);
	return (0);
}

static void
varnish_vsc(const struct varnish *v, const char *arg)
{
	struct dump_priv dp;

	memset(&dp, 0, sizeof dp);
	dp.v = v;
	dp.arg = arg;
	(void)VSM_Status(v->vsm_vsc);
	(void)VSC_Iter(v->vsc, v->vsm_vsc, do_stat_dump_cb, &dp);
}

/**********************************************************************
 * Check statistics
 */

struct stat_priv {
	char target_pattern[256];
	uintmax_t val;
	const struct varnish *v;
};

static int
do_expect_cb(void *priv, const struct VSC_point * const pt)
{
	struct stat_priv *sp = priv;

	if (pt == NULL)
		return (0);

	if (fnmatch(sp->target_pattern, pt->name, 0))
		return (0);

	AZ(strcmp(pt->ctype, "uint64_t"));
	AN(pt->ptr);
	sp->val = *pt->ptr;
	return (1);
}

/**********************************************************************
 */

static void
varnish_expect(const struct varnish *v, char * const *av)
{
	uint64_t ref;
	int good;
	char *r;
	char *p;
	int i, not = 0;
	struct stat_priv sp;

	r = av[0];
	if (r[0] == '!') {
		not = 1;
		r++;
		AZ(av[1]);
	} else {
		AN(av[1]);
		AN(av[2]);
	}
	p = strrchr(r, '.');
	if (p == NULL) {
		bprintf(sp.target_pattern, "MAIN.%s", r);
	} else {
		bprintf(sp.target_pattern, "%s", r);
	}

	sp.val = 0;
	sp.v = v;
	ref = 0;
	good = 0;
	for (i = 0; i < 50; i++, (void)usleep(100000)) {
		(void)VSM_Status(v->vsm_vsc);
		good = VSC_Iter(v->vsc, v->vsm_vsc, do_expect_cb, &sp);
		if (!good) {
			good = -2;
			continue;
		}

		if (not)
			vtc_fatal(v->vl, "Found (not expected): %s", av[0]+1);

		good = 0;
		ref = strtoumax(av[2], &p, 0);
		if (ref == UINTMAX_MAX || *p)
			vtc_fatal(v->vl, "Syntax error in number (%s)", av[2]);
		if      (!strcmp(av[1], "==")) { if (sp.val == ref) good = 1; }
		else if (!strcmp(av[1], "!=")) { if (sp.val != ref) good = 1; }
		else if (!strcmp(av[1], ">"))  { if (sp.val > ref)  good = 1; }
		else if (!strcmp(av[1], "<"))  { if (sp.val < ref)  good = 1; }
		else if (!strcmp(av[1], ">=")) { if (sp.val >= ref) good = 1; }
		else if (!strcmp(av[1], "<=")) { if (sp.val <= ref) good = 1; }
		else
			vtc_fatal(v->vl, "comparison %s unknown", av[1]);
		if (good)
			break;
	}
	if (good == -1) {
		vtc_fatal(v->vl, "VSM error: %s", VSM_Error(v->vsm_vsc));
	}
	if (good == -2) {
		if (not) {
			vtc_log(v->vl, 2, "not found (as expected): %s",
			    av[0] + 1);
			return;
		}
		vtc_fatal(v->vl, "stats field %s unknown", av[0]);
	}

	if (good == 1) {
		vtc_log(v->vl, 2, "as expected: %s (%ju) %s %s",
		    av[0], sp.val, av[1], av[2]);
	} else {
		vtc_fatal(v->vl, "Not true: %s (%ju) %s %s (%ju)",
		    av[0], (uintmax_t)sp.val, av[1], av[2], (uintmax_t)ref);
	}
}

/* SECTION: varnish varnish
 *
 * Define and interact with varnish instances.
 *
 * To define a Varnish server, you'll use this syntax::
 *
 *	varnish vNAME [-arg STRING] [-vcl STRING] [-vcl+backend STRING]
 *		[-errvcl STRING STRING] [-jail STRING] [-proto PROXY]
 *
 * The first ``varnish vNAME`` invocation will start the varnishd master
 * process in the background, waiting for the ``-start`` switch to actually
 * start the child.
 *
 * Types used in the description below:
 *
 * PATTERN
 *         is a 'glob' style pattern (ie: fnmatch(3)) as used in shell filename
 *         expansion.
 *
 * Arguments:
 *
 * vNAME
 *	   Identify the Varnish server with a string, it must starts with 'v'.
 *
 * \-arg STRING
 *         Pass an argument to varnishd, for example "-h simple_list".
 *
 * \-vcl STRING
 *         Specify the VCL to load on this Varnish instance. You'll probably
 *         want to use multi-lines strings for this ({...}).
 *
 * \-vcl+backend STRING
 *         Do the exact same thing as -vcl, but adds the definition block of
 *         known backends (ie. already defined).
 *
 * \-errvcl STRING1 STRING2
 *         Load STRING2 as VCL, expecting it to fail, and Varnish to send an
 *         error string matching STRING2
 *
 * \-jail STRING
 *         Look at ``man varnishd`` (-j) for more information.
 *
 * \-proto PROXY
 *         Have Varnish use the proxy protocol. Note that PROXY here is the
 *         actual string.
 *
 * You can decide to start the Varnish instance and/or wait for several events::
 *
 *         varnish vNAME [-start] [-wait] [-wait-running] [-wait-stopped]
 *
 * \-start
 *         Start the child process.
 *
 * \-stop
 *         Stop the child process.
 *
 * \-syntax
 *         Set the VCL syntax level for this command (default: 4.1)
 *
 * \-wait
 *         Wait for that instance to terminate.
 *
 * \-wait-running
 *         Wait for the Varnish child process to be started.
 *
 * \-wait-stopped
 *         Wait for the Varnish child process to stop.
 *
 * \-cleanup
 *         Once Varnish is stopped, clean everything after it. This is only used
 *         in very few tests and you should never need it.
 *
 * Once Varnish is started, you can talk to it (as you would through
 * ``varnishadm``) with these additional switches::
 *
 *         varnish vNAME [-cli STRING] [-cliok STRING] [-clierr STRING]
 *                       [-clijson STRING] [-expect STRING OP NUMBER]
 *
 * \-cli STRING|-cliok STRING|-clierr STATUS STRING|-cliexpect REGEXP STRING
 *         All four of these will send STRING to the CLI, the only difference
 *         is what they expect the result to be. -cli doesn't expect
 *         anything, -cliok expects 200, -clierr expects STATUS, and
 *         -cliexpect expects the REGEXP to match the returned response.
 *
 * \-clijson STRING
 *	   Send STRING to the CLI, expect success (CLIS_OK/200) and check
 *	   that the response is parsable JSON.
 *
 * \-expect PATTERN OP NUMBER
 *         Look into the VSM and make sure the first VSC counter identified by
 *         PATTERN has a correct value. OP can be ==, >, >=, <, <=. For
 *         example::
 *
 *                 varnish v1 -expect SM?.s1.g_space > 1000000
 * \-expectexit NUMBER
 *	   Expect varnishd to exit(3) with this value
 *
 * \-vsc PATTERN
 *         Dump VSC counters matching PATTERN.
 *
 * \-vsl_catchup
 *         Wait until the logging thread has idled to make sure that all
 *         the generated log is flushed
 */

void
cmd_varnish(CMD_ARGS)
{
	struct varnish *v, *v2;

	(void)priv;
	(void)cmd;

	if (av == NULL) {
		/* Reset and free */
		VTAILQ_FOREACH_SAFE(v, &varnishes, list, v2) {
			if (v->cli_fd >= 0)
				varnish_wait(v);
			VTAILQ_REMOVE(&varnishes, v, list);
			varnish_delete(v);
		}
		return;
	}

	AZ(strcmp(av[0], "varnish"));
	av++;

	VTC_CHECK_NAME(vl, av[0], "Varnish", 'v');
	VTAILQ_FOREACH(v, &varnishes, list)
		if (!strcmp(v->name, av[0]))
			break;
	if (v == NULL)
		v = varnish_new(av[0]);
	av++;
	v->syntax = 4.1;

	for (; *av != NULL; av++) {
		if (vtc_error)
			break;
		if (!strcmp(*av, "-arg")) {
			AN(av[1]);
			AZ(v->pid);
			VSB_cat(v->args, " ");
			VSB_cat(v->args, av[1]);
			if (av[1][0] == '-' && av[1][1] == 'a')
				v->has_a_arg = 1;
			av++;
			continue;
		}
		if (!strcmp(*av, "-cleanup")) {
			AZ(av[1]);
			varnish_cleanup(v);
			continue;
		}
		if (!strcmp(*av, "-cli")) {
			AN(av[1]);
			varnish_cli(v, av[1], 0, NULL);
			av++;
			continue;
		}
		if (!strcmp(*av, "-clierr")) {
			AN(av[1]);
			AN(av[2]);
			varnish_cli(v, av[2], atoi(av[1]), NULL);
			av += 2;
			continue;
		}
		if (!strcmp(*av, "-cliexpect")) {
			AN(av[1]);
			AN(av[2]);
			varnish_cli(v, av[2], 0, av[1]);
			av += 2;
			continue;
		}
		if (!strcmp(*av, "-clijson")) {
			AN(av[1]);
			varnish_cli_json(v, av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-cliok")) {
			AN(av[1]);
			varnish_cli(v, av[1], (unsigned)CLIS_OK, NULL);
			av++;
			continue;
		}
		if (!strcmp(*av, "-errvcl")) {
			char *r = NULL;
			AN(av[1]);
			AN(av[2]);
			varnish_vcl(v, av[2], 1, &r);
			if (strstr(r, av[1]) == NULL)
				vtc_fatal(v->vl,
				    "Did not find expected string: (\"%s\")",
				    av[1]);
			else
				vtc_log(v->vl, 3,
				    "Found expected string: (\"%s\")",
				    av[1]);
			free(r);
			av += 2;
			continue;
		}
		if (!strcmp(*av, "-expect")) {
			av++;
			varnish_expect(v, av);
			av += 2;
			continue;
		}
		if (!strcmp(*av, "-expectexit")) {
			v->expect_exit = strtoul(av[1], NULL, 0);
			av++;
			continue;
		}
		if (!strcmp(*av, "-jail")) {
			AN(av[1]);
			AZ(v->pid);
			REPLACE(v->jail, av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-proto")) {
			AN(av[1]);
			AZ(v->pid);
			REPLACE(v->proto, av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-start")) {
			varnish_start(v);
			continue;
		}
		if (!strcmp(*av, "-stop")) {
			varnish_stop(v);
			continue;
		}
		if (!strcmp(*av, "-syntax")) {
			AN(av[1]);
			v->syntax = strtod(av[1], NULL);
			av++;
			continue;
		}
		if (!strcmp(*av, "-vcl")) {
			AN(av[1]);
			varnish_vcl(v, av[1], 0, NULL);
			av++;
			continue;
		}
		if (!strcmp(*av, "-vcl+backend")) {
			AN(av[1]);
			varnish_vclbackend(v, av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-vsc")) {
			AN(av[1]);
			varnish_vsc(v, av[1]);
			av++;
			continue;
		}
		if (!strcmp(*av, "-wait-stopped")) {
			wait_stopped(v);
			continue;
		}
		if (!strcmp(*av, "-wait-running")) {
			wait_running(v);
			continue;
		}
		if (!strcmp(*av, "-wait")) {
			varnish_wait(v);
			continue;
		}
		if (!strcmp(*av, "-vsl_catchup")) {
			vsl_catchup(v);
			continue;
		}
		vtc_fatal(v->vl, "Unknown varnish argument: %s", *av);
	}
}

#endif /* VTEST_WITH_VTC_VARNISH */
