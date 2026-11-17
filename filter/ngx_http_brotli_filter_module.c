/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 * Copyright (C) 2026 Thijs Eilander
 * Copyright (C) Hanada
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <brotli/encode.h>

#if (NGX_EXPR)
#include <ngx_http_expr_module.h>
#endif


#define NGX_HTTP_BROTLI_PROCESS        0
#define NGX_HTTP_BROTLI_FLUSH          1
#define NGX_HTTP_BROTLI_FINISH         2
#define NGX_HTTP_BROTLI_INPUT_SIZE     (64 * 1024)
#define NGX_HTTP_BROTLI_INPUT_LIMIT    (256 * 1024)
#define NGX_HTTP_BROTLI_CALLS          64
#define NGX_HTTP_BROTLI_MULTI_STATUS   207


typedef struct {
#if (NGX_EXPR)
    ngx_array_t  *enable;
    ngx_array_t  *level;
    ngx_array_t  *min_length;
    ngx_array_t  *max_length;
#else
    ngx_flag_t    enable;
    ngx_int_t     level;
    size_t        min_length;
    size_t        max_length;
#endif
    ngx_hash_t    types;
    ngx_array_t  *types_keys;
    ngx_array_t  *bypass;
    ngx_bufs_t    bufs;
    ngx_flag_t    no_buffer;
    size_t        postpone;
    size_t        window;
} ngx_http_brotli_conf_t;


typedef struct {
    ngx_http_request_t      *request;
    ngx_http_brotli_conf_t  *conf;
    ngx_event_t             *event;
    ngx_chain_t             *in;
    ngx_chain_t            **last_in;
    ngx_chain_t             *out;
    ngx_chain_t            **last_out;
    ngx_chain_t             *free;
    ngx_chain_t             *busy;
    ngx_buf_t               *in_buf;
    ngx_buf_t               *out_buf;
    BrotliEncoderState      *state;
    off_t                    content_length;
    off_t                    bytes_in;
    off_t                    bytes_out;
    size_t                   input;
    size_t                   limit;
    size_t                   postponed;
    ngx_int_t                level;
    ngx_uint_t               bufs;
    ngx_uint_t               calls;
    ngx_uint_t               operation;
    unsigned                 initialized:1;
    unsigned                 buffering:1;
    unsigned                 done:1;
    unsigned                 success:1;
} ngx_http_brotli_ctx_t;


static ngx_int_t ngx_http_brotli_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_brotli_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_brotli_queue(ngx_http_brotli_ctx_t *ctx,
    ngx_chain_t *in);
