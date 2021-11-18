/* 
 * mod_zip
 *
 * Copyright (C) Evan Miller
 *
 * This module may be distributed under the same terms as Nginx itself.
 */

#include "ngx_http_zip_module.h"
#include "ngx_http_zip_parsers.h"
#include "ngx_http_zip_file.h"
#include "ngx_http_zip_headers.h"

static size_t ngx_chain_length(ngx_chain_t *chain_link);
static ngx_chain_t *ngx_chain_last_link(ngx_chain_t *chain_link);
static ngx_int_t ngx_http_zip_discard_chain(ngx_http_request_t *r,
        ngx_chain_t *in);

static ngx_int_t ngx_http_zip_ranges_intersect(ngx_http_zip_range_t *range1,
        ngx_http_zip_range_t *range2);

static ngx_int_t ngx_http_zip_copy_unparsed_request(ngx_http_request_t *r,
        ngx_chain_t *in, ngx_http_zip_ctx_t *ctx);
static ngx_int_t ngx_http_zip_set_headers(ngx_http_request_t *r, 
        ngx_http_zip_ctx_t *ctx);

static ngx_int_t ngx_http_zip_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_zip_body_filter(ngx_http_request_t *r, 
        ngx_chain_t *in);
static ngx_int_t ngx_http_zip_main_request_body_filter(ngx_http_request_t *r,
        ngx_chain_t *in);
static ngx_int_t ngx_http_zip_subrequest_body_filter(ngx_http_request_t *r, 
        ngx_chain_t *in);

static ngx_int_t ngx_http_zip_subrequest_update_crc32(ngx_chain_t *in, 
        ngx_http_zip_file_t *file);
static ngx_int_t ngx_http_zip_subrequest_done(ngx_http_request_t *r, void *data, ngx_int_t rc);

static ngx_int_t ngx_http_zip_send_pieces(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx);
static ngx_int_t ngx_http_zip_send_header_piece(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range);
static ngx_int_t ngx_http_zip_send_file_piece(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range);
static ngx_int_t ngx_http_zip_send_directory_piece(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range);
static ngx_int_t ngx_http_zip_send_trailer_piece(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range);
static ngx_int_t ngx_http_zip_send_central_directory_piece(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range);
static ngx_int_t ngx_http_zip_send_piece(ngx_http_request_t *r, 
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range);

static ngx_int_t ngx_http_zip_send_boundary(ngx_http_request_t *r, 
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_range_t *range);
static ngx_int_t ngx_http_zip_send_final_boundary(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx);

static ngx_int_t ngx_http_zip_init(ngx_conf_t *cf);

static ngx_int_t ngx_http_zip_main_request_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_zip_subrequest_header_filter(ngx_http_request_t *r);

static ngx_str_t ngx_http_zip_header_variable_name = ngx_string("upstream_http_x_archive_files");

static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;

static ngx_http_module_t  ngx_http_zip_module_ctx = {
    NULL,                       /* preconfiguration */
    ngx_http_zip_init,          /* postconfiguration */

    NULL,                       /* create main configuration */
    NULL,                       /* init main configuration */

    NULL,                       /* create server configuration */
    NULL,                       /* merge server configuration */

    NULL,                       /* create location configuration */
    NULL                        /* merge location configuration */
};

ngx_module_t  ngx_http_zip_module = {
    NGX_MODULE_V1,
    &ngx_http_zip_module_ctx,   /* module context */
    NULL,                       /* module directives */
    NGX_HTTP_MODULE,            /* module type */
    NULL,                       /* init master */
    NULL,                       /* init module */
    NULL,                       /* init process */
    NULL,                       /* init thread */
    NULL,                       /* exit thread */
    NULL,                       /* exit process */
    NULL,                       /* exit master */
    NGX_MODULE_V1_PADDING
};

static size_t ngx_chain_length(ngx_chain_t *chain_link)
{
    size_t len;
    for (len=0; chain_link; chain_link = chain_link->next) {
        len += chain_link->buf->last - chain_link->buf->pos;
    }
    return len;
}

static ngx_chain_t *ngx_chain_last_link(ngx_chain_t *chain_link)
{
    ngx_chain_t *cl;
    for (cl = chain_link; cl->next; cl = cl->next) {
        /* void */
    }
    return cl;
}

