#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include "ixp.h"

static void ixp_handle_req(Req *r);

void *
createfid(Intmap *map, int fid) {
	Fid *f = cext_emallocz(sizeof(Fid));
	f->fid = fid;
	f->omode = -1;
	f->map = map;
	if(caninsertkey(map, fid, f))
		return f;
	free(f);
	return nil;
}
int
destroyfid(Intmap *map, unsigned long fid) {
	Fid *f;
	if(!(f = deletekey(map, fid)))
		return 0;
	free(f);
	return 1;
}

static char
	Eduptag[] = "tag in use",
	Edupfid[] = "fid in use",
	Enofunc[] = "function not implemented",
	Ebotch[] = "9P protocol botch",
	Enofile[] = "the requested file does not exist",
	Enofid[] = "fid does not exist",
	Enotdir[] = "not a directory",
	Eisdir[] = "cannot perform operation on a directory";

enum {	TAG_BUCKETS = 64,
	FID_BUCKETS = 64 };

typedef struct P9Conn {
	Intmap	tagmap;
	void	*taghash[TAG_BUCKETS];
	Intmap	fidmap;
	void	*fidhash[FID_BUCKETS];
	P9Srv	*srv;
	unsigned int	msize;
	unsigned char	*buf;
} P9Conn;

void
ixp_server_handle_fcall(IXPConn *c)
{
	Fcall fcall;
	P9Conn *pc = c->aux;
	Req *req;
	unsigned int msize;
	char *errstr = nil;

	if(!(msize = ixp_recv_message(c->fd, pc->buf, pc->msize, &errstr)))
		goto Fail;
	if(!(msize = ixp_msg2fcall(&fcall, pc->buf, IXP_MAX_MSG)))
		goto Fail;

	req = cext_emallocz(sizeof(Req));
	req->conn = c;
	req->ifcall = fcall;

	if(lookupkey(&pc->tagmap, fcall.tag))
		return respond(req, Eduptag);

	insertkey(&pc->tagmap, fcall.tag, req);
	return ixp_handle_req(req);

Fail:
	ixp_server_close_conn(c);
}

static void
ixp_handle_req(Req *r)
{
	P9Conn *pc = r->conn->aux;
	P9Srv *srv = pc->srv;

	switch(r->ifcall.type) {
	default:
		respond(r, Enofunc);
		break;
	case TVERSION:
		if(!strncmp(r->ifcall.version, "9P", 3)) {
			r->ofcall.version = "9P";
		}else
		if(!strncmp(r->ifcall.version, "9P2000", 7)) {
			r->ofcall.version = "9P2000";
		}else{
			r->ofcall.version = "unknown";
		}
		r->ofcall.msize = r->ifcall.msize;
		respond(r, nil);
		break;
	case TATTACH:
		if(!(r->fid = createfid(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Edupfid);
		/* attach is a required function */
		srv->attach(r);
		break;
	case TCLUNK:
		if(!destroyfid(&pc->fidmap, r->ifcall.fid))
			return respond(r, Enofid);
		respond(r, nil);
		break;
	case TCREATE:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Enofid);
		if(r->fid->omode != -1)
			return respond(r, Ebotch);
		if(!(r->fid->qid.type&QTDIR))
			return respond(r, Enotdir);
		if(!pc->srv->create)
			return respond(r, Enofunc);
		pc->srv->create(r);
		break;
	case TOPEN:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Enofid);
		if((r->fid->qid.type&QTDIR) && (r->ifcall.mode|ORCLOSE) != (OREAD|ORCLOSE))
			return respond(r, Eisdir);
		r->ofcall.qid = r->fid->qid;
		if(!pc->srv->open)
			return respond(r, Enofunc);
		pc->srv->open(r);
		break;
	case TREAD:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Enofid);
		if(r->fid->omode == -1)
			return respond(r, Ebotch);
		if(!pc->srv->read)
			return respond(r, Enofunc);
		pc->srv->read(r);
		break;
	case TREMOVE:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Enofid);
		if(!pc->srv->remove)
			return respond(r, Enofunc);
		pc->srv->remove(r);
		break;
	case TSTAT:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Enofid);
		if(!pc->srv->stat)
			return respond(r, Enofunc);
		pc->srv->stat(r);
		break;
	case TWALK:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Enofid);
		if(r->fid->omode != -1)
			return respond(r, "cannot walk from an open fid");
		if(r->ifcall.nwname && !(r->fid->qid.type&QTDIR))
			return respond(r, Enotdir);
		if((r->ifcall.fid != r->ifcall.newfid)) {
			if(!(r->newfid = createfid(&pc->fidmap, r->ifcall.newfid)))
				return respond(r, Edupfid);
		}else
			r->newfid = r->fid;
		if(!pc->srv->walk)
			return respond(r, Enofunc);
		pc->srv->walk(r);
		break;
	case TWRITE:
		if(!(r->fid = lookupkey(&pc->fidmap, r->ifcall.fid)))
			return respond(r, Enofid);
		if((r->fid->omode&3) != OWRITE && (r->fid->omode&3) != ORDWR)
			return respond(r, "write on fid not opened for writing");
		if(!pc->srv->write)
			return respond(r, Enofunc);
		pc->srv->write(r);
		break;
	/* Still to be implemented: flush, wstat, auth */
	}
}

