
/*
 * Copyright (C) Valery Kholodkov
 * Copyright (C) Yichun Zhang
 */


#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"


#include <nginx.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


static void ngx_unescape_uri_patched(u_char **dst, u_char **src,
    size_t size, ngx_uint_t type);


typedef struct {
    ngx_http_variable_t        *variable;
    ngx_uint_t                  index;
} ngx_http_eval_variable_t;


typedef struct {
    ngx_array_t                *variables;
    ngx_str_t                   eval_location;
    ngx_flag_t                  escalate;
    ngx_str_t                   override_content_type;
    ngx_flag_t                  subrequest_in_memory;
    size_t                      buffer_size;
} ngx_http_eval_loc_conf_t;


typedef struct {
    ngx_http_eval_loc_conf_t   *base_conf;
    ngx_http_variable_value_t **values;
    unsigned int                done:1;
    unsigned int                in_progress:1;
    ngx_int_t                   status;
    ngx_buf_t                   buffer;
} ngx_http_eval_ctx_t;


typedef ngx_int_t (*ngx_http_eval_format_handler_pt)(ngx_http_request_t *r,
    ngx_http_eval_ctx_t *ctx);


typedef struct {
    ngx_str_t                           content_type;
    ngx_http_eval_format_handler_pt     handler;
} ngx_http_eval_format_t;


static ngx_int_t
ngx_http_eval_init_variables(ngx_http_request_t *r, ngx_http_eval_ctx_t *ctx,
    ngx_http_eval_loc_conf_t *ecf);

static ngx_int_t ngx_http_eval_post_subrequest_handler(ngx_http_request_t *r,
    void *data, ngx_int_t rc);

static void *ngx_http_eval_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_eval_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child);

static char *ngx_http_eval_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

static char *ngx_http_eval_subrequest_in_memory(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);

static ngx_int_t ngx_http_eval_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_eval_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);

static void ngx_http_eval_discard_bufs(ngx_pool_t *pool, ngx_chain_t *in);

static ngx_int_t ngx_http_eval_init(ngx_conf_t *cf);

static ngx_int_t ngx_http_eval_octet_stream(ngx_http_request_t *r,
    ngx_http_eval_ctx_t *ctx);
static ngx_int_t ngx_http_eval_plain_text(ngx_http_request_t *r,
    ngx_http_eval_ctx_t *ctx);
static ngx_int_t ngx_http_eval_urlencoded(ngx_http_request_t *r,
    ngx_http_eval_ctx_t *ctx);


static ngx_flag_t  ngx_http_eval_requires_filter = 1;

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_http_eval_format_t ngx_http_eval_formats[] = {
    { ngx_string("application/octet-stream"), ngx_http_eval_octet_stream },
    { ngx_string("text/plain"), ngx_http_eval_plain_text },
    { ngx_string("application/x-www-form-urlencoded"),
      ngx_http_eval_urlencoded },

    { ngx_null_string, ngx_http_eval_plain_text }
};


