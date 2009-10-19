/* Parser functions */

ngx_int_t ngx_http_zip_parse_request(ngx_http_zip_ctx_t *ctx);
ngx_int_t ngx_http_zip_parse_range(ngx_http_request_t *r, ngx_str_t *range, ngx_http_zip_ctx_t *ctx);