static ngx_int_t
ngx_http_zip_discard_chain(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_chain_t         *chain_link;

    for (chain_link = in; chain_link; chain_link = chain_link->next) {
        chain_link->buf->flush = 1;
        chain_link->buf->sync = 1;
        chain_link->buf->temporary = 0;
        chain_link->buf->memory = 0;
        chain_link->buf->mmap = 0;
        chain_link->buf->last = chain_link->buf->pos;
    }

    return ngx_http_next_body_filter(r, in);
}

static ngx_int_t
ngx_http_zip_ranges_intersect(ngx_http_zip_range_t *range1, ngx_http_zip_range_t *range2)
{
    return !(range1->start >= range2->end || range2->start >= range1->end);
}

static ngx_int_t ngx_http_zip_copy_unparsed_request(ngx_http_request_t *r,
        ngx_chain_t *in, ngx_http_zip_ctx_t *ctx)
{
    ngx_str_t   *old_unparsed_request;
    ngx_chain_t *chain_link;
    size_t       len, offset = 0;

    old_unparsed_request = ctx->unparsed_request;

    len = ngx_chain_length(in);

    if (old_unparsed_request != NULL)
        len += old_unparsed_request->len;
    
    if ((ctx->unparsed_request = ngx_palloc(r->pool, sizeof(ngx_str_t))) == NULL 
        || (ctx->unparsed_request->data = ngx_palloc(r->pool, len)) == NULL) {
        return NGX_ERROR;
    }

    if (old_unparsed_request != NULL) {
        ngx_memcpy(ctx->unparsed_request->data, old_unparsed_request->data, old_unparsed_request->len);
        offset += old_unparsed_request->len;
    }

    for (chain_link = in; chain_link; chain_link = chain_link->next ) {
        ngx_memcpy(ctx->unparsed_request->data + offset, chain_link->buf->pos, chain_link->buf->last - chain_link->buf->pos);
        offset += chain_link->buf->last - chain_link->buf->pos;
    }

    ctx->unparsed_request->len = offset;

    chain_link = ngx_chain_last_link(in);

    return chain_link->buf->last_buf ? NGX_OK: NGX_AGAIN;
}

/* 
 * The header filter looks for "X-Archive-Files: zip" and allocates
 * a module context struct if found
 */
static ngx_int_t ngx_http_zip_header_filter(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: entering header filter");

    if (r != r->main)
        return ngx_http_zip_subrequest_header_filter(r);

    return ngx_http_zip_main_request_header_filter(r);
}

static ngx_int_t
ngx_http_zip_main_request_header_filter(ngx_http_request_t *r)
{
    ngx_http_variable_value_t  *vv;
    ngx_http_zip_ctx_t         *ctx;

    if ((ctx = ngx_http_get_module_ctx(r, ngx_http_zip_module)) != NULL)
        return ngx_http_next_header_filter(r);

    if ((vv = ngx_palloc(r->pool, sizeof(ngx_http_variable_value_t))) == NULL) 
        return NGX_ERROR;
  
    /* Look for X-Archive-Files */
    ngx_int_t variable_header_status = NGX_OK;
    if (r->upstream) {
        variable_header_status = ngx_http_variable_unknown_header(vv,
                &ngx_http_zip_header_variable_name,
                &r->upstream->headers_in.headers.part, sizeof("upstream_http_") - 1); 
    } else if (r->headers_out.status == NGX_HTTP_OK) {
        variable_header_status = ngx_http_variable_unknown_header(vv,
                &ngx_http_zip_header_variable_name,
                &r->headers_out.headers.part, sizeof("upstream_http_") - 1); 
    } else {
        vv->not_found = 1;
    }

    if (variable_header_status != NGX_OK || vv->not_found || 
            ngx_strncmp(vv->data, "zip", sizeof("zip") - 1) != 0) {
        return ngx_http_next_header_filter(r);
    }
    
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: X-Archive-Files found");

    if ((ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_zip_ctx_t))) == NULL 
        || ngx_array_init(&ctx->files, r->pool, 1, sizeof(ngx_http_zip_file_t)) == NGX_ERROR
        || ngx_array_init(&ctx->ranges, r->pool, 1, sizeof(ngx_http_zip_range_t)) == NGX_ERROR)
        return NGX_ERROR;
    
    ngx_http_set_ctx(r, ctx, ngx_http_zip_module);

    return NGX_OK;
}

