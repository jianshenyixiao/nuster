/*
 * nuster nosql filter related variables and functions.
 *
 * Copyright (C) Jiang Wenyuan, < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <common/cfgparse.h>
#include <common/standard.h>

#include <proto/filters.h>
#include <proto/log.h>
#include <proto/stream.h>
#include <proto/http_ana.h>
#include <proto/stream_interface.h>

#include <nuster/nuster.h>

static int
_nst_nosql_filter_init(hpx_proxy_t *px, hpx_flt_conf_t *fconf) {
    nst_flt_conf_t  *conf = fconf->conf;

    fconf->flags |= FLT_CFG_FL_HTX;
    conf->pid     = px->uuid;

    return 0;
}

static void
_nst_nosql_filter_deinit(hpx_proxy_t *px, hpx_flt_conf_t *fconf) {
    nst_flt_conf_t  *conf = fconf->conf;

    if(conf) {
        free(conf);
    }

    fconf->conf = NULL;
}

static int
_nst_nosql_filter_check(hpx_proxy_t *px, hpx_flt_conf_t *fconf) {

    if(px->mode != PR_MODE_HTTP) {
        ha_warning("Proxy [%s]: mode should be http to enable nosql\n", px->id);
    }

    return 0;
}

static int
_nst_nosql_filter_attach(hpx_stream_t *s, hpx_filter_t *filter) {
    nst_flt_conf_t  *conf = FLT_CONF(filter);

    if(global.nuster.nosql.status != NST_STATUS_ON || conf->status != NST_STATUS_ON) {
        return 0;
    }

    if(!filter->ctx) {
        nst_ctx_t  *ctx;
        int         rule_cnt, key_cnt, size;

        rule_cnt = nuster.proxy[conf->pid]->rule_cnt;
        key_cnt  = nuster.proxy[conf->pid]->key_cnt;

        size = sizeof(nst_ctx_t) + key_cnt * sizeof(nst_key_t);

        ctx = malloc(size);

        if(ctx == NULL) {
            return 0;
        }

        memset(ctx, 0, size);

        ctx->state    = NST_CTX_STATE_INIT;
        ctx->pid      = conf->pid;
        ctx->rule_cnt = rule_cnt;
        ctx->key_cnt  = key_cnt;

        if(nst_http_txn_attach(&ctx->txn) != NST_OK) {
            free(ctx);

            return 0;
        }

        filter->ctx = ctx;
    }

    register_data_filter(s, &s->req, filter);
    //register_data_filter(s, &s->res, filter);
    return 1;
}

static void
_nst_nosql_filter_detach(hpx_stream_t *s, hpx_filter_t *filter) {

    if(filter->ctx) {
        nst_ctx_t  *ctx = filter->ctx;

        nst_stats_update_nosql(ctx->state);

        if(ctx->state == NST_CTX_STATE_CREATE) {
            nst_nosql_abort(ctx);
        }

        nst_http_txn_detach(&ctx->txn);

        free(ctx);
    }
}

static int
_nst_nosql_filter_http_headers(hpx_stream_t *s, hpx_filter_t *filter, hpx_http_msg_t *msg) {

    hpx_stream_interface_t  *si     = &s->si[1];
    nst_ctx_t               *ctx    = filter->ctx;
    hpx_proxy_t             *px     = s->be;
    hpx_appctx_t            *appctx = si_appctx(si);
    hpx_channel_t           *req    = msg->chn;
    hpx_channel_t           *res    = &s->res;

    if((msg->chn->flags & CF_ISRESP)) {
        return 1;
    }

    if(s->txn->meth != HTTP_METH_GET &&
            s->txn->meth != HTTP_METH_POST &&
            s->txn->meth != HTTP_METH_DELETE) {

        appctx->st0 = NST_NOSQL_APPCTX_STATE_NOT_ALLOWED;

        return 1;
    }

    if(ctx->state == NST_CTX_STATE_INIT) {
        int  i = 0;

        if(nst_http_parse_htx(s, msg, &ctx->txn) != NST_OK) {
            appctx->st0 = NST_NOSQL_APPCTX_STATE_ERROR;

            return 1;
        }

        ctx->rule = nuster.proxy[px->uuid]->rule;

        for(i = 0; i < ctx->rule_cnt; i++) {
            int         idx = ctx->rule->key->idx;
            nst_key_t  *key = &(ctx->keys[idx]);

            nst_debug(s, "[nosql] ==== Check rule: %s ====", ctx->rule->name);

            if(!key->data) {
                /* build key */
                if(nst_key_build(s, msg, ctx->rule, &ctx->txn, key, HTTP_METH_GET) != NST_OK) {
                    ctx->state = NST_NOSQL_APPCTX_STATE_ERROR;

                    return 1;
                }
            }

            nst_key_hash(key);

            nst_key_debug(s, key);

            if(s->txn->meth == HTTP_METH_GET) {
                nst_debug_beg(s, "[nosql] Check key existence: ");

                ctx->state = nst_nosql_exists(ctx);

                if(ctx->state == NST_CTX_STATE_HIT_MEMORY || ctx->state == NST_CTX_STATE_HIT_DISK) {
                    /* OK, nosql exists */

                    nst_debug_end("HIT");

                    break;
                }

                nst_debug_end("MISS");
            } else if(s->txn->meth == HTTP_METH_POST) {
                nst_debug_beg(s, "[nosql] Test rule ACL: ");

                if(nst_test_rule(s, ctx->rule, msg->chn->flags & CF_ISRESP) == NST_OK) {

                    nst_debug_end("PASS");
                    nst_debug(s, "[nosql] To create");

                    if(nst_nosql_get_headers(s, msg, ctx)) {
                        ctx->state = NST_CTX_STATE_PASS;
                        ctx->pid   = px->uuid;
                    } else {
                        ctx->state = NST_CTX_STATE_INVALID;
                    }

                    break;
                }

                nst_debug_end("FAIL");
            } else if(s->txn->meth == HTTP_METH_DELETE) {

                if(nst_nosql_delete(key)) {
                    nst_debug(s, "[nosql] EXIST, to delete");
                    ctx->state = NST_CTX_STATE_DELETE;

                    break;
                }

                nst_debug(s, "[nosql] NOT EXIST");
            }

            ctx->rule = ctx->rule->next;
        }
    }

    /* ctx->state should have been changed in previous stage,
     * if not, either the key does not exist for GET/DELETE
     * or all rules do not pass for POST request
     * */
    if(ctx->state == NST_CTX_STATE_INIT) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_NOT_FOUND;

        return 1;
    }

    if(ctx->state == NST_CTX_STATE_HIT_MEMORY) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_HIT_MEMORY;
        /* 0: header unsent, 1: sent */
        appctx->st1 = 0;

        appctx->ctx.nuster.nosql.store.ring.data = ctx->store.ring.data;
        appctx->ctx.nuster.nosql.store.ring.item = ctx->store.ring.data->item;

        req->analysers &= ~AN_REQ_FLT_HTTP_HDRS;
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;

        req->analysers |= AN_REQ_FLT_END;
        req->analyse_exp = TICK_ETERNITY;

        res->flags |= CF_NEVER_WAIT;
    }

    if(ctx->state == NST_CTX_STATE_HIT_DISK) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_HIT_DISK;
        appctx->st1 = NST_DISK_APPLET_HEADER;

        appctx->ctx.nuster.nosql.fd         = ctx->store.disk.fd;
        appctx->ctx.nuster.nosql.offset     = nst_disk_get_header_pos(ctx->store.disk.meta);
        appctx->ctx.nuster.nosql.header_len = nst_disk_meta_get_header_len(ctx->store.disk.meta);


        req->analysers &= ~AN_REQ_FLT_HTTP_HDRS;
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;

        req->analysers |= AN_REQ_FLT_END;
        req->analyse_exp = TICK_ETERNITY;

        res->flags |= CF_NEVER_WAIT;
    }

    if(ctx->state == NST_CTX_STATE_PASS) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_CREATE;
        nst_nosql_create(s, msg, ctx);
    }

    if(ctx->state == NST_CTX_STATE_WAIT) {
        ctx->state  = NST_CTX_STATE_PASS;
        appctx->st0 = NST_NOSQL_APPCTX_STATE_WAIT;

        return 0;
    }

    if(ctx->state == NST_CTX_STATE_INVALID && ctx->store.disk.file == NULL) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_ERROR;
    }

    if(ctx->state == NST_CTX_STATE_DELETE) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_END;
    }

    if(ctx->state == NST_CTX_STATE_FULL) {
        appctx->st0 = NST_NOSQL_APPCTX_STATE_FULL;
    }

    return 1;
}

