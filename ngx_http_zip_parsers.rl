/* Ragel Parser definitions for mod_zip64 */

#include "ngx_http_zip_module.h"
#include "ngx_http_zip_parsers.h"

static void
ngx_http_zip_file_init(ngx_http_zip_file_t *parsing_file)
{
    parsing_file->uri.data = NULL;
    parsing_file->uri.len = 0;

    parsing_file->args.data = NULL;
    parsing_file->args.len = 0;

    parsing_file->filename.data = NULL;
    parsing_file->filename.len = 0;
    
    parsing_file->filename_utf8.data = NULL;
    parsing_file->filename_utf8.len = 0;

    parsing_file->header_sent = 0;
    parsing_file->trailer_sent = 0;

    parsing_file->crc32 = 0;
    parsing_file->size = 0;

    parsing_file->missing_crc32 = 0;
    parsing_file->need_zip64 = 0;
    parsing_file->need_zip64_offset = 0;
}

static char 
hex_char_value(unsigned char ch) {
    if ('0' <= ch && ch <= '9')
	return ch - '0';
    if ('A' <= ch && ch <= 'F')
	return ch - 'A' + 10;
    if ('a' <= ch && ch <= 'f')
	return ch - 'A' + 10;
    return 0;	
}

static size_t 
destructive_url_decode_len(unsigned char* start, unsigned char* end)
{
    unsigned char *read_pos = start, *write_pos = start;
    
    for (; read_pos < end; read_pos++) {
	unsigned char ch = *read_pos;
	if (ch == '%' && (read_pos+2 < end)) {
	    ch = 16 * hex_char_value(*(read_pos+1)) + hex_char_value(*(read_pos+2));
	    read_pos += 2;
	    }
	if (ch == '+')
	    ch = ' ';
	*(write_pos++) = ch;
    }
    
    return write_pos - start;
}


static ngx_int_t
ngx_http_zip_clean_range(ngx_http_zip_range_t *range,
        int prefix, int suffix, ngx_http_zip_ctx_t *ctx)
{
    if (suffix) {
        range->end = ctx->archive_size;
        range->start = ctx->archive_size - range->start;
    } else if (prefix) {
        range->end = ctx->archive_size;
    } else {
        range->end++;
        /*
         * Download Accelerator sends the last byte position
         * that equals to the file length
         */
        if (range->end >= ctx->archive_size) {
            range->end = ctx->archive_size;
        }
    }
    if (range->start < 0) {
        return NGX_ERROR;
    }
    if (range->start >= ctx->archive_size) {
        return NGX_ERROR;
    }
    return NGX_OK;
}

%%{
    machine request;
    write data;
}%%

ngx_int_t 
ngx_http_zip_parse_request(ngx_http_zip_ctx_t *ctx)
{
    int cs;
    u_char *p = ctx->unparsed_request->data;
    u_char *pe = ctx->unparsed_request->data + ctx->unparsed_request->len;
    ngx_http_zip_file_t *parsing_file = NULL;

    %%{

        action start_file {
            parsing_file = ngx_array_push(&ctx->files);
            ngx_http_zip_file_init(parsing_file);

            parsing_file->index = ctx->files.nelts - 1;
        }

        action start_uri {
            parsing_file->uri.data = fpc;
            parsing_file->uri.len = 1;
        }

        action end_uri {
	    parsing_file->uri.len = destructive_url_decode_len(parsing_file->uri.data, fpc);
        }
        action start_args {
            parsing_file->args.data = fpc;
        }
        action end_args {
            parsing_file->args.len = fpc - parsing_file->args.data;
        }
        action size_incr {
            parsing_file->size = parsing_file->size * 10 + (fc - '0');
        }
        action crc_incr {
            if (fc == '-') {
                ctx->missing_crc32 = 1;
                parsing_file->missing_crc32 = 1;
                parsing_file->crc32 = 0xffffffff;
            } else {
                parsing_file->crc32 *= 16;
                if (fc >= 'a' && fc <= 'f') {
                    parsing_file->crc32 += fc - 'a' + 10;
                }
                else if (fc >= 'A' && fc <= 'F') {
                    parsing_file->crc32 += fc - 'A' + 10;
                } else { /* 0-9 */
                    parsing_file->crc32 += fc - '0';
                }
            }
        }
        action start_filename {
            parsing_file->filename.data = fpc;
        }
        action end_filename {
            parsing_file->filename.len = fpc - parsing_file->filename.data;
        }

        main := (
                  ( [0-9a-fA-F]+ | "-" ) >start_file $crc_incr
                  " "+
                  [0-9]+ $size_incr
                  " "+
                  [^? ]+ >start_uri %end_uri
                  ( "?" [^ ]+ >start_args %end_args )?
                  " "+
                  [^ ] >start_filename
                  [^\r\n\0]* %end_filename
                  [\r\n]+
                )+;

       write init;
       write exec;
    }%%

    if (cs < request_first_final) {
        return NGX_ERROR;
    }

    ctx->parsed = 1;

    return NGX_OK;
}

%%{
    machine range;
    write data;
}%%

ngx_int_t
ngx_http_zip_parse_range(ngx_http_request_t *r, ngx_str_t *range_str, ngx_http_zip_ctx_t *ctx)
{
    int cs, prefix = 0, suffix = 0;

    ngx_http_zip_range_t *range = NULL;
    u_char *p = range_str->data;
    u_char *pe = range_str->data + range_str->len;

    %%{
        action new_range {
            if (range) {
                if (ngx_http_zip_clean_range(range, prefix, suffix, ctx) == NGX_ERROR) {
                    return NGX_ERROR;
                }
            }
            if ((range = ngx_array_push(&ctx->ranges)) == NULL) {
                return NGX_ERROR;
            }
            range->start = 0; range->end = 0; range->boundary_sent = 0;
            suffix = 0;
            prefix = 1;
        }

        action start_incr { range->start = range->start * 10 + (fc - '0'); }

        action end_incr { range->end = range->end * 10 + (fc - '0'); prefix = 0; }

        action suffix { suffix = 1; }

        suffix_byte_range_spec = "-" [0-9]+ $start_incr >suffix;
        byte_range_spec = [0-9]+ $start_incr
                          "-" 
                          [0-9]* $end_incr;
        byte_range_specs = ( byte_range_spec | suffix_byte_range_spec ) >new_range;
        byte_range_set = byte_range_specs ( "," byte_range_specs )*;

        main := "bytes=" byte_range_set;

      write init;
      write exec;
    }%%

    if (cs < range_first_final) {
        return NGX_ERROR;
    }

    if (range) {
        if (ngx_http_zip_clean_range(range, prefix, suffix, ctx) == NGX_ERROR) {
            return NGX_ERROR;
        }
    }

    return NGX_OK;
}