void
respond(Req *r, char *error) {
	P9Conn *pc = r->conn->aux;
	switch(r->ifcall.type) {
	default:
		if(!error)
			cext_assert(!"Respond called on unsupported fcall type");
		break;
	case TVERSION:
		cext_assert(!error);
		pc->msize = r->ofcall.msize;
		free(pc->buf);
		pc->buf = cext_emallocz(r->ofcall.msize);
		break;
	case TATTACH:
		if(error)
			destroyfid(r->fid->map, r->fid->fid);
		break;
	case TOPEN:
	case TCREATE:
		if(!error) {
			r->fid->omode = r->ofcall.mode;
			r->fid->qid = r->ofcall.qid;
		}
		break;
	case TWALK:
		if(error || r->ofcall.nwqid < r->ifcall.nwname) {
			if(r->ifcall.fid != r->ifcall.newfid && r->newfid)
				destroyfid(r->newfid->map, r->newfid->fid);
			if(!error && r->ofcall.nwqid == 0)
				error = Enofile;
		}else{
			if(r->ofcall.nwqid == 0)
				r->newfid->qid = r->fid->qid;
			else
				r->newfid->qid = r->ofcall.wqid[r->ofcall.nwqid-1];
		}
		break;
	case TCLUNK:
	case TREAD:
	case TREMOVE:
	case TSTAT:
	case TWRITE:
		break;
	/* Still to be implemented: flush, wstat, auth */
	}

	r->ofcall.tag = r->ifcall.tag;
	if(!error)
		r->ofcall.type = r->ifcall.type + 1;
	else {
		r->ofcall.type = RERROR;
		r->ofcall.ename = error;
	}

	/* XXX Check if conn is still open */
	ixp_server_respond_fcall(r->conn, &r->ofcall);

	switch(r->ofcall.type) {
	case RSTAT:
		free(r->ofcall.stat);
		break;
	case RREAD:
		free(r->ofcall.data);
		break;
	}
	switch(r->ifcall.type) {
	case TWALK:
		free(r->ifcall.wname[0]);
		break;
	case TWRITE:
		free(r->ifcall.data);
	}

	deletekey(&pc->tagmap, r->ifcall.tag);;
	free(r);
}

static void
ixp_void_request(void *r) {
	/* generate flush request */
}

static void
ixp_void_fid(void *f) {
	/* generate clunk request */
}

static void
ixp_cleanup_conn(IXPConn *c) {
	P9Conn *pc = c->aux;
	freemap(&pc->tagmap, ixp_void_request);
	freemap(&pc->fidmap, ixp_void_fid);
	free(pc->buf);
	free(pc);
}

void
serve_9pcon(IXPConn *c) {
	int fd = accept(c->fd, nil, nil);
	if(fd < 0)
		return;

	P9Conn *pc = cext_emallocz(sizeof(P9Conn));
	pc->srv = c->aux;

	/* XXX */
	pc->msize = 1024;
	pc->buf = cext_emallocz(pc->msize);

	initmap(&pc->tagmap, TAG_BUCKETS, &pc->taghash);
	initmap(&pc->fidmap, FID_BUCKETS, &pc->fidhash);

	ixp_server_open_conn(c->srv, fd, pc, ixp_server_handle_fcall, ixp_cleanup_conn);
}