static int
_nst_nosql_filter_http_payload(hpx_stream_t *s, hpx_filter_t *filter, hpx_http_msg_t *msg,
        unsigned int offset, unsigned int len) {

    hpx_stream_interface_t  *si     = &s->si[1];
    hpx_appctx_t            *appctx = si_appctx(si);
    nst_ctx_t               *ctx    = filter->ctx;

    if(len <= 0) {
        return 0;
    }

    if(ctx->state == NST_CTX_STATE_CREATE && !(msg->chn->flags & CF_ISRESP)) {

        if(nst_nosql_update(msg, ctx, offset, len) != NST_OK) {
            ctx->entry->state = NST_DICT_ENTRY_STATE_INVALID;
            appctx->st0       = NST_NOSQL_APPCTX_STATE_FULL;
            ctx->state        = NST_CTX_STATE_INVALID;
        }
    }

    return len;
}

static int
_nst_nosql_filter_http_end(hpx_stream_t *s, hpx_filter_t *filter, hpx_http_msg_t *msg) {
    hpx_stream_interface_t  *si     = &s->si[1];
    hpx_appctx_t            *appctx = si_appctx(si);
    nst_ctx_t               *ctx    = filter->ctx;

    if(ctx->state == NST_CTX_STATE_CREATE && !(msg->chn->flags & CF_ISRESP)) {

        nst_nosql_finish(s, msg, ctx);

        if(ctx->state == NST_CTX_STATE_DONE) {
            nst_debug(s, "[nosql] Created");
            appctx->st0 = NST_NOSQL_APPCTX_STATE_END;
        } else {
            appctx->st0 = NST_NOSQL_APPCTX_STATE_EMPTY;
        }

    }

    return 1;
}

hpx_flt_ops_t nst_nosql_filter_ops = {
    /* Manage nosql filter, called for each filter declaration */
    .init   = _nst_nosql_filter_init,
    .deinit = _nst_nosql_filter_deinit,
    .check  = _nst_nosql_filter_check,

    .attach = _nst_nosql_filter_attach,
    .detach = _nst_nosql_filter_detach,

    /* Filter HTTP requests and responses */
    .http_headers      = _nst_nosql_filter_http_headers,
    .http_payload      = _nst_nosql_filter_http_payload,
    .http_end          = _nst_nosql_filter_http_end,

};
