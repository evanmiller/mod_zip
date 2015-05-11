#include "ngx_http_zip_module.h"
#include "ngx_http_zip_headers.h"
#include "ngx_http_zip_file.h"

ngx_int_t 
ngx_http_zip_add_cache_control(ngx_http_request_t *r)
{
    ngx_table_elt_t           **ccp, *cc;
    ngx_uint_t                  i;

    /* convoluted way of adding Cache-Control: max-age=0 */
    /* The header is necessary so IE doesn't barf */
    ccp = r->headers_out.cache_control.elts;

    if (ccp == NULL) {
        if (ngx_array_init(&r->headers_out.cache_control, r->pool,
                    1, sizeof(ngx_table_elt_t *))
                != NGX_OK)
        {
            return NGX_ERROR;
        }

        ccp = ngx_array_push(&r->headers_out.cache_control);
        if (ccp == NULL) {
            return NGX_ERROR;
        }

        cc = ngx_list_push(&r->headers_out.headers);
        if (cc == NULL) {
            return NGX_ERROR;
        }

        cc->hash = 1;
        ngx_str_set(&cc->key, "Cache-Control");

        *ccp = cc;

    } else {
        for (i = 1; i < r->headers_out.cache_control.nelts; i++) {
            ccp[i]->hash = 0;
        }

        cc = ccp[0];
    }

    ngx_str_set(&cc->value, "max-age=0");

    return NGX_OK;
}

ngx_int_t 
ngx_http_zip_add_content_range_header(ngx_http_request_t *r)
{
    ngx_table_elt_t              *content_range;

    content_range = ngx_list_push(&r->headers_out.headers);
    if (content_range == NULL) {
        return NGX_ERROR;
    }

    r->headers_out.content_range = content_range;

    content_range->hash = 1;
    ngx_str_set(&content_range->key, "Content-Range");

    if (r->headers_out.content_length) {
        r->headers_out.content_length->hash = 0;
        r->headers_out.content_length = NULL;
    }

    return NGX_OK;
}

ngx_int_t 
ngx_http_zip_add_full_content_range(ngx_http_request_t *r)
{
    ngx_table_elt_t              *content_range;

    if (ngx_http_zip_add_content_range_header(r) == NGX_ERROR) {
        return NGX_ERROR;
    }

    content_range = r->headers_out.content_range;
    if (content_range == NULL) {
        return NGX_ERROR;
    }

    content_range->value.data = ngx_palloc(r->pool,
            sizeof("bytes */") - 1 + NGX_OFF_T_LEN);
    if (content_range->value.data == NULL) {
        return NGX_ERROR;
    }

    content_range->value.len = ngx_sprintf(content_range->value.data,
            "bytes */%O", r->headers_out.content_length_n)
        - content_range->value.data;

    return NGX_OK;
}

ngx_int_t
ngx_http_zip_init_multipart_range(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx)
{
    ngx_uint_t i;
    ngx_http_zip_range_t *range;
    size_t len, message_len = 0;

    len = sizeof(CRLF "--") - 1 + NGX_ATOMIC_T_LEN
        + sizeof(CRLF "Content-Type: " NGX_ZIP_MIME_TYPE) - 1
        + sizeof(CRLF "Content-Range: bytes ") - 1
        + sizeof(/* start */ "-" /* end */ "/" /* size */ CRLF CRLF) - 1
        + 3 * NGX_OFF_T_LEN;

    ctx->boundary = ngx_next_temp_number(0);

    r->headers_out.content_type.data = ngx_palloc(r->pool,
            sizeof("Content-Type: multipart/byteranges; boundary=") - 1
            + NGX_ATOMIC_T_LEN);
    if (r->headers_out.content_type.data == NULL) {
        return NGX_ERROR;
    }

    r->headers_out.content_type.len =
        ngx_sprintf(r->headers_out.content_type.data,
                "multipart/byteranges; boundary=%0muA", ctx->boundary)
        - r->headers_out.content_type.data;

    for (i=0; i < ctx->ranges.nelts; i++) {
        range = &((ngx_http_zip_range_t *)ctx->ranges.elts)[i];
        range->boundary_header.data = ngx_palloc(r->pool, len);
        if (range->boundary_header.data == NULL) {
            return NGX_ERROR;
        }

        range->boundary_header.len = ngx_sprintf(range->boundary_header.data,
                CRLF "--%0muA" CRLF
                "Content-Type: " NGX_ZIP_MIME_TYPE CRLF
                "Content-Range: bytes %O-%O/%O" CRLF CRLF,
                ctx->boundary, range->start, range->end - 1, ctx->archive_size)
            - range->boundary_header.data;

        ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                "mod_zip: Allocating boundary for range start=%O end=%O (size %d)",
                range->start, range->end, range->boundary_header.len);

        message_len += range->boundary_header.len + (range->end - range->start);
    }

    message_len += sizeof(CRLF "--") - 1 + NGX_ATOMIC_T_LEN + sizeof("--" CRLF) - 1;

    r->headers_out.content_length_n = message_len;

    return NGX_OK;
}

ngx_int_t 
ngx_http_zip_add_partial_content_range(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx)
{
    ngx_table_elt_t      *content_range;
    ngx_http_zip_range_t *range;

    if (ngx_http_zip_add_content_range_header(r) == NGX_ERROR) {
        return NGX_ERROR;
    }

    content_range = r->headers_out.content_range;

    range = &((ngx_http_zip_range_t *)ctx->ranges.elts)[0];

    if (content_range == NULL) {
        return NGX_ERROR;
    }

    content_range->value.data = ngx_palloc(r->pool, 
            sizeof("bytes " /* start */ "-" /* end */ "/" /* total */) - 1
            + 3 * NGX_OFF_T_LEN);
    if (content_range->value.data == NULL) {
        return NGX_ERROR;
    }

    content_range->value.len = ngx_sprintf(content_range->value.data,
            "bytes %O-%O/%O", range->start, range->end - 1,
            r->headers_out.content_length_n)
        - content_range->value.data;

    r->headers_out.content_length_n = range->end - range->start;

    return NGX_OK;
}

ngx_int_t
ngx_http_zip_strip_range_header(ngx_http_request_t *r)
{
    ngx_table_elt_t    *header;

    header = r->headers_in.range;

    if (header) {
        ngx_str_set(&header->key, "X-Range");
        header->lowcase_key = (u_char *)"x-range";
    }

    return NGX_OK;
}