static ngx_int_t
ngx_http_zip_subrequest_header_filter(ngx_http_request_t *r)
{
    ngx_http_zip_ctx_t    *ctx;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_zip_module);
    if (ctx != NULL) {
        if (r->headers_out.status != NGX_HTTP_OK &&
                r->headers_out.status != NGX_HTTP_PARTIAL_CONTENT) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "mod_zip: a subrequest returned %d, aborting...",
                    r->headers_out.status);
            ctx->abort = 1;
            return NGX_ERROR;
        }
        if (ctx->missing_crc32) {
            r->filter_need_in_memory = 1;
        }
    }
    return ngx_http_next_header_filter(r);
}

static ngx_int_t
ngx_http_zip_set_headers(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx)
{
    time_t if_range, last_modified;

    if (ngx_http_zip_add_cache_control(r) == NGX_ERROR) {
        return NGX_ERROR;
    }

    r->headers_out.content_type_len = sizeof(NGX_ZIP_MIME_TYPE) - 1;
    ngx_str_set(&r->headers_out.content_type, NGX_ZIP_MIME_TYPE);
    ngx_http_clear_content_length(r);

    if (ctx->missing_crc32) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: Clearing Accept-Ranges header");
        ngx_http_clear_accept_ranges(r);
    }
    r->headers_out.content_length_n = ctx->archive_size;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "mod_zip: Archive will be %O bytes", ctx->archive_size);
    if (r->headers_in.range) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: Range found");
        if (ctx->missing_crc32) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "mod_zip: Missing checksums, ignoring Range");
            return NGX_OK;
        }
        if (r->headers_in.if_range && r->upstream) {
            if_range = ngx_http_parse_time(r->headers_in.if_range->value.data,
                    r->headers_in.if_range->value.len);
            if (if_range == NGX_ERROR) { /* treat as ETag */
                if (r->upstream->headers_in.etag) {
                    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                            "mod_zip: If-Range = %V, ETag = %V", 
                            &r->headers_in.if_range->value, &r->upstream->headers_in.etag->value);
                    if (r->upstream->headers_in.etag->value.len != r->headers_in.if_range->value.len
                            || ngx_strncmp(r->upstream->headers_in.etag->value.data,
                                r->headers_in.if_range->value.data,
                                r->headers_in.if_range->value.len)) {
                        return NGX_OK;
                    }
                } else {
                    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                            "mod_zip: No ETag from upstream");
                    return NGX_OK;
                }
            } else { /* treat as modification time */
                if (r->upstream->headers_in.last_modified) {
                    last_modified = ngx_http_parse_time(r->upstream->headers_in.last_modified->value.data,
                            r->upstream->headers_in.last_modified->value.len);
                    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                            "mod_zip: If-Range = %d, Last-Modified = %d", 
                            if_range, last_modified);
                    if (if_range != last_modified && last_modified != -1) {
                        return NGX_OK;
                    }
                } else {
                    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                            "mod_zip: No Last-Modified from upstream");
                    return NGX_OK;
                }
            }
        }
        if (ngx_http_zip_parse_range(r, &r->headers_in.range->value, ctx) 
                == NGX_ERROR) {
            r->headers_out.status = NGX_HTTP_RANGE_NOT_SATISFIABLE;
            if (ngx_http_zip_add_full_content_range(r) == NGX_ERROR) {
                return NGX_ERROR;
            }
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "mod_zip: Range not satisfiable");
            ctx->ranges.nelts = 0;
            return NGX_HTTP_RANGE_NOT_SATISFIABLE;
        }
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: Range is satisfiable");
        if (ctx->ranges.nelts == 1) {
            if (ngx_http_zip_add_partial_content_range(r, ctx) == NGX_ERROR) {
                return NGX_ERROR;
            }
        } else {
            if (ngx_http_zip_init_multipart_range(r, ctx) == NGX_ERROR) {
                return NGX_ERROR;
            }
        }
        r->headers_out.status = NGX_HTTP_PARTIAL_CONTENT;
        r->headers_out.status_line.len = 0;
    }

    return NGX_OK;
}

static ngx_int_t 
ngx_http_zip_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    if (r != r->main) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: entering subrequest body filter");
        return ngx_http_zip_subrequest_body_filter(r, in);
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: entering main request body filter");
    return ngx_http_zip_main_request_body_filter(r, in);
}

