/* data structures and useful byte arrays */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <time.h>

#define NGX_ZIP_MIME_TYPE "application/zip"
#define ngx_http_zip_current_file(ctx) ctx->pieces[ctx->pieces_i].file

extern uint32_t   ngx_crc32_table256[];

typedef struct {
    uint32_t    crc32;
    ngx_str_t   uri;
    ngx_str_t   args;
    size_t      index; //! zip64 allows for 64bit number of files
    ngx_uint_t  dos_time;
    ngx_uint_t  unix_time;
    ngx_str_t   filename;
    ngx_str_t   filename_utf8;
    uint32_t    filename_utf8_crc32;
    off_t       size; 
    off_t       offset;

    unsigned    header_sent:1;
    unsigned    trailer_sent:1;
    unsigned    missing_crc32:1;
    unsigned    need_zip64:1;
    unsigned    need_zip64_offset:1;
} ngx_http_zip_file_t;

typedef struct {
    off_t       start;
    off_t       end;
    ngx_str_t   boundary_header;

    unsigned    boundary_sent:1;
} ngx_http_zip_range_t;

typedef enum {
    zip_header_piece, //local file header
    zip_file_piece, // file data
    zip_trailer_piece, // data descriptor (for files without CRC, exists if bit 3 of GP flag is set),
    zip_trailer_piece64, // the same but for zip64 (if zip64 extended information extra field is in file header)
    zip_central_directory_piece,
    zip_zip64_directory,
    zip_zip64_directory_locator
} ngx_http_zip_piece_e;

typedef struct {
    ngx_http_zip_range_t    range;
    ngx_http_zip_file_t    *file;
    ngx_http_zip_piece_e    type;
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
    off_t                   cd_size; // zip central directory size
    ngx_http_request_t     *wait;

    unsigned                parsed:1;
    unsigned                trailer_sent:1;
    unsigned                abort:1;
    unsigned                missing_crc32:1; // used in subrequest, if true = reads file into memory and calculates it; also to indicate presence of such file
    unsigned                zip64_used:1;
    unsigned                unicode_path:1;
    unsigned                native_charset:1;
} ngx_http_zip_ctx_t;

typedef struct {
    ngx_http_zip_file_t    *requesting_file;
    ngx_http_zip_range_t   *range;
    off_t                   subrequest_pos;
} ngx_http_zip_sr_ctx_t;

