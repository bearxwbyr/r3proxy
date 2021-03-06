/*
 * twemproxy - A fast and lightweight proxy for memcached protocol.
 * Copyright (C) 2011 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nc_core.h>
#include <nc_server.h>
#include <netdb.h>

static void
check_out_slowlog(struct context *ctx, struct server_pool *sp, struct msg *msg);

struct msg *
rsp_get(struct conn *conn)
{
    struct msg *msg;

    ASSERT(!conn->client && !conn->proxy);

    msg = msg_get(conn, false, conn->redis);
    if (msg == NULL) {
        conn->err = errno;
    }

    return msg;
}

void
rsp_put(struct msg *msg)
{
    ASSERT(!msg->request);
    ASSERT(msg->peer == NULL);
    msg_put(msg);
}

static struct msg *
rsp_make_error(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg;        /* peer message (response) */
    struct msg *cmsg, *nmsg; /* current and next message (request) */
    uint64_t id;
    err_t err;

    ASSERT(conn->client && !conn->proxy);
    ASSERT(msg->request && req_error(conn, msg));
    ASSERT(msg->owner == conn);

    id = msg->frag_id;
    if (id != 0) {
        for (err = 0, cmsg = TAILQ_NEXT(msg, c_tqe);
             cmsg != NULL && cmsg->frag_id == id;
             cmsg = nmsg) {
            nmsg = TAILQ_NEXT(cmsg, c_tqe);

            /* dequeue request (error fragment) from client outq */
            conn->dequeue_outq(ctx, conn, cmsg);
            if (err == 0 && cmsg->err != 0) {
                err = cmsg->err;
            }

            req_put(cmsg);
        }
    } else {
        err = msg->err;
    }

    pmsg = msg->peer;
    if (pmsg != NULL) {
        ASSERT(!pmsg->request && pmsg->peer == msg);
        msg->peer = NULL;
        pmsg->peer = NULL;
        rsp_put(pmsg);
    }

    return msg_get_error(conn->redis, err);
}

struct msg *
rsp_recv_next(struct context *ctx, struct conn *conn, bool alloc)
{
    struct msg *msg;

    ASSERT(!conn->client && !conn->proxy);

    if (conn->eof) {
        msg = conn->rmsg;

        /* server sent eof before sending the entire request */
        if (msg != NULL) {
            conn->rmsg = NULL;

            ASSERT(msg->peer == NULL);
            ASSERT(!msg->request);

            log_error("eof s %d discarding incomplete rsp %"PRIu64" len "
                      "%"PRIu32"", conn->sd, msg->id, msg->mlen);

            rsp_put(msg);
        }

        /*
         * We treat TCP half-close from a server different from how we treat
         * those from a client. On a FIN from a server, we close the connection
         * immediately by sending the second FIN even if there were outstanding
         * or pending requests. This is actually a tricky part in the FA, as
         * we don't expect this to happen unless the server is misbehaving or
         * it crashes
         */
        conn->done = 1;
        log_error("s %d active %d is done", conn->sd, conn->active(conn));

        return NULL;
    }

    msg = conn->rmsg;
    if (msg != NULL) {
        ASSERT(!msg->request);
        return msg;
    }

    if (!alloc) {
        return NULL;
    }

    msg = rsp_get(conn);
    if (msg != NULL) {
        conn->rmsg = msg;
    }

    return msg;
}

static bool
rsp_filter(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct server_pool *sp;
    struct server *s;
    struct msg *pmsg;

    ASSERT(!conn->client && !conn->proxy);

    if (msg_empty(msg)) {
        ASSERT(conn->rmsg == NULL);
        log_debug(LOG_VERB, "filter empty rsp %"PRIu64" on s %d", msg->id,
                  conn->sd);
        rsp_put(msg);
        return true;
    }

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL) {
        log_debug(LOG_ERR, "filter stray rsp %"PRIu64" len %"PRIu32" on s %d",
                  msg->id, msg->mlen, conn->sd);
        rsp_put(msg);

        /*
         * Memcached server can respond with an error response before it has
         * received the entire request. This is most commonly seen for set
         * requests that exceed item_size_max. IMO, this behavior of memcached
         * is incorrect. The right behavior for update requests that are over
         * item_size_max would be to either:
         * - close the connection Or,
         * - read the entire item_size_max data and then send CLIENT_ERROR
         *
         * We handle this stray packet scenario in nutcracker by closing the
         * server connection which would end up sending SERVER_ERROR to all
         * clients that have requests pending on this server connection. The
         * fix is aggressive, but not doing so would lead to clients getting
         * out of sync with the server and as a result clients end up getting
         * responses that don't correspond to the right request.
         *
         * See: https://github.com/twitter/twemproxy/issues/149
         */
        conn->err = EINVAL;
        conn->done = 1;
        return true;
    }
    ASSERT(pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    if (pmsg->swallow) {
        conn->swallow_msg(conn, pmsg, msg);

        conn->dequeue_outq(ctx, conn, pmsg);
        pmsg->done = 1;

        log_debug(LOG_INFO, "swallow rsp %"PRIu64" len %"PRIu32" of req "
                  "%"PRIu64" on s %d", msg->id, msg->mlen, pmsg->id,
                  conn->sd);

        rsp_put(msg);
        req_put(pmsg);
        return true;
    }

    return false;
}