static ngx_int_t
ngx_http_zip_subrequest_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_zip_sr_ctx_t *sr_ctx;

    sr_ctx = ngx_http_get_module_ctx(r, ngx_http_zip_module);

    if (in && sr_ctx && sr_ctx->requesting_file->missing_crc32) {
        uint32_t old_crc32 = sr_ctx->requesting_file->crc32;

        ngx_http_zip_subrequest_update_crc32(in, sr_ctx->requesting_file);

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
                "mod_zip: updated CRC-32 (%08Xd -> %08Xd)", old_crc32, sr_ctx->requesting_file->crc32);

        (void)old_crc32;
    }
    
    return ngx_http_next_body_filter(r, in);
}

static ngx_int_t
ngx_http_zip_subrequest_update_crc32(ngx_chain_t *in, 
        ngx_http_zip_file_t *file)
{
    ngx_chain_t *cl;
    size_t       len;
    u_char      *p;

    if (file == NULL) 
        return NGX_ERROR;

    for (cl = in; cl != NULL; cl = cl->next) {
        p = cl->buf->pos;
        len = cl->buf->last - p;

        ngx_crc32_update(&file->crc32, p, len);
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_zip_subrequest_done(ngx_http_request_t *r, void *data, ngx_int_t rc)
{
    ngx_http_zip_piece_t *piece = (ngx_http_zip_piece_t *)data;

    (void)piece; /* fix warning */

    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "mod_zip: subrequest for \"%V?%V\" done, result %d",
            &piece->file->uri, &piece->file->args, rc);

    return rc;
}

static ngx_int_t
ngx_http_zip_main_request_body_filter(ngx_http_request_t *r,
        ngx_chain_t *in)
{
    ngx_http_zip_ctx_t   *ctx;
    ngx_chain_t          *chain_link;
    int rc;

    ctx = ngx_http_get_module_ctx(r, ngx_http_zip_module);

    if (ctx == NULL || ctx->trailer_sent) {
        return ngx_http_next_body_filter(r, in);
    }

    if (ctx->abort) {
        return NGX_ERROR;
    }

    if (r->headers_out.status != NGX_HTTP_OK &&
            r->headers_out.status != NGX_HTTP_PARTIAL_CONTENT) {
        return ngx_http_next_body_filter(r, in);
    }

    if (ctx->parsed) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: restarting subrequests");
        return ngx_http_zip_send_pieces(r, ctx);
    }

    if (in == NULL) {
        return ngx_http_next_body_filter(r, NULL);
    }

    rc = ngx_http_zip_copy_unparsed_request(r, in, ctx);
    if (rc == NGX_AGAIN) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: not the last buf");
        return ngx_http_zip_discard_chain(r, in);
    } else if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "mod_zip: about to parse list");

    if (ngx_http_zip_parse_request(ctx) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "mod_zip: invalid file list from upstream");
        return NGX_ERROR;
    }

    if (ngx_http_zip_generate_pieces(r, ctx) == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (!r->header_sent) {
        rc = ngx_http_zip_set_headers(r, ctx);
        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }
        if (rc == NGX_HTTP_RANGE_NOT_SATISFIABLE) {
            return ngx_http_special_response_handler(r, rc);
        }
        if ((rc = ngx_http_send_header(r)) != NGX_OK) {
            return rc;
        }
    }

    chain_link = ngx_chain_last_link(in);
    chain_link->buf->last_buf = 0;

    if (ngx_http_zip_strip_range_header(r) == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "mod_zip: failed to strip Range: header from request");
        return NGX_ERROR;
    }

    return ngx_http_zip_send_pieces(r, ctx);
}

static ngx_int_t
ngx_http_zip_send_header_piece(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range)
{
    ngx_chain_t *link;
    if ((link = ngx_http_zip_file_header_chain_link(r, ctx, piece, range)) == NULL)
        return NGX_ERROR;
    return ngx_http_next_body_filter(r, link);
}

