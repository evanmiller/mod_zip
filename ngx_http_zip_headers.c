#include "ngx_http_zip_module.h"
#include "ngx_http_zip_headers.h"
#include "ngx_http_zip_file.h"

static ngx_uint_t
ngx_http_zip_find_key_in_set(ngx_str_t *key, ngx_array_t *set)
{
    ngx_uint_t i;
    ngx_str_t  *items = set->elts;

    for (i = 0; i < set->nelts; ++i) {
        if (items[i].len == key->len
                && !ngx_rstrncasecmp(items[i].data, key->data, key->len)) {
            return 1;
        }
    }

    return 0;
}

ngx_int_t 
ngx_http_zip_add_cache_control(ngx_http_request_t *r)
{
#ifdef NGX_ZIP_MULTI_HEADERS_LINKED_LISTS
    ngx_table_elt_t            *cc;

    /* convoluted way of adding Cache-Control: max-age=0 */
    /* The header is necessary so IE doesn't barf */
    cc = r->headers_out.cache_control;

    if (cc == NULL) {
        cc = ngx_list_push(&r->headers_out.headers);
        if (cc == NULL) {
            return NGX_ERROR;
        }

        r->headers_out.cache_control = cc;
        cc->next = NULL;
        cc->hash = 1;
        ngx_str_set(&cc->key, "Cache-Control");
    } else {
         for (cc = cc->next; cc; cc = cc->next) {
            cc->hash = 0;
         }

         cc = r->headers_out.cache_control;
         cc->next = NULL;
    }

    ngx_str_set(&cc->value, "max-age=0");

    return NGX_OK;
#else
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
#endif
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

ngx_int_t
ngx_http_zip_init_subrequest_headers(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_request_t *sr, ngx_http_zip_range_t *piece_range,
        ngx_http_zip_range_t *req_range)
{
    ngx_list_t new_headers;

    if (ngx_list_init(&new_headers, r->pool, 1, sizeof(ngx_table_elt_t)) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ctx->pass_srq_headers.nelts) {
        // Pass original header fileds, that appear on the list.
        ngx_list_part_t *next_part;
        ngx_table_elt_t *h;
        ngx_uint_t      i;

        next_part = &sr->headers_in.headers.part;

        for (; next_part; next_part = next_part->next) {
            h = next_part->elts;

            for (i = 0; i < next_part->nelts; ++i) {
                if (ngx_http_zip_find_key_in_set(&h[i].key, &ctx->pass_srq_headers)) {
                    ngx_memcpy(ngx_list_push(&new_headers), &h[i], sizeof(ngx_table_elt_t));
                }
            }
        }
    }

    ngx_memzero(&sr->headers_in, sizeof(sr->headers_in));
    ngx_memcpy(&sr->headers_in.headers, &new_headers, sizeof(new_headers));
    sr->headers_in.content_length_n = -1;
    sr->headers_in.keep_alive_n = -1;

    if (req_range && (piece_range->start < req_range->start || piece_range->end > req_range->end)) {
        ngx_table_elt_t *range_header = ngx_list_push(&sr->headers_in.headers);
        off_t start = req_range->start - piece_range->start;
        off_t end = req_range->end - piece_range->start;

        if (start < 0)
            start = 0;
        if (end > piece_range->end)
            end = piece_range->end;

        if (range_header == NULL)
            return NGX_ERROR;

        range_header->value.data = ngx_pnalloc(r->pool, sizeof("bytes=-") + 2 * NGX_OFF_T_LEN);
        if (range_header->value.data == NULL)
            return NGX_ERROR;

        range_header->value.len = ngx_sprintf(range_header->value.data, "bytes=%O-%O", start, end-1)
            - range_header->value.data;
        range_header->value.data[range_header->value.len] = '\0';

        range_header->hash = 1;
        ngx_str_set(&range_header->key, "Range");

        sr->headers_in.range = range_header;
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_zip_variable_unknown_header(ngx_http_request_t *r,
                                 ngx_http_variable_value_t *v, ngx_str_t *var,
                                 ngx_list_part_t *part, size_t prefix)
{
#ifdef NGX_ZIP_MULTI_HEADERS_LINKED_LISTS
    return ngx_http_variable_unknown_header(r, v, var, part, prefix);
#else
    return ngx_http_variable_unknown_header(v, var, part, prefix);
#endif
}