static void
rsp_forward_stats(struct context *ctx, struct server *server, struct msg *msg, uint32_t msgsize)
{
    ASSERT(!msg->request);

    stats_server_incr(ctx, server, responses);
    stats_server_incr_by(ctx, server, response_bytes, msgsize);
}

static void
rsp_forward(struct context *ctx, struct conn *s_conn, struct msg *msg)
{
    rstatus_t status;
    struct msg *pmsg;
    struct conn *c_conn;
    uint32_t msgsize;
    struct server_pool *sp;
    struct server *server;

    msgsize = msg->mlen;

    /* response from server implies that server is ok and heartbeating */
    server_ok(ctx, s_conn);

    /* dequeue peer message (request) from server */
    pmsg = TAILQ_FIRST(&s_conn->omsg_q);
    ASSERT(pmsg != NULL && pmsg->peer == NULL);
    ASSERT(pmsg->request && !pmsg->done);

    s_conn->dequeue_outq(ctx, s_conn, pmsg);

    /* establish msg <-> pmsg (response <-> request) link */
    pmsg->peer = msg;
    msg->peer = pmsg;

    if (msg->pre_rsp_forward != NULL &&
        msg->pre_rsp_forward(ctx, s_conn, msg) != NC_OK) {
        return;
    }

    pmsg->done = 1;

    server = s_conn->owner;
    ASSERT(server!=NULL);
    sp = server->owner;
    ASSERT(sp!=NULL);
    if (sp->slowlog) {
        int64_t now = nc_msec_now();
        if (now < 0) {
            log_debug(LOG_WARN, "slowlog access end time failed!");
            now = 0;
        } else {
            pmsg->slowlog_etime = now;
            check_out_slowlog(ctx, sp, pmsg);
        }
    }

    
    msg->pre_coalesce(msg);

    c_conn = pmsg->owner;
    ASSERT(c_conn->client && !c_conn->proxy);

    if (req_done(c_conn, TAILQ_FIRST(&c_conn->omsg_q))) {
        status = event_add_out(ctx->evb, c_conn);
        if (status != NC_OK) {
            c_conn->err = errno;
        }
    }

    rsp_forward_stats(ctx, s_conn->owner, msg, msgsize);
}

void
rsp_recv_done(struct context *ctx, struct conn *conn, struct msg *msg,
              struct msg *nmsg)
{
    ASSERT(!conn->client && !conn->proxy);
    ASSERT(msg != NULL && conn->rmsg == msg);
    ASSERT(!msg->request);
    ASSERT(msg->owner == conn);
    ASSERT(nmsg == NULL || !nmsg->request);

    /* enqueue next message (response), if any */
    conn->rmsg = nmsg;

    if (rsp_filter(ctx, conn, msg)) {
        return;
    }

    rsp_forward(ctx, conn, msg);
}

struct msg *
rsp_send_next(struct context *ctx, struct conn *conn)
{
    rstatus_t status;
    struct msg *msg, *pmsg; /* response and it's peer request */

    ASSERT(conn->client && !conn->proxy);

    pmsg = TAILQ_FIRST(&conn->omsg_q);
    if (pmsg == NULL || !req_done(conn, pmsg)) {
        /* nothing is outstanding, initiate close? */
        if (pmsg == NULL && conn->eof) {
            conn->done = 1;
            log_debug(LOG_INFO, "c %d is done", conn->sd);
        }

        status = event_del_out(ctx->evb, conn);
        if (status != NC_OK) {
            conn->err = errno;
        }

        return NULL;
    }

    msg = conn->smsg;
    if (msg != NULL) {
        ASSERT(!msg->request && msg->peer != NULL);
        ASSERT(req_done(conn, msg->peer));
        pmsg = TAILQ_NEXT(msg->peer, c_tqe);
    }

    if (pmsg == NULL || !req_done(conn, pmsg)) {
        conn->smsg = NULL;
        return NULL;
    }
    ASSERT(pmsg->request && !pmsg->swallow);

    if (req_error(conn, pmsg)) {
        msg = rsp_make_error(ctx, conn, pmsg);
        if (msg == NULL) {
            conn->err = errno;
            return NULL;
        }
        msg->peer = pmsg;
        pmsg->peer = msg;
        stats_pool_incr(ctx, conn->owner, forward_error);
    } else {
        msg = pmsg->peer;
    }
    ASSERT(!msg->request);

    conn->smsg = msg;

    log_debug(LOG_VVERB, "send next rsp %"PRIu64" on c %d", msg->id, conn->sd);

    return msg;
}

