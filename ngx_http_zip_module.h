/* data structures and useful byte arrays */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <time.h>

#define NGX_ZIP_MIME_TYPE "application/zip"
#define ngx_http_zip_current_file(ctx) ctx->pieces[ctx->pieces_i].file

extern uint32_t   ngx_crc32_table256[];

typedef struct {
    ngx_uint_t  crc32;
    ngx_str_t   uri;
    ngx_str_t   args;
    ngx_uint_t  index;
    ngx_uint_t  dos_time;
    ngx_uint_t  unix_time;
    ngx_str_t   filename;
    ngx_uint_t  size;
    size_t      offset;

    unsigned    header_sent:1;
    unsigned    trailer_sent:1;
} ngx_http_zip_file_t;

typedef struct {
    off_t       start;
    off_t       end;
    ngx_str_t   boundary_header;

    unsigned    boundary_sent:1;
} ngx_http_zip_range_t;

typedef struct {
    ngx_http_zip_range_t    range;
    ngx_http_zip_file_t    *file;
    ngx_int_t               type;
} ngx_http_zip_piece_t;

typedef struct {
    ngx_str_t              *unparsed_request;
    ngx_http_zip_piece_t   *pieces;
    ngx_array_t             files;
    ngx_array_t             ranges;
    ngx_uint_t              ranges_i;
    ngx_uint_t              pieces_i;
    ngx_uint_t              pieces_n;
    ngx_atomic_uint_t       boundary;
    off_t                   archive_size;

    unsigned                parsed:1;
    unsigned                trailer_sent:1;
    unsigned                abort:1;
    unsigned                missing_crc32:1;
    unsigned                missing_size:1;
} ngx_http_zip_ctx_t;

typedef struct {
    ngx_http_zip_file_t    *requesting_file;
    ngx_http_zip_range_t   *range;
    off_t                   subrequest_pos;
} ngx_http_zip_sr_ctx_t;

typedef enum {
    zip_start_state = 0,
    zip_filename_state,
    zip_size_state,
    zip_uri_state,
    zip_args_state,
    zip_eol_state
} ngx_http_zip_state_e;

typedef enum {
    zip_header_piece,
    zip_file_piece,
    zip_trailer_piece,
    zip_central_directory_piece
} ngx_http_zip_piece_e;