static ngx_int_t
ngx_http_zip_send_file_piece(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range)
{
    ngx_http_zip_sr_ctx_t *sr_ctx;
    ngx_http_request_t *sr;
    ngx_http_post_subrequest_t *ps;
    ngx_int_t rc;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "mod_zip: subrequest for \"%V?%V\"", &piece->file->uri, &piece->file->args);
    // need to check if the context has something going on....
    if (ctx->wait) {
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: have a wait context for \"%V?%V\"",
                &ctx->wait->uri, &ctx->wait->args);
        if (!ctx->wait->done) {
            ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "mod_zip: wait NOT DONE  \"%V?%V\"",
                    &ctx->wait->uri, &ctx->wait->args);
            return NGX_AGAIN;
        }
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: wait \"%V?%V\" done",
                &ctx->wait->uri, &ctx->wait->args);
        ctx->wait = NULL;
    }

    ps = ngx_palloc(r->pool, sizeof(ngx_http_post_subrequest_t));
    if (ps == NULL) {
        return NGX_ERROR;
    }

    ps->handler = ngx_http_zip_subrequest_done;
    ps->data = piece;

    rc = ngx_http_subrequest(r, &piece->file->uri, &piece->file->args, &sr, ps, NGX_HTTP_SUBREQUEST_WAITED);
    ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "mod_zip: subrequest for \"%V?%V\" initiated, result %d", 
            &piece->file->uri, &piece->file->args, rc);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    sr->allow_ranges = 1;
    sr->subrequest_ranges = 1;
    sr->single_range = 1;

    rc = ngx_http_zip_init_subrequest_headers(r, sr, &piece->range, req_range);
    if (sr->headers_in.range) {
        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: subrequest for \"%V?%V\" Range: %V", 
                &piece->file->uri, &piece->file->args, &sr->headers_in.range->value);
    }
    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if ((sr_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_zip_ctx_t))) == NULL) {
        return NGX_ERROR;
    }

    sr_ctx->requesting_file = piece->file;

    ngx_http_set_ctx(sr, sr_ctx, ngx_http_zip_module);
    if (ctx->wait) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "mod_zip : only one subrequest may be waited at the same time; ");
        return NGX_ERROR;
    }
    ctx->wait = sr;
    return NGX_AGAIN;   // must be NGX_AGAIN
}

static ngx_int_t ngx_http_zip_send_directory_piece(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range)
{
    // Directory has no data.
    return NGX_OK;
}

static ngx_int_t
ngx_http_zip_send_trailer_piece(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range)
{
    ngx_chain_t *link;

    if (piece->file->missing_crc32) { // should always be true, but if we somehow needed trailer piece - go on
        uint32_t old_crc32 = piece->file->crc32;
        ngx_crc32_final(piece->file->crc32);

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: finalized CRC-32 (%08Xd -> %08Xd)", old_crc32, piece->file->crc32);
        (void)old_crc32;
    }

    if ((link = ngx_http_zip_data_descriptor_chain_link(r, piece, req_range)) == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: data descriptor failed");
        return NGX_ERROR;
    }
    return ngx_http_next_body_filter(r, link);
}

static ngx_int_t
ngx_http_zip_send_central_directory_piece(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range)
{
    ngx_chain_t *link;

    if ((link = ngx_http_zip_central_directory_chain_link(r, ctx, piece, req_range)) == NULL) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: CD piece failed");
        return NGX_ERROR;
    }
    return ngx_http_next_body_filter(r, link);
}

static ngx_int_t
ngx_http_zip_send_piece(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *req_range)
{
    ngx_int_t rc = NGX_ERROR;

    if (piece->type == zip_header_piece) {
        rc = ngx_http_zip_send_header_piece(r, ctx, piece, req_range);
    } else if (piece->type == zip_file_piece) {
        rc = ngx_http_zip_send_file_piece(r, ctx, piece, req_range);
    } else if (piece->type == zip_dir_piece) {
        rc = ngx_http_zip_send_directory_piece(r, ctx, piece, req_range);
    } else if (piece->type == zip_trailer_piece) {
        rc = ngx_http_zip_send_trailer_piece(r, ctx, piece, req_range);
    } else if (piece->type == zip_central_directory_piece) {
        rc = ngx_http_zip_send_central_directory_piece(r, ctx, piece, req_range);
    }

    return rc;
}

static ngx_int_t
ngx_http_zip_send_boundary(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_range_t *range)
{
    ngx_chain_t *link;
    ngx_buf_t   *b;

    if (range->boundary_sent) 
        return NGX_OK;

    if ((link = ngx_alloc_chain_link(r->pool)) == NULL 
        || (b = ngx_calloc_buf(r->pool)) == NULL) 
        return NGX_ERROR;

    b->memory = 1;
    b->pos = range->boundary_header.data;
    b->last = b->pos + range->boundary_header.len;

    link->buf = b;
    link->next = NULL;

    range->boundary_sent = 1;
    return ngx_http_next_body_filter(r, link);
}

