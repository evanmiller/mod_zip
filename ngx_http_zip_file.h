ngx_chain_t *ngx_http_zip_file_header_chain_link(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range);
ngx_chain_t *ngx_http_zip_data_descriptor_chain_link(ngx_http_request_t *r,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range);
ngx_chain_t *ngx_http_zip_central_directory_chain_link(ngx_http_request_t *r, 
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range);
u_char *ngx_http_zip_write_central_directory_entry(u_char *p, 
        ngx_http_zip_file_t *file, ngx_http_zip_ctx_t *ctx);
off_t ngx_http_zip_calculate_central_directory_size(off_t files_n,
        off_t filename_s);
ngx_int_t ngx_http_zip_generate_pieces(ngx_http_request_t *r,
        ngx_http_zip_ctx_t *ctx);