void
rsp_send_done(struct context *ctx, struct conn *conn, struct msg *msg)
{
    struct msg *pmsg; /* peer message (request) */
    // struct server_pool *sp; 

    ASSERT(conn->client && !conn->proxy);
    ASSERT(conn->smsg == NULL);

    log_debug(LOG_VVERB, "send done rsp %"PRIu64" on c %d", msg->id, conn->sd);

    pmsg = msg->peer;

    ASSERT(!msg->request && pmsg->request);
    ASSERT(pmsg->peer == msg);
    ASSERT(pmsg->done && !pmsg->swallow);

    /* dequeue request from client outq */
    conn->dequeue_outq(ctx, conn, pmsg);

    req_put(pmsg);
}

#define MAX_TIMEOUT_MS  600000

static void
check_out_slowlog(struct context *ctx, struct server_pool *sp, struct msg *msg) {

    // max_cost_time 10min
    struct msg *pmsg; /* peer message (response) */
    struct conn *c_conn;
    struct conn *s_conn;
    struct server *server;
    int64_t cost_time;
    int client_fd;
    int server_fd;
    char *c_host;
    char *s_host;
    static char client_host[NI_MAXHOST + NI_MAXSERV];
    static char server_host[NI_MAXHOST + NI_MAXSERV];
    struct string *req_type;
    uint32_t req_len, rsp_len; 
    struct keypos *kpos;

    ASSERT(sp->slowlog);
    cost_time = msg->slowlog_etime - msg->slowlog_stime;

    pmsg = msg->peer;

    ASSERT(msg->done == 1);
    ASSERT(msg->request && !pmsg->request);

    c_conn = msg->owner;
    s_conn = pmsg->owner;
    if (s_conn) {
        server = s_conn->owner;
        if (server != NULL) {
            if (server->local_idc == 0) {
                // update cross stats
                switch (cost_time) {
                    case 501 ... MAX_TIMEOUT_MS:
                        stats_pool_incr(ctx, sp, xrequest_gt_500ms);
                    case 201 ... 500:
                        stats_pool_incr(ctx, sp, xrequest_gt_200ms);
                    case 101 ... 200:
                        stats_pool_incr(ctx, sp, xrequest_gt_100ms);
                    case 51 ... 100:
                        stats_pool_incr(ctx, sp, xrequest_gt_50ms);
                    case 21 ... 50:
                        stats_pool_incr(ctx, sp, xrequest_gt_20ms);
                    case 11 ... 20:
                        stats_pool_incr(ctx, sp, xrequest_gt_10ms);
                    default:
                        break;
                }
            } else{
                // update local stats
                switch (cost_time) {
                    case 501 ... MAX_TIMEOUT_MS:
                        stats_pool_incr(ctx, sp, lrequest_gt_500ms);
                    case 201 ... 500:
                        stats_pool_incr(ctx, sp, lrequest_gt_200ms);
                    case 101 ... 200:
                        stats_pool_incr(ctx, sp, lrequest_gt_100ms);
                    case 51 ... 100:
                        stats_pool_incr(ctx, sp, lrequest_gt_50ms);
                    case 21 ... 50:
                        stats_pool_incr(ctx, sp, lrequest_gt_20ms);
                    case 11 ... 20:
                        stats_pool_incr(ctx, sp, lrequest_gt_10ms);
                    default:
                        break;
                }
            }
        }
    }

    if (cost_time < sp-> slowlog_slower_than) {
        return;
    }

    client_fd = c_conn == NULL ? 0 : c_conn->sd;
    server_fd = s_conn == NULL ? 0 : s_conn->sd;

    c_host = nc_unresolve_peer_desc(client_fd);
    memcpy(client_host, c_host, NI_MAXHOST + NI_MAXSERV);
    s_host = nc_unresolve_peer_desc(server_fd);
    memcpy(server_host, s_host, NI_MAXHOST + NI_MAXSERV);

    req_type = msg_type_string(msg->type);
    req_len = msg->mlen;
    rsp_len = pmsg->mlen;
    kpos = array_get(msg->keys, 0);
    if (kpos->end != NULL) {
        *(kpos->end) = '\0';
    }
    log_slow("request_msg_id=%"PRIu64", client_address=%s, server_address=%s, cost_time=%"PRIu64"ms, fragment_id=%"PRIu64", request_type=%s, request_len %"PRIu32", response_len %"PRIu32", key='%s'",
        msg->id, client_host, server_host, cost_time ,msg->frag_id, req_type->data, req_len, rsp_len, kpos->start);  

}