static ngx_int_t
ngx_http_zip_send_final_boundary(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx)
{
    size_t len;
    ngx_chain_t *link;
    ngx_buf_t   *b;

    if ((link = ngx_alloc_chain_link(r->pool)) == NULL
        || (b = ngx_calloc_buf(r->pool)) == NULL)
        return NGX_ERROR;

    len = sizeof(CRLF "--") - 1 + NGX_ATOMIC_T_LEN + sizeof("--" CRLF) - 1;

    b->memory = 1;
    if ((b->pos = ngx_palloc(r->pool, len)) == NULL) 
        return NGX_ERROR;
   
    b->last = ngx_sprintf(b->pos, CRLF "--%0muA--" CRLF, ctx->boundary);

    link->buf = b;
    link->next = NULL;

    return ngx_http_next_body_filter(r, link);
}

/* Initiate one or more subrequests for files to put in the ZIP archive */
static ngx_int_t
ngx_http_zip_send_pieces(ngx_http_request_t *r, 
        ngx_http_zip_ctx_t *ctx)
{
    ngx_int_t             rc = NGX_OK, pieces_sent = 0;
    ngx_http_zip_piece_t *piece;
    ngx_http_zip_range_t *req_range = NULL;

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "mod_zip: sending pieces, starting with piece %d of total %d", ctx->pieces_i, ctx->pieces_n);

    switch(ctx->ranges.nelts) {
        case 0:
            while (rc == NGX_OK && ctx->pieces_i < ctx->pieces_n) {
                piece = &ctx->pieces[ctx->pieces_i++];
                pieces_sent++;
                ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: no ranges / sending piece type %d", piece->type);
                rc = ngx_http_zip_send_piece(r, ctx, piece, NULL);
            }
            break;
        case 1:
            req_range = &((ngx_http_zip_range_t *)ctx->ranges.elts)[0];
            while (rc == NGX_OK && ctx->pieces_i < ctx->pieces_n) {
                piece = &ctx->pieces[ctx->pieces_i++];
                if (ngx_http_zip_ranges_intersect(&piece->range, req_range)) {
                    pieces_sent++;
                    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "mod_zip: 1 range / sending piece type %d", piece->type);
                    rc = ngx_http_zip_send_piece(r, ctx, piece, req_range);
                }
            }
            break;
        default:
            while (rc == NGX_OK && ctx->ranges_i < ctx->ranges.nelts) {
                req_range = &((ngx_http_zip_range_t *)ctx->ranges.elts)[ctx->ranges_i];
                ngx_log_debug4(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                        "mod_zip: sending range #%d start=%O end=%O (size %d)", 
                        ctx->ranges_i, req_range->start, req_range->end, req_range->boundary_header.len);
                rc = ngx_http_zip_send_boundary(r, ctx, req_range);
                while (rc == NGX_OK && ctx->pieces_i < ctx->pieces_n) {
                    piece = &ctx->pieces[ctx->pieces_i++];
                    if (ngx_http_zip_ranges_intersect(&piece->range, req_range)) {
                        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                                "mod_zip: sending range=%d piece=%d",
                                ctx->ranges_i, pieces_sent);
                        pieces_sent++;
                        rc = ngx_http_zip_send_piece(r, ctx, piece, req_range);
                    }
                }

                if (rc == NGX_OK) {
                    ctx->ranges_i++;
                    ctx->pieces_i = 0;
                }
            }

            if (rc == NGX_OK) {
                ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                        "mod_zip: sending final boundary");
                rc = ngx_http_zip_send_final_boundary(r, ctx);
            }
            break;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
            "mod_zip: sent %d pieces, last rc = %d", pieces_sent, rc);

    if (rc == NGX_OK) {
        ctx->trailer_sent = 1;
        return ngx_http_send_special(r, NGX_HTTP_LAST);
    }

    /* NGX_DONE, NGX_AGAIN or NGX_ERROR */
    return rc;
}

/* Install the module filters */
static ngx_int_t
ngx_http_zip_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_zip_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_zip_body_filter;

    return NGX_OK;
}