static ngx_command_t  ngx_http_eval_commands[] = {

    { ngx_string("eval"),
      NGX_HTTP_LOC_CONF|NGX_CONF_1MORE|NGX_CONF_BLOCK,
      ngx_http_eval_block,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("eval_escalate"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_eval_loc_conf_t, escalate),
      NULL },

    { ngx_string("eval_override_content_type"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_eval_loc_conf_t, override_content_type),
      NULL },

    { ngx_string("eval_subrequest_in_memory"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_http_eval_subrequest_in_memory,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_eval_loc_conf_t, subrequest_in_memory),
      NULL },

    { ngx_string("eval_buffer_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_eval_loc_conf_t, buffer_size),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_eval_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_eval_init,                    /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_eval_create_loc_conf,         /* create location configuration */
    ngx_http_eval_merge_loc_conf           /* merge location configuration */
};


ngx_module_t  ngx_http_eval_module = {
    NGX_MODULE_V1,
    &ngx_http_eval_module_ctx,             /* module context */
    ngx_http_eval_commands,                /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_eval_handler(ngx_http_request_t *r)
{
    /* size_t                      loc_len; */
    ngx_str_t                   args;
    ngx_str_t                   subrequest_uri;
    ngx_uint_t                  flags;
    /* ngx_http_core_loc_conf_t   *clcf; */
    ngx_http_eval_loc_conf_t   *ecf;
    ngx_http_eval_ctx_t        *ctx;
    ngx_http_eval_ctx_t        *sr_ctx;
    ngx_http_request_t         *sr;
    ngx_int_t                   rc;
    ngx_http_post_subrequest_t *psr;
    u_char                     *p;

    /*
    if (r != r->main) {
        clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

        loc_len = r->valid_location ? clcf->name.len : 0;

        if (r->uri.len != loc_len) {
            r->uri.data += loc_len;
            r->uri.len -= loc_len;

        } else {
            r->uri.len = 1;
        }
    }
    */

    ecf = ngx_http_get_module_loc_conf(r, ngx_http_eval_module);

    if (ecf->variables == NULL || !ecf->variables->nelts) {
        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_eval_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_eval_ctx_t));
        if (ctx == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ctx->base_conf = ecf;

        ngx_http_set_ctx(r, ctx, ngx_http_eval_module);
    }

    if (ctx->done) {
        dd("subrequest done");
        if (!ecf->escalate
            || ctx->status == NGX_OK
            || ctx->status == NGX_HTTP_OK)
        {
            return NGX_DECLINED;
        }

        dd("status: %d", (int) ctx->status);

        return ctx->status;
    }

    if (ctx->in_progress) {
        dd("still in progress");
        return NGX_DONE;
    }

    psr = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
    if (psr == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_eval_init_variables(r, ctx, ecf) != NGX_OK) {
        return NGX_ERROR;
    }

    args = r->args;
    flags = 0;

    subrequest_uri.len = ecf->eval_location.len + r->uri.len;

    p = ngx_palloc(r->pool, subrequest_uri.len);
    if (p == NULL) {
        return NGX_ERROR;
    }

    subrequest_uri.data = p;

    p = ngx_copy(p, ecf->eval_location.data, ecf->eval_location.len);
    ngx_memcpy(p, r->uri.data, r->uri.len);

    if (ngx_http_parse_unsafe_uri(r, &subrequest_uri, &args, &flags)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    psr->handler = ngx_http_eval_post_subrequest_handler;
    psr->data = ctx;

    flags |= NGX_HTTP_SUBREQUEST_WAITED;

    dd("subrequest in memory : %d", (int) ecf->subrequest_in_memory);

    if (ecf->subrequest_in_memory) {
        flags |= NGX_HTTP_SUBREQUEST_IN_MEMORY;
    } else {
    }

    dd("issue subrequest");

    rc = ngx_http_subrequest(r, &subrequest_uri, &args, &sr, psr, flags);

    if (rc == NGX_ERROR || rc == NGX_DONE) {
        return rc;
    }

    sr->discard_body = 1;

    /* we don't want to forward certain request headers to the subrequest */
    sr->headers_in.content_length_n = 0;
    sr->headers_in.content_length = NULL;
    sr->headers_in.content_type = NULL;
#if (NGX_HTTP_GZIP)
    sr->headers_in.accept_encoding = NULL;
#endif

    ctx->in_progress = 1;

    /* XXX we don't allow eval in subrequests, i think? */
    sr_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_eval_ctx_t));
    if (sr_ctx == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ngx_http_set_ctx(sr, sr_ctx, ngx_http_eval_module);

    dd("wait for subrequest to complete");

    return NGX_DONE;
}


static ngx_int_t
ngx_http_eval_init_variables(ngx_http_request_t *r, ngx_http_eval_ctx_t *ctx,
    ngx_http_eval_loc_conf_t *ecf)
{
    ngx_uint_t                   i;
    ngx_http_eval_variable_t    *variable;

    ctx->values = ngx_pcalloc(r->pool,
                              ecf->variables->nelts
                              * sizeof(ngx_http_variable_value_t *));

    if (ctx->values == NULL) {
        return NGX_ERROR;
    }

    variable = ecf->variables->elts;

    for (i = 0;i<ecf->variables->nelts;i++) {
        ctx->values[i] = r->variables + variable[i].index;

        ctx->values[i]->valid = 0;
        ctx->values[i]->not_found = 1;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_eval_post_subrequest_handler(ngx_http_request_t *r, void *data,
    ngx_int_t rc)
{
    ngx_http_eval_ctx_t     *ctx = data;
    ngx_http_eval_format_t  *f = ngx_http_eval_formats;
    ngx_str_t                content_type;

    if (ctx->base_conf->override_content_type.len) {
        content_type.data = ctx->base_conf->override_content_type.data;
        content_type.len = ctx->base_conf->override_content_type.len;

    } else if (r->headers_out.content_type.len) {
        content_type.data = r->headers_out.content_type.data;
        content_type.len = r->headers_out.content_type.len;

    } else {
        content_type.data = (u_char *) "application/octet-stream";
        content_type.len = sizeof("application/octet-stream") - 1;
    }

    dd("content_type: %.*s", (int) content_type.len, content_type.data);

    while (f->content_type.len) {

        if (ngx_strncasecmp(f->content_type.data, content_type.data,
                            f->content_type.len)
            == 0)
        {
            f->handler(r, ctx);
            break;
        }

        f++;
    }

    ctx->done = 1;
    /* dd("rc == %d", rc); */
    ctx->status = rc;

#if 0
    /* work-around a bug in the nginx core (ngx_http_named_location)
     */
    r->parent->write_event_handler = ngx_http_core_run_phases;
#endif

    return NGX_OK;
}


/*
 * The next two evaluation methods assume we have at least one varible.
 *
 * ngx_http_eval_handler must guarantee this. *
 */
static ngx_int_t
ngx_http_eval_octet_stream(ngx_http_request_t *r, ngx_http_eval_ctx_t *ctx)
{
    ngx_http_variable_value_t *value = ctx->values[0];
    ngx_http_eval_ctx_t       *sr_ctx;

    dd("eval octet stream");

    sr_ctx = ngx_http_get_module_ctx(r, ngx_http_eval_module);

    if (sr_ctx && sr_ctx->buffer.start) {
        value->len = sr_ctx->buffer.last - sr_ctx->buffer.pos;
        value->data = sr_ctx->buffer.pos;
        value->valid = 1;
        value->not_found = 0;

        return NGX_OK;
    }

    if (r->upstream) {
        value->len = r->upstream->buffer.last - r->upstream->buffer.pos;
        value->data = r->upstream->buffer.pos;
        dd("found upstream buffer %d: %.*s", (int) value->len,
           (int) value->len, value->data);
        value->valid = 1;
        value->not_found = 0;
        dd("XXX no cacheable: %d", (int) value->no_cacheable);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_eval_plain_text(ngx_http_request_t *r, ngx_http_eval_ctx_t *ctx)
{
    u_char         *p;
    ngx_int_t       rc;

    ngx_http_variable_value_t   *value = ctx->values[0];

    dd("eval plain text");

    rc = ngx_http_eval_octet_stream(r, ctx);
    if (rc != NGX_OK) {
        return rc;
    }

    /*
     * Remove trailing spaces and control characters
     */
    if (value->valid) {
        p = value->data + value->len;

        while (p != value->data) {
            p--;

            if (*p != CR && *p != LF && *p != '\t' && *p != ' ') {
                break;
            }

            value->len--;
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_eval_set_variable_value(ngx_http_request_t *r,
    ngx_http_eval_ctx_t *ctx, ngx_str_t *name, ngx_str_t *value)
{
    ngx_uint_t                   i;
    ngx_http_eval_variable_t    *variable;

    variable = ctx->base_conf->variables->elts;

    for (i = 0; i<ctx->base_conf->variables->nelts; i++) {
        if (ngx_strncasecmp(variable[i].variable->name.data, name->data,
                            variable[i].variable->name.len)
            == 0)
        {
            ctx->values[i]->len = value->len;
            ctx->values[i]->data = value->data;
            ctx->values[i]->valid = 1;
            ctx->values[i]->not_found = 0;

            return NGX_OK;
        }
    }

    ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                  "eval: ignored undefined variable \"%V\"", value);

    return NGX_OK;
}


static ngx_int_t
ngx_http_eval_parse_param(ngx_http_request_t *r, ngx_http_eval_ctx_t *ctx,
    ngx_str_t *param)
{
    u_char                    *p, *src, *dst;

    ngx_str_t                  name;
    ngx_str_t                  value;

    /* param->data is not \0 terminated */
    p = (u_char *) memchr(param->data, '=', param->len);

    if (p == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "eval: invalid param \"%V\"", param);
        return NGX_ERROR;
    }

    name.data = param->data;
    name.len = p - param->data;

    value.data = p + 1;
    value.len = param->len - (p - param->data) - 1;

    src = dst = value.data;

    ngx_unescape_uri_patched(&dst, &src, value.len, NGX_UNESCAPE_URI);

    value.len = dst - value.data;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "eval param: \"%V\"=\"%V\"", &name, &value);

    return ngx_http_eval_set_variable_value(r, ctx, &name, &value);
}


static ngx_int_t
ngx_http_eval_urlencoded(ngx_http_request_t *r, ngx_http_eval_ctx_t *ctx)
{
    u_char          *pos, *last;
    ngx_str_t        param;
    ngx_int_t        rc;

    ngx_http_eval_ctx_t       *sr_ctx;

    sr_ctx = ngx_http_get_module_ctx(r, ngx_http_eval_module);

    if (sr_ctx && sr_ctx->buffer.start) {
        pos = sr_ctx->buffer.pos;
        last = sr_ctx->buffer.last;

    } else {
        if (!r->upstream
            || r->upstream->buffer.last == r->upstream->buffer.pos)
        {
            return NGX_OK;
        }

        pos = r->upstream->buffer.pos;
        last = r->upstream->buffer.last;
    }

    do {
        param.data = pos;
        param.len = 0;

        while (pos != last) {
            if (*pos == '&') {
                pos++;
                break;
            }

            if (*pos == CR || *pos == LF) {
                pos = last;
                break;
            }

            param.len++;
            pos++;
        }

        if (param.len != 0) {
            rc = ngx_http_eval_parse_param(r, ctx, &param);

            if (rc != NGX_OK) {
                return rc;
            }
        }

    } while (pos != last);

    return NGX_OK;
}


static void *
ngx_http_eval_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_eval_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_eval_loc_conf_t));

    if (conf == NULL) {
        return NULL;
    }

    conf->escalate = NGX_CONF_UNSET;
    conf->buffer_size = NGX_CONF_UNSET_SIZE;
    conf->subrequest_in_memory = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_eval_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_eval_loc_conf_t *prev = parent;
    ngx_http_eval_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->escalate, prev->escalate, 0);
    ngx_conf_merge_str_value(conf->override_content_type,
                             prev->override_content_type, "");
    ngx_conf_merge_size_value(conf->buffer_size, prev->buffer_size,
                              (size_t) ngx_pagesize);
    ngx_conf_merge_value(conf->subrequest_in_memory,
                         prev->subrequest_in_memory, 0);

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_eval_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    dd("XXX eval variable get_handle");

    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    v->len = 0;
    v->data = (u_char *) "";

    return NGX_OK;
}


static char *
ngx_http_eval_add_variables(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_eval_loc_conf_t            *ecf = conf;

    ngx_uint_t                           i;
    ngx_int_t                            index;
    ngx_str_t                           *value;
    ngx_http_variable_t                 *v;
    ngx_http_eval_variable_t            *variable;

    value = cf->args->elts;

    ecf->variables = ngx_array_create(cf->pool,
        cf->args->nelts, sizeof(ngx_http_eval_variable_t));

    if (ecf->variables == NULL) {
        return NGX_CONF_ERROR;
    }

    for (i = 1;i<cf->args->nelts;i++) {
        if (value[i].data[0] != '$') {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid variable name \"%V\"", &value[1]);
            return NGX_CONF_ERROR;
        }

        variable = ngx_array_push(ecf->variables);
        if (variable == NULL) {
            return NGX_CONF_ERROR;
        }

        value[i].len--;
        value[i].data++;

        v = ngx_http_add_variable(cf, &value[i], NGX_HTTP_VAR_CHANGEABLE);
        if (v == NULL) {
            return NGX_CONF_ERROR;
        }

        index = ngx_http_get_variable_index(cf, &value[i]);
        if (index == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }

        if (v->get_handler == NULL) {
            v->get_handler = ngx_http_eval_variable;
            v->data = index;
        }

        variable->variable = v;
        variable->index = index;
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_eval_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_eval_loc_conf_t  *pecf = conf;

    char                      *rv;
    void                      *mconf;
    ngx_str_t                  name;
    ngx_uint_t                 i;
    ngx_conf_t                 save;
    ngx_module_t             **modules;
    ngx_http_module_t         *module;
    ngx_http_conf_ctx_t       *ctx, *pctx;
    ngx_http_core_loc_conf_t  *clcf, *rclcf;
    ngx_http_core_srv_conf_t  *cscf;

#if defined(nginx_version) && nginx_version >= 8042 && nginx_version <= 8053
    return "does not work with " NGINX_VER;
#endif

    if (ngx_http_eval_add_variables(cf, cmd, conf) != NGX_CONF_OK) {
        return NGX_CONF_ERROR;
    }

    ctx = ngx_pcalloc(cf->pool, sizeof(ngx_http_conf_ctx_t));
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    pctx = cf->ctx;
    ctx->main_conf = pctx->main_conf;
    ctx->srv_conf = pctx->srv_conf;

    ctx->loc_conf = ngx_pcalloc(cf->pool, sizeof(void *) * ngx_http_max_module);
    if (ctx->loc_conf == NULL) {
        return NGX_CONF_ERROR;
    }

#if defined(nginx_version) && nginx_version >= 1009011
    modules = cf->cycle->modules;
#else
    modules = ngx_modules;
#endif

    for (i = 0; modules[i]; i++) {
        if (modules[i]->type != NGX_HTTP_MODULE) {
            continue;
        }

        module = modules[i]->ctx;

        if (module->create_loc_conf) {

            mconf = module->create_loc_conf(cf);
            if (mconf == NULL) {
                return NGX_CONF_ERROR;
            }

            ctx->loc_conf[modules[i]->ctx_index] = mconf;
        }
    }

    clcf = ctx->loc_conf[ngx_http_core_module.ctx_index];

    name.len = sizeof("/eval_") - 1 + NGX_OFF_T_LEN;

    name.data = ngx_palloc(cf->pool, name.len);

    if (name.data == NULL) {
        return NGX_CONF_ERROR;
    }

    name.len = ngx_sprintf(name.data, "/eval_%O", (off_t)(uintptr_t) clcf)
               - name.data;

    clcf->loc_conf = ctx->loc_conf;
    clcf->name = name;
    clcf->exact_match = 0;
    clcf->noname = 0;
    clcf->internal = 1;
    clcf->noregex = 1;

    cscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_core_module);
    if (cscf == NULL || cscf->ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    rclcf = cscf->ctx->loc_conf[ngx_http_core_module.ctx_index];
    if (rclcf == NULL) {
        return NGX_CONF_ERROR;
    }

    if (ngx_http_add_location(cf, &rclcf->locations, clcf) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    pecf->eval_location = clcf->name;

    save = *cf;
    cf->ctx = ctx;
    cf->cmd_type = NGX_HTTP_LOC_CONF;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    return rv;
}


static char *
ngx_http_eval_subrequest_in_memory(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    char                        *res;

    dd("eval subrequest in memory");

    res = ngx_conf_set_flag_slot(cf, cmd, conf);
    if (res != NGX_CONF_OK) {
        return res;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_eval_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_eval_handler;

    if (ngx_http_eval_requires_filter) {
        dd("requires filter");
        ngx_http_next_header_filter = ngx_http_top_header_filter;
        ngx_http_top_header_filter = ngx_http_eval_header_filter;

        ngx_http_next_body_filter = ngx_http_top_body_filter;
        ngx_http_top_body_filter = ngx_http_eval_body_filter;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_eval_header_filter(ngx_http_request_t *r)
{
    ngx_http_eval_ctx_t             *ctx;

    dd("header filter");

    if (r == r->main) {
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_eval_module);
    if (ctx == NULL) {
        dd("WARNING: ctx is null, forwarding header filter");
        return ngx_http_next_header_filter(r);
    }

    r->filter_need_in_memory = 1;

    /* suppress header output */

    dd("header filter called: type: %.*s",
       (int) r->headers_out.content_type.len,
       r->headers_out.content_type.data);

    return NGX_OK;
}


static ngx_int_t
ngx_http_eval_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_eval_ctx_t         *ctx;
    ngx_chain_t                 *cl;
    ngx_buf_t                   *b;
    ngx_http_eval_loc_conf_t    *conf;
    size_t                       len;
    ssize_t                      rest;

    dd("in body filter");

    if (r == r->main) {
        return ngx_http_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_eval_module);
    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    dd("in body filter");

    conf = ngx_http_get_module_loc_conf(r->parent, ngx_http_eval_module);

    if (conf->subrequest_in_memory) {
        return ngx_http_next_body_filter(r, in);
    }

    b = &ctx->buffer;

    if (b->start == NULL) {
        dd("allocate buffer");

        b->start = ngx_palloc(r->pool, conf->buffer_size);
        if (b->start == NULL) {
            return NGX_ERROR;
        }

        b->end = b->start + conf->buffer_size;
        b->pos = b->last = b->start;
    }

    for (cl = in; cl; cl = cl->next) {
        rest = b->end - b->last;
        if (rest == 0) {
            break;
        }

        if (!ngx_buf_in_memory(cl->buf)) {
            dd("buf not in memory!");
            continue;
        }

        len = cl->buf->last - cl->buf->pos;

        if (len == 0) {
            continue;
        }

        if (len > (size_t) rest) {
            /* we truncate the exceeding part of the response body */
            dd("truncate and ignore exceeding bufs");
            len = rest;
        }

        dd("copied data '%.*s' (len %d, c0: %d)", (int) len, cl->buf->pos,
           (int) len, (int) *(cl->buf->pos));
        b->last = ngx_copy(b->last, cl->buf->pos, len);
    }

    ngx_http_eval_discard_bufs(r->pool, in);

    return NGX_OK;
}


static void
ngx_http_eval_discard_bufs(ngx_pool_t *pool, ngx_chain_t *in)
{
    ngx_chain_t         *cl;

    for (cl = in; cl; cl = cl->next) {
#if 0
        if (cl->buf->temporary && cl->buf->memory
            && ngx_buf_size(cl->buf) > 0)
        {
            ngx_pfree(pool, cl->buf->start);
        }
#endif

        cl->buf->pos = cl->buf->last;
    }
}


/* XXX we also decode '+' to ' ' */
static void
ngx_unescape_uri_patched(u_char **dst, u_char **src, size_t size,
    ngx_uint_t type)
{
    u_char  *d, *s, ch, c, decoded;
    enum {
        sw_usual = 0,
        sw_quoted,
        sw_quoted_second
    } state;

    d = *dst;
    s = *src;

    state = 0;
    decoded = 0;

    while (size--) {

        ch = *s++;

        switch (state) {
        case sw_usual:
            if (ch == '?'
                && (type & (NGX_UNESCAPE_URI|NGX_UNESCAPE_REDIRECT)))
            {
                *d++ = ch;
                goto done;
            }

            if (ch == '%') {
                state = sw_quoted;
                break;
            }

            if (ch == '+') {
                *d++ = ' ';
                break;
            }

            *d++ = ch;
            break;

        case sw_quoted:

            if (ch >= '0' && ch <= '9') {
                decoded = (u_char) (ch - '0');
                state = sw_quoted_second;
                break;
            }

            c = (u_char) (ch | 0x20);
            if (c >= 'a' && c <= 'f') {
                decoded = (u_char) (c - 'a' + 10);
                state = sw_quoted_second;
                break;
            }

            /* the invalid quoted character */

            state = sw_usual;

            *d++ = ch;

            break;

        case sw_quoted_second:

            state = sw_usual;

            if (ch >= '0' && ch <= '9') {
                ch = (u_char) ((decoded << 4) + ch - '0');

                if (type & NGX_UNESCAPE_REDIRECT) {
                    if (ch > '%' && ch < 0x7f) {
                        *d++ = ch;
                        break;
                    }

                    *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);

                    break;
                }

                *d++ = ch;

                break;
            }

            c = (u_char) (ch | 0x20);
            if (c >= 'a' && c <= 'f') {
                ch = (u_char) ((decoded << 4) + c - 'a' + 10);

                if (type & NGX_UNESCAPE_URI) {
                    if (ch == '?') {
                        *d++ = ch;
                        goto done;
                    }

                    *d++ = ch;
                    break;
                }

                if (type & NGX_UNESCAPE_REDIRECT) {
                    if (ch == '?') {
                        *d++ = ch;
                        goto done;
                    }

                    if (ch > '%' && ch < 0x7f) {
                        *d++ = ch;
                        break;
                    }

                    *d++ = '%'; *d++ = *(s - 2); *d++ = *(s - 1);
                    break;
                }

                *d++ = ch;

                break;
            }

            /* the invalid quoted character */

            break;
        }
    }

done:

    *dst = d;
    *src = s;
}
