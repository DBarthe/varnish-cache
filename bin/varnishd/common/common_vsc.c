/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
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

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vdef.h"
#include "vrt.h"

#include "miniobj.h"
#include "vas.h"
#include "vmb.h"
#include "vsc_priv.h"
#include "vqueue.h"

#include "heritage.h"
#include "vsmw.h"

/*--------------------------------------------------------------------*/

struct vsc_seg {
	unsigned		magic;
#define VSC_SEG_MAGIC		0x9b355991

	struct vsmw		*vsm;		// keep master/child sep.
	const char		*nm;
	VTAILQ_ENTRY(vsc_seg)	list;
	void			*seg;

	/* VSC segments */
	struct vsc_head		*head;
	void			*ptr;
	struct vsc_seg		*doc;

	/* DOC segments */
	const unsigned char	*jp;
	int			refs;
};

static VTAILQ_HEAD(,vsc_seg)	vsc_seglist =
    VTAILQ_HEAD_INITIALIZER(vsc_seglist);

vsc_callback_f *vsc_lock;
vsc_callback_f *vsc_unlock;

static struct vsc_seg *
vrt_vsc_mksegv(const char *class, size_t payload, const char *fmt, va_list va)
{
	struct vsc_seg *vsg;
	size_t co;

	co = PRNDUP(sizeof(struct vsc_head));
	ALLOC_OBJ(vsg, VSC_SEG_MAGIC);
	AN(vsg);
	vsg->seg = VSMW_Allocv(heritage.proc_vsmw, class,
	    co + PRNDUP(payload), fmt, va);
	AN(vsg->seg);
	vsg->vsm = heritage.proc_vsmw;
	vsg->head = (void*)vsg->seg;
	vsg->head->body_offset = co;
	vsg->ptr = (char*)vsg->seg + co;
	return (vsg);
}

static struct vsc_seg *
vrt_vsc_mksegf(const char *class, size_t payload, const char *fmt, ...)
{
	va_list ap;
	struct vsc_seg *vsg;

	va_start(ap, fmt);
	vsg = vrt_vsc_mksegv(class, payload, fmt, ap);
	va_end(ap);
	return (vsg);
}

void *
VRT_VSC_Alloc(struct vsmw_cluster *vc, struct vsc_seg **sg,
    const char *nm, size_t sd,
    const unsigned char *jp, size_t sj, const char *fmt, va_list va)
{
	struct vsc_seg *vsg, *dvsg;
	char buf[1024];
	uintptr_t jjp;

	AZ(vc);
	if (vsc_lock != NULL)
		vsc_lock();

	jjp = (uintptr_t)jp;

	VTAILQ_FOREACH(dvsg, &vsc_seglist, list) {
		if (dvsg->vsm != heritage.proc_vsmw)
			continue;
		if (dvsg->jp == NULL || dvsg->jp == jp)
			break;
	}
	if (dvsg == NULL || dvsg->jp == NULL) {
		/* Create a new documentation segment */
		dvsg = vrt_vsc_mksegf(VSC_DOC_CLASS, sj,
		    "%jx", (uintmax_t)jjp);
		AN(dvsg);
		dvsg->jp = jp;
		dvsg->head->doc_id = jjp;
		memcpy(dvsg->ptr, jp, sj);
		VWMB();
		dvsg->head->ready = 1;
		VTAILQ_INSERT_HEAD(&vsc_seglist, dvsg, list);
	}
	AN(dvsg);
	dvsg->refs++;

	if (*fmt == '\0')
		bprintf(buf, "%s", nm);
	else
		bprintf(buf, "%s.%s", nm, fmt);

	AN(heritage.proc_vsmw);

	vsg = vrt_vsc_mksegv(VSC_CLASS, sd, buf, va);
	AN(vsg);
	vsg->nm = nm;
	vsg->doc = dvsg;
	vsg->head->doc_id = jjp;
	VTAILQ_INSERT_TAIL(&vsc_seglist, vsg, list);
	VWMB();
	vsg->head->ready = 1;
	if (vsc_unlock != NULL)
		vsc_unlock();
	if (sg != NULL)
		*sg = vsg;
	return (vsg->ptr);
}

void
VRT_VSC_Destroy(const char *nm, struct vsc_seg *vsg)
{
	struct vsc_seg *dvsg;

	if (vsc_lock != NULL)
		vsc_lock();

	AN(heritage.proc_vsmw);
	CHECK_OBJ_NOTNULL(vsg, VSC_SEG_MAGIC);
	AZ(vsg->jp);
	CHECK_OBJ_NOTNULL(vsg->doc, VSC_SEG_MAGIC);
	assert(vsg->vsm == heritage.proc_vsmw);
	assert(vsg->nm == nm);

	dvsg = vsg->doc;
	VSMW_Free(heritage.proc_vsmw, &vsg->seg);
	VTAILQ_REMOVE(&vsc_seglist, vsg, list);
	FREE_OBJ(vsg);
	if (--dvsg->refs == 0) {
		VSMW_Free(heritage.proc_vsmw, &dvsg->seg);
		VTAILQ_REMOVE(&vsc_seglist, dvsg, list);
		FREE_OBJ(dvsg);
	}
	if (vsc_unlock != NULL)
		vsc_unlock();
}