static void ngx_http_brotli_release_input(ngx_http_brotli_ctx_t *ctx);
static ngx_int_t ngx_http_brotli_get_buffer(ngx_http_brotli_ctx_t *ctx);
static ngx_int_t ngx_http_brotli_output(ngx_http_brotli_ctx_t *ctx);
static ngx_int_t ngx_http_brotli_post(ngx_http_brotli_ctx_t *ctx);
static void ngx_http_brotli_handler(ngx_event_t *ev);
static void ngx_http_brotli_cleanup(void *data);
static ngx_int_t ngx_http_brotli_init(ngx_http_brotli_ctx_t *ctx);
static ngx_int_t ngx_http_brotli_process(ngx_http_brotli_ctx_t *ctx);
static void ngx_http_brotli_close(ngx_http_brotli_ctx_t *ctx);
static void *ngx_http_brotli_alloc(void *opaque, size_t size);
static void ngx_http_brotli_free(void *opaque, void *address);
static ngx_int_t ngx_http_brotli_ok(ngx_http_request_t *r);
static ngx_int_t ngx_http_brotli_accept_encoding(ngx_str_t *ae);
static ngx_uint_t ngx_http_brotli_quantity(u_char *p, u_char *last);
static ngx_int_t ngx_http_brotli_cache_control(ngx_http_request_t *r);
static ngx_int_t ngx_http_brotli_policy(ngx_http_request_t *r);
static char *ngx_http_brotli_alias(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_brotli_ratio_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_brotli_add_variables(ngx_conf_t *cf);
static char *ngx_http_brotli_level(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_brotli_size(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static void *ngx_http_brotli_create_conf(ngx_conf_t *cf);
static char *ngx_http_brotli_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_brotli_filter_init(ngx_conf_t *cf);


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;
static u_char                           ngx_http_brotli_empty;
static ngx_buf_t                        ngx_http_brotli_empty_buf;
static ngx_str_t  ngx_http_brotli_ratio = ngx_string("brotli_ratio");

static ngx_str_t  ngx_http_brotli_aliases[] = {
    ngx_string("gzip_http_version"),
    ngx_string("gzip_proxied"),
    ngx_string("gzip_disable")
};


static ngx_command_t  ngx_http_brotli_commands[] = {

    { ngx_string("brotli"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
#if (NGX_EXPR)
      ngx_conf_set_conditional_flag_slot,
#else
      ngx_conf_set_flag_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, enable),
      NULL },

    { ngx_string("brotli_types"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_types_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, types_keys),
      &ngx_http_html_default_types[0] },

    { ngx_string("brotli_min_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
#if (NGX_EXPR)
      ngx_conf_set_conditional_size_slot,
#else
      ngx_conf_set_size_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, min_length),
      NULL },

    { ngx_string("brotli_max_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
#if (NGX_EXPR)
      ngx_conf_set_conditional_size_slot,
#else
      ngx_conf_set_size_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, max_length),
      NULL },

    { ngx_string("brotli_bypass"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_set_predicate_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, bypass),
      NULL },

    { ngx_string("brotli_buffers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, bufs),
      NULL },

    { ngx_string("brotli_postpone"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, postpone),
      NULL },

    { ngx_string("brotli_no_buffer"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, no_buffer),
      NULL },

    { ngx_string("brotli_http_version"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_brotli_alias,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      &ngx_http_brotli_aliases[0] },

    { ngx_string("brotli_proxied"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_brotli_alias,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      &ngx_http_brotli_aliases[1] },

    { ngx_string("brotli_disable"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_brotli_alias,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      &ngx_http_brotli_aliases[2] },

    { ngx_string("brotli_comp_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
      ngx_http_brotli_level,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, level),
      NULL },

    { ngx_string("brotli_window"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_brotli_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_brotli_conf_t, window),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_brotli_module_ctx = {
    ngx_http_brotli_add_variables, /* preconfiguration */
    ngx_http_brotli_filter_init, /* postconfiguration */

    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_brotli_create_conf, /* create location configuration */
    ngx_http_brotli_merge_conf /* merge location configuration */
};


ngx_module_t  ngx_http_brotli_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_brotli_module_ctx, /* module context */
    ngx_http_brotli_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    NULL, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_brotli_header_filter(ngx_http_request_t *r)
{
    size_t                   min, max;
    ngx_int_t                rc;
    ngx_flag_t               enabled;
    ngx_table_elt_t         *h;
    ngx_pool_cleanup_t      *cln;
    ngx_http_brotli_ctx_t   *ctx;
    ngx_http_brotli_conf_t  *conf;

    if (r != r->main || r->header_only || r->method == NGX_HTTP_HEAD
        || (r->headers_out.status != NGX_HTTP_OK
            && r->headers_out.status != NGX_HTTP_FORBIDDEN
            && r->headers_out.status != NGX_HTTP_NOT_FOUND
            && r->headers_out.status != NGX_HTTP_BROTLI_MULTI_STATUS)
        || (r->headers_out.content_encoding
            && r->headers_out.content_encoding->value.len))
    {
        return ngx_http_next_header_filter(r);
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_brotli_filter_module);

#if (NGX_EXPR)
    enabled = ngx_http_get_expr_flag_value(r, conf->enable);
#else
    enabled = conf->enable;
#endif

    if (!enabled) {
        return ngx_http_next_header_filter(r);
    }

#if (NGX_EXPR)
    min = ngx_http_get_expr_size_value(r, conf->min_length);
    max = ngx_http_get_expr_size_value(r, conf->max_length);
#else
    min = conf->min_length;
    max = conf->max_length;
#endif

    if ((r->headers_out.content_length_n != -1
         && (r->headers_out.content_length_n < (off_t) min
             || (max && r->headers_out.content_length_n > (off_t) max)))
        || ngx_http_test_content_type(r, &conf->types) == NULL)
    {
        return ngx_http_next_header_filter(r);
    }

    rc = ngx_http_test_predicates(r, conf->bypass);
    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc == NGX_DECLINED || ngx_http_brotli_policy(r) != NGX_OK) {
        return ngx_http_next_header_filter(r);
    }

    r->gzip_vary = 1;

    if (ngx_http_brotli_ok(r) != NGX_OK) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_brotli_ctx_t));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    ctx->request = r;
    ctx->conf = conf;
    ctx->content_length = r->headers_out.content_length_n;
    ctx->buffering = (conf->postpone != 0);
    ctx->last_in = &ctx->in;
    ctx->last_out = &ctx->out;
    ctx->limit = NGX_HTTP_BROTLI_INPUT_LIMIT;
    ctx->calls = NGX_HTTP_BROTLI_CALLS;

#if (NGX_EXPR)
    ctx->level = ngx_http_get_expr_num_value(r, conf->level);
#else
    ctx->level = conf->level;
#endif

    cln->handler = ngx_http_brotli_cleanup;
    cln->data = ctx;
    ngx_http_set_ctx(r, ctx, ngx_http_brotli_filter_module);

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    h->hash = 1;
    h->next = NULL;
    ngx_str_set(&h->key, "Content-Encoding");
    ngx_str_set(&h->value, "br");
    r->headers_out.content_encoding = h;
    r->main_filter_need_in_memory = 1;

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_weak_etag(r);

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_brotli_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    off_t                   before_in, before_out;
    ngx_int_t               rc;
    ngx_uint_t              blocked, yield;
    ngx_chain_t            *cl;
    ngx_http_brotli_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_brotli_filter_module);

    if (ctx == NULL || ctx->done || r->header_only) {
        return ngx_http_next_body_filter(r, in);
    }

    if (in) {
        r->connection->buffered |= NGX_HTTP_GZIP_BUFFERED;
    }

    if (ctx->buffering || (ctx->event && ctx->event->posted)) {
        if (ngx_http_brotli_queue(ctx, in) != NGX_OK) {
            goto failed;
        }

        in = NULL;
    }

    if (ctx->event && ctx->event->posted) {
        return NGX_AGAIN;
    }

    if (ctx->buffering) {
        return NGX_OK;
    }

    for ( ;; ) {
        blocked = 0;
        yield = 0;

        for ( ;; ) {
            if (ctx->in_buf == NULL) {
                if (ctx->in) {
                    cl = ctx->in;
                    ctx->in = cl->next;
                    ctx->in_buf = cl->buf;
                    ngx_free_chain(r->pool, cl);

                    if (ctx->in == NULL) {
                        ctx->last_in = &ctx->in;
                    }

                } else if (in) {
                    ctx->in_buf = in->buf;
                    in = in->next;

                } else {
                    break;
                }

                if (ngx_buf_size(ctx->in_buf)
                    && !ngx_buf_in_memory(ctx->in_buf))
                {
                    ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                                  "brotli filter received a file buffer");
                    goto failed;
                }

                if (ctx->in_buf->last_buf) {
                    ctx->operation = NGX_HTTP_BROTLI_FINISH;

                } else if (ctx->in_buf->flush) {
                    ctx->operation = NGX_HTTP_BROTLI_FLUSH;

                } else {
                    ctx->operation = NGX_HTTP_BROTLI_PROCESS;
                }

                if (ngx_buf_size(ctx->in_buf) == 0
                    && ctx->operation == NGX_HTTP_BROTLI_PROCESS)
                {
                    ngx_http_brotli_release_input(ctx);
                    continue;
                }
            }

            if (ctx->limit == 0 || ctx->calls == 0) {
                yield = 1;
                break;
            }

            if (!ctx->initialized) {
                if (ngx_http_brotli_init(ctx) != NGX_OK) {
                    goto failed;
                }

                ctx->initialized = 1;
            }

            rc = ngx_http_brotli_get_buffer(ctx);

            if (rc == NGX_ERROR) {
                goto failed;
            }

            if (rc == NGX_AGAIN) {
                blocked = 1;
                break;
            }

            ctx->input = ngx_min((size_t) ngx_buf_size(ctx->in_buf),
                                 ctx->limit);
            ctx->input = ngx_min(ctx->input, NGX_HTTP_BROTLI_INPUT_SIZE);

            before_in = ctx->bytes_in;
            before_out = ctx->bytes_out;

            rc = ngx_http_brotli_process(ctx);

            ctx->limit -= (size_t) (ctx->bytes_in - before_in);
            ctx->calls--;

            if (rc == NGX_ERROR) {
                goto failed;
            }

            if (rc == NGX_OK
                && ctx->operation != NGX_HTTP_BROTLI_PROCESS)
            {
                if (ctx->operation == NGX_HTTP_BROTLI_FINISH) {
                    ctx->out_buf->last_buf = 1;
                    ctx->done = 1;
                    ctx->success = 1;

                    ngx_http_brotli_cleanup(ctx);
                    r->connection->buffered &= ~NGX_HTTP_GZIP_BUFFERED;

                } else {
                    ctx->out_buf->flush = 1;

                    if (ctx->in == NULL && in == NULL) {
                        r->connection->buffered &= ~NGX_HTTP_GZIP_BUFFERED;
                    }
                }

                if (ngx_http_brotli_output(ctx) != NGX_OK) {
                    goto failed;
                }

            } else if (ctx->out_buf->last == ctx->out_buf->end) {
                if (ngx_http_brotli_output(ctx) != NGX_OK) {
                    goto failed;
                }
            }

            if (rc == NGX_OK) {
                ngx_http_brotli_release_input(ctx);

            } else if (before_in == ctx->bytes_in
                       && before_out == ctx->bytes_out)
            {
                ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                              "brotli encoder made no progress");
                goto failed;
            }

            if (rc == NGX_AGAIN && ngx_buf_size(ctx->in_buf) == 0) {
                /* The caller may reuse the buffer before we resume. */

                ngx_http_brotli_release_input(ctx);
                ctx->in_buf = &ngx_http_brotli_empty_buf;
            }

            if (ctx->done) {
                break;
            }
        }

        if (ctx->out_buf && ctx->out_buf->last != ctx->out_buf->pos
            && (yield
                || (ctx->conf->no_buffer
                    && ctx->in_buf == NULL && ctx->in == NULL && in == NULL)))
        {
            if (ngx_http_brotli_output(ctx) != NGX_OK) {
                goto failed;
            }
        }

        rc = NGX_OK;

        if (ctx->out || ctx->busy) {
            rc = ngx_http_next_body_filter(r, ctx->out);

            if (rc == NGX_ERROR) {
                goto failed;
            }

            ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &ctx->out,
                           (ngx_buf_tag_t) &ngx_http_brotli_filter_module);
            ctx->last_out = &ctx->out;
        }

        if (ctx->done) {
            return rc;
        }

        if (yield) {
            if (ngx_http_brotli_queue(ctx, in) != NGX_OK) {
                goto failed;
            }

            if (ngx_http_brotli_post(ctx) == NGX_ERROR) {
                goto failed;
            }

            return NGX_AGAIN;
        }

        if (!blocked) {
            return rc;
        }

        if (ctx->free == NULL) {
            if (ngx_http_brotli_queue(ctx, in) != NGX_OK) {
                goto failed;
            }

            return NGX_AGAIN;
        }
    }

failed:

    ctx->success = 0;
    ctx->done = 1;
    ngx_http_brotli_cleanup(ctx);
    r->connection->buffered &= ~NGX_HTTP_GZIP_BUFFERED;

    return NGX_ERROR;
}


static ngx_int_t
ngx_http_brotli_queue(ngx_http_brotli_ctx_t *ctx, ngx_chain_t *in)
{
    size_t        size;
    ngx_buf_t    *b;
    ngx_pool_t   *pool;
    ngx_chain_t  *cl;

    pool = ctx->request->pool;

    for ( /* void */ ; in; in = in->next) {
        b = in->buf;

        if (ngx_buf_size(b) && !ngx_buf_in_memory(b)) {
            ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                          "brotli filter received a file buffer");
            return NGX_ERROR;
        }

        size = ngx_buf_size(b);

        if (ctx->buffering) {
            if (b->flush || b->last_buf
                || size >= ctx->conf->postpone - ctx->postponed)
            {
                ctx->buffering = 0;

            } else {
                ctx->postponed += size;

                if (size) {
                    b = ngx_create_temp_buf(pool, size);
                    if (b == NULL) {
                        return NGX_ERROR;
                    }

                    b->last = ngx_cpymem(b->pos, in->buf->pos, size);
                    b->tag =
                        (ngx_buf_tag_t) &ngx_http_brotli_filter_module;
                    in->buf->pos = in->buf->last;
                }
            }
        }

        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = b;
        cl->next = NULL;
        *ctx->last_in = cl;
        ctx->last_in = &cl->next;
    }

    return NGX_OK;
}


static void
ngx_http_brotli_release_input(ngx_http_brotli_ctx_t *ctx)
{
    if (ctx->in_buf->tag
        == (ngx_buf_tag_t) &ngx_http_brotli_filter_module
        && ctx->in_buf->start != NULL)
    {
        ngx_pfree(ctx->request->pool, ctx->in_buf->start);
    }

    ctx->in_buf = NULL;
}


static ngx_int_t
ngx_http_brotli_get_buffer(ngx_http_brotli_ctx_t *ctx)
{
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    if (ctx->out_buf) {
        return NGX_OK;
    }

    if (ctx->free) {
        cl = ctx->free;
        ctx->free = cl->next;
        b = cl->buf;
        ngx_free_chain(ctx->request->pool, cl);

    } else if (ctx->bufs < (ngx_uint_t) ctx->conf->bufs.num) {
        b = ngx_create_temp_buf(ctx->request->pool, ctx->conf->bufs.size);
        if (b == NULL) {
            return NGX_ERROR;
        }

        ctx->bufs++;

    } else {
        return NGX_AGAIN;
    }

    b->pos = b->start;
    b->last = b->start;
    b->tag = (ngx_buf_tag_t) &ngx_http_brotli_filter_module;
    b->temporary = 1;
    b->recycled = 1;
    b->flush = 0;
    b->sync = 0;
    b->last_buf = 0;
    b->last_in_chain = 0;

    ctx->out_buf = b;

    return NGX_OK;
}


static ngx_int_t
ngx_http_brotli_output(ngx_http_brotli_ctx_t *ctx)
{
    ngx_chain_t  *cl;

    cl = ngx_alloc_chain_link(ctx->request->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    if (ctx->out_buf->pos == ctx->out_buf->last) {
        ctx->out_buf->temporary = 0;
        ctx->out_buf->sync = 1;
    }

    cl->buf = ctx->out_buf;
    cl->next = NULL;
    *ctx->last_out = cl;
    ctx->last_out = &cl->next;
    ctx->out_buf = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_http_brotli_post(ngx_http_brotli_ctx_t *ctx)
{
    ngx_http_request_t  *r;

    r = ctx->request;

    if (ctx->event == NULL) {
        ctx->event = ngx_pcalloc(r->pool, sizeof(ngx_event_t));
        if (ctx->event == NULL) {
            return NGX_ERROR;
        }

        ctx->event->data = ctx;
        ctx->event->handler = ngx_http_brotli_handler;
        ctx->event->log = r->connection->log;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "brotli filter yield");

    r->connection->buffered |= NGX_HTTP_GZIP_BUFFERED;
    ngx_post_event(ctx->event, &ngx_posted_next_events);

    return NGX_AGAIN;
}


static void
ngx_http_brotli_handler(ngx_event_t *ev)
{
    ngx_connection_t       *c;
    ngx_http_request_t     *r;
    ngx_http_brotli_ctx_t  *ctx;

    ctx = ev->data;
    r = ctx->request;
    c = r->connection;

    ctx->limit = NGX_HTTP_BROTLI_INPUT_LIMIT;
    ctx->calls = NGX_HTTP_BROTLI_CALLS;

    ngx_http_set_log_request(c->log, r);

    r->write_event_handler(r);
    ngx_http_run_posted_requests(c);
}


static void
ngx_http_brotli_cleanup(void *data)
{
    ngx_http_brotli_ctx_t  *ctx = data;

    if (ctx->event && ctx->event->posted) {
        ngx_delete_posted_event(ctx->event);
    }

    ngx_http_brotli_close(ctx);
    ctx->initialized = 0;
}


static ngx_int_t
ngx_http_brotli_init(ngx_http_brotli_ctx_t *ctx)
{
    size_t  window;

    window = ctx->conf->window;

    if (ctx->content_length >= 0) {
        window = BROTLI_MIN_WINDOW_BITS;

        while (window < ctx->conf->window
               && ctx->content_length > ((off_t) 1 << window))
        {
            window++;
        }
    }

    ctx->state =
        BrotliEncoderCreateInstance(ngx_http_brotli_alloc,
                                    ngx_http_brotli_free,
                                    ctx->request->connection->log);
    if (ctx->state == NULL) {
        return NGX_ERROR;
    }

    if (!BrotliEncoderSetParameter(ctx->state, BROTLI_PARAM_QUALITY,
                                   (uint32_t) ctx->level)
        || !BrotliEncoderSetParameter(ctx->state, BROTLI_PARAM_LGWIN,
                                      (uint32_t) window))
    {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "BrotliEncoderSetParameter() failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_brotli_process(ngx_http_brotli_ctx_t *ctx)
{
    size_t                  in, out, available_in, available_out;
    u_char                 *next_out;
    const u_char           *next_in;
    BrotliEncoderOperation  operation;

    in = ctx->input;
    out = ctx->out_buf->end - ctx->out_buf->last;
    available_in = in;
    available_out = out;
    next_in = in ? ctx->in_buf->pos : &ngx_http_brotli_empty;
    next_out = ctx->out_buf->last;

    if ((off_t) in < ngx_buf_size(ctx->in_buf)) {
        operation = BROTLI_OPERATION_PROCESS;

    } else if (ctx->operation == NGX_HTTP_BROTLI_FINISH) {
        operation = BROTLI_OPERATION_FINISH;

    } else if (ctx->operation == NGX_HTTP_BROTLI_FLUSH) {
        operation = BROTLI_OPERATION_FLUSH;

    } else {
        operation = BROTLI_OPERATION_PROCESS;
    }

    if (!BrotliEncoderCompressStream(ctx->state, operation,
                                     &available_in, &next_in,
                                     &available_out, &next_out, NULL))
    {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "BrotliEncoderCompressStream() failed");
        return NGX_ERROR;
    }

    if (in != available_in) {
        ctx->in_buf->pos += in - available_in;
    }

    ctx->out_buf->last = next_out;
    ctx->bytes_in += in - available_in;
    ctx->bytes_out += out - available_out;

    if (ctx->operation == NGX_HTTP_BROTLI_FINISH) {
        return BrotliEncoderIsFinished(ctx->state) ? NGX_OK : NGX_AGAIN;
    }

    return (ngx_buf_size(ctx->in_buf) == 0
            && !BrotliEncoderHasMoreOutput(ctx->state))
           ? NGX_OK : NGX_AGAIN;
}


static void
ngx_http_brotli_close(ngx_http_brotli_ctx_t *ctx)
{
    if (ctx->state) {
        BrotliEncoderDestroyInstance(ctx->state);
        ctx->state = NULL;
    }
}


static void *
ngx_http_brotli_alloc(void *opaque, size_t size)
{
    return ngx_alloc(size, opaque);
}


static void
ngx_http_brotli_free(void *opaque, void *address)
{
    ngx_free(address);
}


static ngx_int_t
ngx_http_brotli_ok(ngx_http_request_t *r)
{
    ngx_table_elt_t  *ae;

    if (r != r->main) {
        return NGX_DECLINED;
    }

    ae = r->headers_in.accept_encoding;
    if (ae == NULL) {
        return NGX_DECLINED;
    }

    if (ae->value.len < sizeof("br") - 1) {
        return NGX_DECLINED;
    }

    if (ngx_memcmp(ae->value.data, "br", 2) != 0
        && ngx_http_brotli_accept_encoding(&ae->value) != NGX_OK)
    {
        return NGX_DECLINED;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_brotli_accept_encoding(ngx_str_t *ae)
{
    u_char  *p, *start, *last;

    start = ae->data;
    last = start + ae->len;

    for ( ;; ) {
        p = ngx_strcasestrn(start, "br", 2 - 1);
        if (p == NULL) {
            return NGX_DECLINED;
        }

        if (p == start || (*(p - 1) == ',' || *(p - 1) == ' ')) {
            break;
        }

        start = p + 2;
    }

    p += 2;

    while (p < last) {
        switch (*p++) {
        case ',':
            return NGX_OK;
        case ';':
            goto quantity;
        case ' ':
            continue;
        default:
            return NGX_DECLINED;
        }
    }

    return NGX_OK;

quantity:

    while (p < last) {
        switch (*p++) {
        case 'q':
        case 'Q':
            goto equal;
        case ' ':
            continue;
        default:
            return NGX_DECLINED;
        }
    }

    return NGX_OK;

equal:

    if (p + 2 > last || *p++ != '=') {
        return NGX_DECLINED;
    }

    if (ngx_http_brotli_quantity(p, last) == 0) {
        return NGX_DECLINED;
    }

    return NGX_OK;
}


static ngx_uint_t
ngx_http_brotli_quantity(u_char *p, u_char *last)
{
    u_char      c;
    ngx_uint_t  n, q;

    c = *p++;

    if (c != '0' && c != '1') {
        return 0;
    }

    q = (c - '0') * 100;

    if (p == last) {
        return q;
    }

    c = *p++;

    if (c == ',' || c == ' ') {
        return q;
    }

    if (c != '.') {
        return 0;
    }

    n = 0;

    while (p < last) {
        c = *p++;

        if (c == ',' || c == ' ') {
            break;
        }

        if (c >= '0' && c <= '9') {
            q += c - '0';
            n++;
            continue;
        }

        return 0;
    }

    if (q > 100 || n > 3) {
        return 0;
    }

    return q;
}


static ngx_int_t
ngx_http_brotli_cache_control(ngx_http_request_t *r)
{
    u_char           *p, *last, *start;
    ngx_uint_t        i, quoted;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;

    part = &r->headers_out.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].hash == 0
            || h[i].key.len != sizeof("Cache-Control") - 1
            || ngx_strncasecmp(h[i].key.data, (u_char *) "Cache-Control",
                               sizeof("Cache-Control") - 1) != 0)
        {
            continue;
        }

        p = h[i].value.data;
        last = p + h[i].value.len;

        while (p < last) {
            while (p < last && (*p == ' ' || *p == '\t' || *p == ',')) {
                p++;
            }

            start = p;

            while (p < last && *p != ' ' && *p != '\t'
                   && *p != ',' && *p != '=')
            {
                p++;
            }

            if (p - start == sizeof("no-transform") - 1
                && ngx_strncasecmp(start, (u_char *) "no-transform",
                                   sizeof("no-transform") - 1) == 0)
            {
                return NGX_DECLINED;
            }

            quoted = 0;

            while (p < last) {
                if (*p == ',' && !quoted) {
                    p++;
                    break;
                }

                if (*p == '"') {
                    quoted = !quoted;

                } else if (*p == '\\' && quoted && p + 1 < last) {
                    p++;
                }

                p++;
            }
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_brotli_policy(ngx_http_request_t *r)
{
    time_t                     date, expires;
    ngx_uint_t                 p;
    ngx_table_elt_t           *e, *d, *cc;
    ngx_http_core_loc_conf_t  *clcf;

    static ngx_str_t  no_cache = ngx_string("no-cache");
    static ngx_str_t  no_store = ngx_string("no-store");
    static ngx_str_t  private = ngx_string("private");

    if (ngx_http_brotli_cache_control(r) != NGX_OK) {
        return NGX_DECLINED;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    if (r->http_version < clcf->gzip_http_version
        || (r->headers_in.msie6 && clcf->gzip_disable_msie6))
    {
        return NGX_DECLINED;
    }

#if (NGX_HTTP_DEGRADATION)
    if (clcf->gzip_disable_degradation && ngx_http_degraded(r)) {
        return NGX_DECLINED;
    }

#endif

    if (r->headers_in.via == NULL) {
        goto ok;
    }

    p = clcf->gzip_proxied;

    if (p & NGX_HTTP_GZIP_PROXIED_OFF) {
        return NGX_DECLINED;
    }

    if (p & NGX_HTTP_GZIP_PROXIED_ANY) {
        goto ok;
    }

    if (r->headers_in.authorization && (p & NGX_HTTP_GZIP_PROXIED_AUTH)) {
        goto ok;
    }

    e = r->headers_out.expires;

    if (e) {
        if (!(p & NGX_HTTP_GZIP_PROXIED_EXPIRED)) {
            return NGX_DECLINED;
        }

        expires = ngx_parse_http_time(e->value.data, e->value.len);
        if (expires == NGX_ERROR) {
            return NGX_DECLINED;
        }

        d = r->headers_out.date;

        if (d) {
            date = ngx_parse_http_time(d->value.data, d->value.len);
            if (date == NGX_ERROR) {
                return NGX_DECLINED;
            }

        } else {
            date = ngx_time();
        }

        if (expires < date) {
            goto ok;
        }

        return NGX_DECLINED;
    }

    cc = r->headers_out.cache_control;

    if (cc) {
        if ((p & NGX_HTTP_GZIP_PROXIED_NO_CACHE)
            && ngx_http_parse_multi_header_lines(r, cc, &no_cache, NULL))
        {
            goto ok;
        }

        if ((p & NGX_HTTP_GZIP_PROXIED_NO_STORE)
            && ngx_http_parse_multi_header_lines(r, cc, &no_store, NULL))
        {
            goto ok;
        }

        if ((p & NGX_HTTP_GZIP_PROXIED_PRIVATE)
            && ngx_http_parse_multi_header_lines(r, cc, &private, NULL))
        {
            goto ok;
        }

        return NGX_DECLINED;
    }

    if ((p & NGX_HTTP_GZIP_PROXIED_NO_LM) && r->headers_out.last_modified) {
        return NGX_DECLINED;
    }

    if ((p & NGX_HTTP_GZIP_PROXIED_NO_ETAG) && r->headers_out.etag) {
        return NGX_DECLINED;
    }

ok:

#if (NGX_PCRE)
    if (clcf->gzip_disable && r->headers_in.user_agent
        && ngx_regex_exec_array(clcf->gzip_disable,
                                &r->headers_in.user_agent->value,
                                r->connection->log)
           != NGX_DECLINED)
    {
        return NGX_DECLINED;
    }

#endif

    return NGX_OK;
}


static char *
ngx_http_brotli_alias(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                 *name;
    ngx_command_t             *core_cmd, local;
    ngx_http_core_loc_conf_t  *clcf;

    name = cmd->post;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    for (core_cmd = ngx_http_core_module.commands; core_cmd->name.len;
         core_cmd++)
    {
        if (core_cmd->name.len == name->len
            && ngx_strncmp(core_cmd->name.data, name->data, name->len) == 0)
        {
            local = *core_cmd;
            local.name = cmd->name;

            return core_cmd->set(cf, &local, clcf);
        }
    }

    return NGX_CONF_ERROR;
}


static ngx_int_t
ngx_http_brotli_ratio_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_brotli_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_brotli_filter_module);

    if (ctx == NULL || !ctx->success || ctx->bytes_out == 0) {
        v->not_found = 1;
        v->no_cacheable = 1;
        return NGX_OK;
    }

    v->data = ngx_pnalloc(r->pool, NGX_OFF_T_LEN + 4);
    if (v->data == NULL) {
        return NGX_ERROR;
    }

    v->len = ngx_sprintf(v->data, "%.3f",
                         (double) ctx->bytes_in / ctx->bytes_out) - v->data;

    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_brotli_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *v;

    v = ngx_http_add_variable(cf, &ngx_http_brotli_ratio,
                              NGX_HTTP_VAR_NOCACHEABLE);
    if (v == NULL) {
        return NGX_ERROR;
    }

    v->get_handler = ngx_http_brotli_ratio_variable;

    return NGX_OK;
}


static char *
ngx_http_brotli_level(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_int_t                 level, sign;
    ngx_str_t                *value;
#if (NGX_EXPR)
    ngx_array_t             **values;
    ngx_expr_when_id_t        id;
    ngx_conf_expr_num_ctx_t  *ctx;
#else
    ngx_int_t                *np;
#endif

    value = cf->args->elts;
    sign = 1;

    if (value[1].len && value[1].data[0] == '-') {
        sign = -1;
        level = ngx_atoi(value[1].data + 1, value[1].len - 1);

    } else {
        level = ngx_atoi(value[1].data, value[1].len);
    }

    if (level == NGX_ERROR) {
        return "has an invalid compression level";
    }

    level *= sign;
    if (level < BROTLI_MIN_QUALITY || level > BROTLI_MAX_QUALITY) {
        return "has an invalid compression level";
    }

#if (NGX_EXPR)
    values = (ngx_array_t **) ((u_char *) conf + cmd->offset);

    if (*values == NGX_CONF_UNSET_PTR) {
        *values = ngx_array_create(cf->pool, 2,
                                   sizeof(ngx_conf_expr_num_ctx_t));
        if (*values == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    id = ngx_expr_get_associated_when_id(cf);
    ctx = ngx_expr_find_ctx(*values, id, sizeof(ngx_conf_expr_num_ctx_t),
                            offsetof(ngx_conf_expr_num_ctx_t, expr_id));
    if (ctx) {
        return "is duplicate";
    }

    ctx = ngx_array_push(*values);
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->expr_id = id;
    ctx->value = level;
#else
    np = (ngx_int_t *) ((u_char *) conf + cmd->offset);

    if (*np != NGX_MAX_INT_T_VALUE) {
        return "is duplicate";
    }

    *np = level;
#endif

    return NGX_CONF_OK;
}


static char *
ngx_http_brotli_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char       *rv;
    size_t     *np, size, n, bits, min, max;
    ngx_str_t  *name;

    rv = ngx_conf_set_size_slot(cf, cmd, conf);
    if (rv != NGX_CONF_OK) {
        return rv;
    }

    np = (size_t *) ((u_char *) conf + cmd->offset);
    size = *np;
    name = &cmd->name;
    min = (size_t) 1 << BROTLI_MIN_WINDOW_BITS;
    max = (size_t) 1 << BROTLI_MAX_WINDOW_BITS;

    if (size < min || size > max || (size & (size - 1))) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must be a power of two between %uz and %uz",
                           name, min, max);
        return NGX_CONF_ERROR;
    }

    for (n = size, bits = 0; n > 1; n >>= 1) {
        bits++;
    }

    *np = bits;

    return NGX_CONF_OK;
}


static void *
ngx_http_brotli_create_conf(ngx_conf_t *cf)
{
    ngx_http_brotli_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_brotli_conf_t));
    if (conf == NULL) {
        return NULL;
    }

#if (NGX_EXPR)
    conf->enable = NGX_CONF_UNSET_PTR;
    conf->level = NGX_CONF_UNSET_PTR;
    conf->min_length = NGX_CONF_UNSET_PTR;
    conf->max_length = NGX_CONF_UNSET_PTR;
#else
    conf->enable = NGX_CONF_UNSET;
    conf->level = NGX_MAX_INT_T_VALUE;
    conf->min_length = NGX_CONF_UNSET_SIZE;
    conf->max_length = NGX_CONF_UNSET_SIZE;
#endif
    conf->bypass = NGX_CONF_UNSET_PTR;
    conf->no_buffer = NGX_CONF_UNSET;
    conf->postpone = NGX_CONF_UNSET_SIZE;
    conf->window = NGX_CONF_UNSET_SIZE;

    return conf;
}


static char *
ngx_http_brotli_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_brotli_conf_t  *prev = parent;
    ngx_http_brotli_conf_t  *conf = child;

#if (NGX_EXPR)
    if (ngx_conf_merge_expr_flag_value(cf, &conf->enable,
                                       prev->enable, 0) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_num_value(cf, &conf->level,
                                       prev->level, 6) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_size_value(cf, &conf->min_length,
                                       prev->min_length, 20) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_size_value(cf, &conf->max_length,
                                       prev->max_length, 0) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

#else
    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    if (conf->level == NGX_MAX_INT_T_VALUE) {
        conf->level = (prev->level == NGX_MAX_INT_T_VALUE)
                      ? 6 : prev->level;
    }
    ngx_conf_merge_size_value(conf->min_length, prev->min_length, 20);
    ngx_conf_merge_size_value(conf->max_length, prev->max_length, 0);
#endif

    ngx_conf_merge_ptr_value(conf->bypass, prev->bypass, NULL);
    ngx_conf_merge_value(conf->no_buffer, prev->no_buffer, 0);
    ngx_conf_merge_size_value(conf->postpone, prev->postpone, 0);
    ngx_conf_merge_size_value(conf->window, prev->window, 19);
    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs,
                              (128 * 1024) / ngx_pagesize, ngx_pagesize);

    if (ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                             &prev->types_keys, &prev->types,
                             ngx_http_html_default_types) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_brotli_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_brotli_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_brotli_body_filter;

    return NGX_OK;
}
