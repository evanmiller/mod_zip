
#include "ngx_http_zip_module.h"
#include "ngx_http_zip_file.h"
#include "ngx_http_zip_file_format.h"
#include "ngx_http_zip_endian.h"

#ifdef NGX_ZIP_HAVE_ICONV
#include <iconv.h>
#endif

#ifdef NGX_ZIP_HAVE_ICONV
static ngx_str_t ngx_http_zip_header_charset_name = ngx_string("upstream_http_x_archive_charset");
#endif
static ngx_str_t ngx_http_zip_header_name_separator = ngx_string("upstream_http_x_archive_name_sep");

#define NGX_MAX_UINT16_VALUE 0xffff


// Chunk templates for fast struct init:

static ngx_zip_extra_field_local_t ngx_zip_extra_field_local_template = {
    0x5455,             /* tag for this extra block type ("UT") */
    sizeof(ngx_zip_extra_field_local_t) - 4,
                        /* total data size for this block */
    0x03,               /* info bits */
    0,                  /* modification time */
    0,                  /* access time */
};

static ngx_zip_extra_field_central_t ngx_zip_extra_field_central_template = {
    0x5455,         /* tag for this extra block type ("UT") */
    sizeof(ngx_zip_extra_field_central_t) - 4,
                    /* total data size for this block */
    0x03,           /* info bits */
    0,              /* modification time */
};

static ngx_zip_extra_field_unicode_path_t ngx_zip_extra_field_unicode_path_template = {
    0x7075,         /* Info-ZIP Unicode Path tag */
    0,
    1,              /* version of this extra field, currently 1 (c) */
    0,              /* crc-32 */
};

static ngx_zip_extra_field_zip64_sizes_only_t ngx_zip_extra_field_zip64_sizes_only_template = {
    0x0001, //tag for zip64 extra field
    sizeof(ngx_zip_extra_field_zip64_sizes_only_t) - 4,
    0,
    0,
};

static ngx_zip_extra_field_zip64_offset_only_t ngx_zip_extra_field_zip64_offset_only_template = {
    0x0001, //tag for zip64 extra field
    sizeof(ngx_zip_extra_field_zip64_offset_only_t) - 4,
    0,
};

static ngx_zip_extra_field_zip64_sizes_offset_t ngx_zip_extra_field_zip64_sizes_offset_template = {
    0x0001, //tag for zip64 extra field
    sizeof(ngx_zip_extra_field_zip64_sizes_offset_t) - 4,
    0,
    0,
    0
};

static ngx_zip_data_descriptor_t ngx_zip_data_descriptor_template = {
    0x08074b50,  /* data descriptor signature */
    0,           /* crc-32 */
    0,           /* compressed size */
    0            /* uncompressed size */
};

static ngx_zip_data_descriptor_zip64_t ngx_zip_data_descriptor_zip64_template = {
    0x08074b50,  /* data descriptor signature */
    0,           /* crc-32 */
    0,           /* compressed size */
    0            /* uncompressed size */
};

static ngx_zip_local_file_header_t ngx_zip_local_file_header_template = {
    0x04034b50,  /* local file header signature */
    0x0a,        /* version needed to extract */
    zip_utf8_flag | zip_missing_crc32_flag,        /* general purpose bit flag */
    0,           /* compression method */
    0,           /* last mod file date/time */
    0,           /* crc-32 */
    0xffffffff,           /* compressed size */
    0xffffffff,           /* uncompressed size */
    0,           /* file name length */
    sizeof(ngx_zip_extra_field_local_t),
                 /* extra field length */
};

static ngx_zip_central_directory_file_header_t ngx_zip_central_directory_file_header_template = {
    0x02014b50,  /* central file header signature */
    zip_version_zip64,      /* version made by */
    zip_version_default,        /* version needed to extract */
    zip_utf8_flag | zip_missing_crc32_flag,        /* general purpose bit flag */
    0,           /* compression method */
    0,           /* last mod file time */
    0,           /* crc-32 */
    0xffffffff,           /* compressed size */
    0xffffffff,           /* uncompressed size */
    0,           /* file name length */
    sizeof(ngx_zip_extra_field_central_t),
                 /* extra field length */
    0,           /* file comment length */
    0,           /* disk number start */
    0,           /* internal file attributes */
    0x81a4000,  /* external file attributes */
    0xffffffff   /* relative offset of local header */
};

static ngx_zip_end_of_central_directory_record_t ngx_zip_end_of_central_directory_record_template = {
    0x06054b50,  /* end of central dir signature */
    0,           /* number of this disk */
    0,           /* number of the disk with the start of the central directory */
    0xffff,           /* total number of entries in the central directory on this disk */
    0xffff,           /* total number of entries in the central directory */
    0xFFFFFFFF,           /* size of the central directory */
    0xffffffff,           /* offset of start of central directory w.r.t. starting disk # */
    0            /* .ZIP file comment length */
};

static ngx_zip_zip64_end_of_central_directory_record_t ngx_zip_zip64_end_of_central_directory_record_template = {
    0x06064b50, //signature for zip64 EOCD
    sizeof(ngx_zip_zip64_end_of_central_directory_record_t)-12, //size of this record (+variable fields, but minus signature and this size field), Size = SizeOfFixedFields + SizeOfVariableData - 12
    zip_version_zip64, //created by
    zip_version_zip64, //needed
    0, //this disk number
    0, // num of disk with start of CD
    0,
    0,
    0,
    0, // cd offset with respect to starting disk number
};

static ngx_zip_zip64_end_of_central_directory_locator_t ngx_zip_zip64_end_of_central_directory_locator_template = {
    0x07064b50, //signature
    0, // number of disk with start of zip64 end of central directory
    0, // offset of central directory
    1 //dics number total
};

//-----------------------------------------------------------------------------------------------------------


// Convert UNIX timestamp to DOS timestamp
static ngx_uint_t ngx_dos_time(time_t t)
{
    ngx_tm_t tm;
    ngx_gmtime(t, &tm); // ngx_gmtime does the mon++ and year += 1900 for us

    return (tm.ngx_tm_sec >> 1)
        + (tm.ngx_tm_min << 5)
        + (tm.ngx_tm_hour << 11)
        + (tm.ngx_tm_mday << 16)
        + (tm.ngx_tm_mon << 21)
        + ((tm.ngx_tm_year-1980) << 25);
}

static void
ngx_http_zip_truncate_buffer(ngx_buf_t *b,
        ngx_http_zip_range_t *piece_range, ngx_http_zip_range_t *req_range)
{
    if (req_range && piece_range && b) {
        if (req_range->end < piece_range->end)
            b->last -= piece_range->end - req_range->end;
        if (req_range->start > piece_range->start)
            b->pos += req_range->start - piece_range->start;
    }
}

static const char *
ngx_http_zip_strnrstr(const char * str, ngx_uint_t n,
                     const char * sub_str, ngx_uint_t sub_n)
{
    ngx_int_t max_shift = n - sub_n;
    ngx_uint_t i;
    for(; max_shift >= 0; --max_shift) {
        for(i = 0; i <= n; ++i) {
            if(i == sub_n)
                return &str[max_shift];
            else if(str[max_shift + i] != sub_str[i])
                break;
        }
    }
    return NULL;
}

#ifndef ICONV_CSNMAXLEN
#define ICONV_CSNMAXLEN 64
#endif

// make our proposed ZIP-file chunk map
ngx_int_t
ngx_http_zip_generate_pieces(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx)
{
    ngx_uint_t i, piece_i;
    off_t offset = 0;
    time_t unix_time = 0;
    ngx_uint_t dos_time = 0;
    ngx_http_zip_file_t  *file;
    ngx_http_zip_piece_t *header_piece, *file_piece, *trailer_piece, *cd_piece;
    ngx_http_variable_value_t  *vv;

    if ((vv = ngx_palloc(r->pool, sizeof(ngx_http_variable_value_t))) == NULL)
        return NGX_ERROR;

    ctx->unicode_path = 0;
#ifdef NGX_ZIP_HAVE_ICONV
    iconv_t *iconv_cd = NULL;
#endif

    // Let's try to find special header that contains separator string.
    // What for this strange separator string you ask?
    // Sometimes there might be a problem converting UTF-8 to zips native
    // charset(CP866), because it's not 1:1 conversion. So my solution is to
    // developers provide their own version of converted filename and pass it
    // to mod_zip along with UTF-8 filename which will go straight to Unicode
    // path extra field (thanks to tony2001). So separator is a solution that doesn't
    // break current format. And allows passing file name in both formats as one string.
    //
    // Normally we pass:
    // CRC32 <size> <path> <filename>\n
    // ...
    // * <filename> passed to archive as filename w/o conversion
    // * UFT-8 flag for filename is set
    //
    // tony2001's X-Archive-Charset: <charset> way:
    // CRC32 <size> <path> <filename>\n
    // ...
    // * <filename> is accepted to be UTF-8 string
    // * <filename>, converted to <charset> and passed to archive as filename
    // * <filename> passed to Unicode path extra field
    // * UFT-8 flag for filename is not set
    //
    // My X-Archive-Name-Sep: <sep> solution:
    // CRC32 <size> <path> <native-filename><sep><utf8-filename>\n
    // ...
    // * <native-filename> passed to archive as filename w/o conversion
    // * <utf8-filename> passed to Unicode path extra field
    // * UFT-8 flag for filename is not set
    //
    // You just need to provide separator that won't interfere with file names. I suggest using '/'
    // as it is ASCII character and forbidden on most (if not all) platforms as a part of filename.
    //
    // Empty separator string means no UTF-8 version provided. Usefull when we need to pass only
    // names encoded in native charset. It's equal to 'X-Archive-Charset: native;'.
    // Note: Currently it is impossible after '[PATCH] Support for UTF-8 file names.'(4f61592b)
    // because UFT-8 flag (zip_utf8_flag) is set default for templates.

    if(ngx_http_upstream_header_variable(r, vv, (uintptr_t)(&ngx_http_zip_header_name_separator)) == NGX_OK && !vv->not_found) {
        ctx->native_charset = 1;
        if(vv->len)
            ctx->unicode_path = 1;
    } else {
#ifdef NGX_ZIP_HAVE_ICONV
        if (ngx_http_upstream_header_variable(r, vv, (uintptr_t)(&ngx_http_zip_header_charset_name)) == NGX_OK
                && !vv->not_found && ngx_strncmp(vv->data, "utf8", sizeof("utf8") - 1) != 0) {

            if(ngx_strncmp(vv->data, "native", sizeof("native") - 1))
            {
                char encoding[ICONV_CSNMAXLEN];
                snprintf(encoding, sizeof(encoding), "%s//TRANSLIT//IGNORE", vv->data);

                iconv_cd = iconv_open((const char *)encoding, "utf-8");
                if (iconv_cd == (iconv_t)(-1)) {
                    ngx_log_error(NGX_LOG_WARN, r->connection->log, errno,
                                  "mod_zip: iconv_open('%s', 'utf-8') failed",
                                  vv->data);
                    iconv_cd = NULL;
                }
                else
                {
                    ctx->unicode_path = 1;
                    ctx->native_charset = 1;
                }
            }
            else
                ctx->native_charset = 1;
        }
#endif
    }

    // pieces: for each file: header, data, footer (if needed) -> 2 or 3 per file
    // plus file footer (CD + [zip64 end + zip64 locator +] end of cd) in one chunk
    ctx->pieces_n = ctx->files.nelts * (2 + (!!ctx->missing_crc32)) + 1;

    if ((ctx->pieces = ngx_palloc(r->pool, sizeof(ngx_http_zip_piece_t) * ctx->pieces_n)) == NULL)
        return NGX_ERROR;

    ctx->cd_size = 0;
    unix_time = time(NULL);
    dos_time = ngx_dos_time(unix_time);
    for (piece_i = i = 0; i < ctx->files.nelts; i++) {
        file = &((ngx_http_zip_file_t *)ctx->files.elts)[i];
        file->offset = offset;
        file->unix_time = unix_time;
        file->dos_time = dos_time;

        if(ctx->unicode_path) {
#ifdef NGX_ZIP_HAVE_ICONV
            if (iconv_cd) {
                size_t inlen = file->filename.len, outlen, outleft;
                u_char *p, *in;

                //inbuf
                file->filename_utf8.data = ngx_pnalloc(r->pool, file->filename.len + 1);
                ngx_memcpy(file->filename_utf8.data, file->filename.data, file->filename.len);
                file->filename_utf8.len = file->filename.len;
                file->filename_utf8.data[file->filename.len] = '\0';

                //outbuf
                outlen = outleft = inlen * sizeof(int) + 15;
                file->filename.data = ngx_pnalloc(r->pool, outlen + 1);

                in = file->filename_utf8.data;
                p = file->filename.data;

                //reset state
                iconv(iconv_cd, NULL, NULL, NULL, NULL);

                //convert the string
                iconv(iconv_cd, (char **)&in, &inlen, (char **)&p, &outleft);
                //XXX if (res == (size_t)-1) { ? }

                file->filename.len = outlen - outleft;

                file->filename_utf8_crc32 = ngx_crc32_long(file->filename_utf8.data, file->filename_utf8.len);
            }
            else
#endif
              if(vv->len) {
                const char * sep = ngx_http_zip_strnrstr((const char*)file->filename.data, file->filename.len,
                                                         (const char*)vv->data, vv->len);
                if(sep) {
                    size_t utf8_len = file->filename.len - vv->len - (size_t)(sep - (const char *)file->filename.data);
                    file->filename_utf8.data = ngx_pnalloc(r->pool, utf8_len);
                    file->filename_utf8.len = utf8_len;
                    ngx_memcpy(file->filename_utf8.data, sep + vv->len, utf8_len);

                    file->filename.len -= utf8_len + vv->len;
                    file->filename_utf8_crc32 = ngx_crc32_long(file->filename_utf8.data, file->filename_utf8.len);
                } /* else { } */    // Separator not found. Okay, no extra field for this one then.
            }
        }

        if(offset >= (off_t) NGX_MAX_UINT32_VALUE)
            ctx->zip64_used = file->need_zip64_offset = 1;
        if(file->size >= (off_t) NGX_MAX_UINT32_VALUE)
            ctx->zip64_used = file->need_zip64 = 1;

        ctx->cd_size += sizeof(ngx_zip_central_directory_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_central_t)
            + (file->need_zip64_offset ?
                    (file->need_zip64 ? sizeof(ngx_zip_extra_field_zip64_sizes_offset_t) : sizeof(ngx_zip_extra_field_zip64_offset_only_t)) :
                    (file->need_zip64 ? sizeof(ngx_zip_extra_field_zip64_sizes_only_t) : 0) +
                    (ctx->unicode_path && file->filename_utf8.len ? (sizeof(ngx_zip_extra_field_unicode_path_t) + file->filename_utf8.len): 0)
              );

        header_piece = &ctx->pieces[piece_i++];
        header_piece->type = zip_header_piece;
        header_piece->file = file;
        header_piece->range.start = offset;
        header_piece->range.end = offset += sizeof(ngx_zip_local_file_header_t)
            + file->filename.len + sizeof(ngx_zip_extra_field_local_t) + (file->need_zip64? sizeof(ngx_zip_extra_field_zip64_sizes_only_t):0)
            + (ctx->unicode_path && file->filename_utf8.len ? (sizeof(ngx_zip_extra_field_unicode_path_t) + file->filename_utf8.len): 0);

        file_piece = &ctx->pieces[piece_i++];
        file_piece->type = zip_file_piece;
        file_piece->file = file;
        file_piece->range.start = offset;
        file_piece->range.end = offset += file->size; //!note: (sizeless chunks): we need file size here / or mark it and modify ranges after

        if (file->missing_crc32) { // if incomplete header -> add footer with that info to file
            trailer_piece = &ctx->pieces[piece_i++];
            trailer_piece->type = zip_trailer_piece;
            trailer_piece->file = file;
            trailer_piece->range.start = offset;
            trailer_piece->range.end = offset += file->need_zip64? sizeof(ngx_zip_data_descriptor_zip64_t) : sizeof(ngx_zip_data_descriptor_t);
            //!!TODO: if we want Ranges support - here we know it is impossible for this set
            //? check conf/some state and abort?
        }
    }

#ifdef NGX_ZIP_HAVE_ICONV
    if (iconv_cd) {
        iconv_close(iconv_cd);
    }
#endif

    ctx->zip64_used |= offset >= (off_t) NGX_MAX_UINT32_VALUE || ctx->files.nelts >= NGX_MAX_UINT16_VALUE;

    ctx->cd_size += sizeof(ngx_zip_end_of_central_directory_record_t);
    if (ctx->zip64_used)
        ctx->cd_size += sizeof(ngx_zip_zip64_end_of_central_directory_record_t) + sizeof(ngx_zip_zip64_end_of_central_directory_locator_t);


    cd_piece = &ctx->pieces[piece_i++];
    cd_piece->type = zip_central_directory_piece;
    cd_piece->range.start = offset;
    cd_piece->range.end = offset += ctx->cd_size;

    ctx->pieces_n = piece_i; //!! nasty hack (truncating allocated array without reallocation)

    ctx->archive_size = offset;

    return NGX_OK;
}

// make Local File Header chunk with extra fields
ngx_chain_t*
ngx_http_zip_file_header_chain_link(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range)
{
    ngx_chain_t *link;
    ngx_buf_t   *b;

    ngx_http_zip_file_t *file = piece->file;
    ngx_zip_extra_field_local_t   extra_field_local;
    ngx_zip_extra_field_zip64_sizes_only_t extra_field_zip64;
    ngx_zip_local_file_header_t   local_file_header;
    ngx_zip_extra_field_unicode_path_t extra_field_unicode_path;

    size_t len = sizeof(ngx_zip_local_file_header_t) + file->filename.len
        + sizeof(ngx_zip_extra_field_local_t) + (file->need_zip64? sizeof(ngx_zip_extra_field_zip64_sizes_only_t):0
        + (ctx->unicode_path && file->filename_utf8.len ? (sizeof(ngx_zip_extra_field_unicode_path_t) + file->filename_utf8.len): 0));

    if ((link = ngx_alloc_chain_link(r->pool)) == NULL || (b = ngx_calloc_buf(r->pool)) == NULL
            || (b->pos = ngx_pcalloc(r->pool, len)) == NULL)
        return NULL;

    b->memory = 1;
    b->last = b->pos + len;
#if (NGX_HTTP_SSL)
    b->flush = !!r->connection->ssl;
#endif

    /* A note about the ZIP format: in order to appease all ZIP software I
     * could find, the local file header contains the file sizes but not the
     * CRC-32, even though setting the third bit of the general purpose bit
     * flag would indicate that all three fields should be zeroed out.
     */

    local_file_header = ngx_zip_local_file_header_template;
    local_file_header.signature = htole32(local_file_header.signature);
    local_file_header.version = htole16(local_file_header.version);
    local_file_header.flags = htole16(local_file_header.flags);
    local_file_header.mtime = htole32(file->dos_time);
    local_file_header.filename_len = htole16(file->filename.len);
    if (ctx->native_charset) {
        local_file_header.flags &= htole16(~zip_utf8_flag);
    }
    extra_field_zip64 = ngx_zip_extra_field_zip64_sizes_only_template;
    extra_field_zip64.tag = htole16(extra_field_zip64.tag);
    extra_field_zip64.size = htole16(extra_field_zip64.size);

    if (file->need_zip64) {
        local_file_header.version = htole16(zip_version_zip64);
        local_file_header.extra_field_len = sizeof(ngx_zip_extra_field_zip64_sizes_only_t) + sizeof(ngx_zip_extra_field_local_t);
        extra_field_zip64.uncompressed_size = extra_field_zip64.compressed_size = htole64(file->size);
    } else {
        local_file_header.compressed_size = htole32(file->size);
        local_file_header.uncompressed_size = htole32(file->size);
    }

    extra_field_unicode_path = ngx_zip_extra_field_unicode_path_template;
    extra_field_unicode_path.tag = htole16(extra_field_unicode_path.tag);

    if (ctx->unicode_path && file->filename_utf8.len) {
        extra_field_unicode_path.crc32 = htole32(file->filename_utf8_crc32);
        extra_field_unicode_path.size = htole16(sizeof(ngx_zip_extra_field_unicode_path_t) + file->filename_utf8.len);

        local_file_header.extra_field_len += sizeof(ngx_zip_extra_field_unicode_path_t) + file->filename_utf8.len;
    }
    local_file_header.extra_field_len = htole16(local_file_header.extra_field_len);

    if (!file->missing_crc32) {
        local_file_header.flags &= htole16(~zip_missing_crc32_flag);
        local_file_header.crc32 = htole32(file->crc32);
    }

    extra_field_local = ngx_zip_extra_field_local_template;
    extra_field_local.tag = htole16(extra_field_local.tag);
    extra_field_local.size = htole16(extra_field_local.size);
    extra_field_local.mtime = htole32(file->unix_time);
    extra_field_local.atime = htole32(file->unix_time);

    ngx_memcpy(b->pos, &local_file_header, sizeof(ngx_zip_local_file_header_t));
    ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t), file->filename.data, file->filename.len);
    ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len,
            &extra_field_local, sizeof(ngx_zip_extra_field_local_t));
    if (file->need_zip64) {
        ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_local_t),
                &extra_field_zip64, sizeof(ngx_zip_extra_field_zip64_sizes_only_t));

        if (ctx->unicode_path && file->filename_utf8.len) {
            ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_local_t) + sizeof(ngx_zip_extra_field_zip64_sizes_only_t), &extra_field_unicode_path, sizeof(ngx_zip_extra_field_unicode_path_t));
            ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_local_t) + sizeof(ngx_zip_extra_field_zip64_sizes_only_t) + sizeof(ngx_zip_extra_field_unicode_path_t), file->filename_utf8.data, file->filename_utf8.len);
        }
    } else if (ctx->unicode_path && file->filename_utf8.len) {
        ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_local_t), &extra_field_unicode_path, sizeof(ngx_zip_extra_field_unicode_path_t));
        ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_local_t) + sizeof(ngx_zip_extra_field_unicode_path_t), file->filename_utf8.data, file->filename_utf8.len);
    }

    ngx_http_zip_truncate_buffer(b, &piece->range, range);

    link->buf = b;
    link->next = NULL;

    return link;
}


// make buffer with 32/64 bit Data Descriptor chunk, this follows files with incomplete headers
ngx_chain_t*
ngx_http_zip_data_descriptor_chain_link(ngx_http_request_t *r, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range)
{
    ngx_chain_t *link;
    ngx_buf_t   *b;
    ngx_http_zip_file_t *file = piece->file;
    size_t struct_size = file->need_zip64? sizeof(ngx_zip_data_descriptor_zip64_t) : sizeof(ngx_zip_data_descriptor_t);
    union {
        ngx_zip_data_descriptor_t  descriptor;
        ngx_zip_data_descriptor_zip64_t  descriptor64;
    } data;

    if ((link = ngx_alloc_chain_link(r->pool)) == NULL || (b = ngx_calloc_buf(r->pool)) == NULL
            || (b->pos = ngx_palloc(r->pool, struct_size)) == NULL)
        return NULL;
    b->memory = 1;
    b->last = b->pos + struct_size;

    if (!file->need_zip64) {
        data.descriptor = ngx_zip_data_descriptor_template;
        data.descriptor.signature = htole32(data.descriptor.signature);
        data.descriptor.crc32 = htole32(file->crc32);
        data.descriptor.compressed_size = data.descriptor.uncompressed_size = htole32(file->size);
    } else {
        data.descriptor64 = ngx_zip_data_descriptor_zip64_template;
        data.descriptor64.signature = htole32(data.descriptor64.signature);
        data.descriptor64.crc32 = htole32(file->crc32);
        data.descriptor64.compressed_size = data.descriptor64.uncompressed_size = htole64(file->size);
    }

    ngx_memcpy(b->pos, &data, struct_size);
    ngx_http_zip_truncate_buffer(b, &piece->range, range);

    link->buf = b;
    link->next = NULL;

    return link;
}


//make archive footer: Central Directory, Zip64 Central Directory End, Zip64 locator and Central Directory end chunks
ngx_chain_t *
ngx_http_zip_central_directory_chain_link(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range)
{
    //nb: this is to be called only after 'generate pieces'
    ngx_chain_t           *trailer;
    ngx_buf_t             *trailer_buf;
    u_char                *p;
    off_t                  cd_size;
    ngx_uint_t             i;
    ngx_array_t           *files;
    ngx_zip_end_of_central_directory_record_t  eocdr;
    ngx_zip_zip64_end_of_central_directory_record_t eocdr64;
    ngx_zip_zip64_end_of_central_directory_locator_t locator64;

    if (!ctx || !ctx->cd_size || (trailer = ngx_alloc_chain_link(r->pool)) == NULL
            || (trailer_buf = ngx_calloc_buf(r->pool)) == NULL
            || (p = ngx_palloc(r->pool, ctx->cd_size)) == NULL)
        return NULL;

    files = &ctx->files;
    trailer->buf = trailer_buf;
    trailer->next = NULL;

    trailer_buf->pos = p;
    trailer_buf->last = p + ctx->cd_size;
    trailer_buf->last_buf = 1;
    trailer_buf->sync = 1;
    trailer_buf->memory = 1;

    for (i = 0; i < files->nelts; i++)
        p = ngx_http_zip_write_central_directory_entry(p, &((ngx_http_zip_file_t *)files->elts)[i], ctx);

    eocdr = ngx_zip_end_of_central_directory_record_template;
    eocdr.signature = htole32(eocdr.signature);
    if (files->nelts < NGX_MAX_UINT16_VALUE) {
        eocdr.disk_entries_n = htole16(files->nelts);
        eocdr.entries_n = htole16(files->nelts);
    }

    cd_size = ctx->cd_size - sizeof(ngx_zip_end_of_central_directory_record_t)
        - (!!ctx->zip64_used)*(sizeof(ngx_zip_zip64_end_of_central_directory_record_t)
                + sizeof(ngx_zip_zip64_end_of_central_directory_locator_t));

    if (cd_size < (off_t) NGX_MAX_UINT32_VALUE)
        eocdr.size = htole32(cd_size);
    if (piece->range.start < (off_t) NGX_MAX_UINT32_VALUE)
        eocdr.offset = htole32(piece->range.start);

    if (ctx->zip64_used) {
        eocdr64 = ngx_zip_zip64_end_of_central_directory_record_template;
        eocdr64.signature = htole32(eocdr64.signature);
        eocdr64.size = htole64(eocdr64.size);
        eocdr64.version_made_by = htole16(eocdr64.version_made_by);
        eocdr64.version_needed = htole16(eocdr64.version_made_by);

        eocdr64.cd_n_entries_on_this_disk = eocdr64.cd_n_entries_total = htole64(files->nelts);
        eocdr64.cd_size = htole64(cd_size);
        eocdr64.cd_offset = htole64(piece->range.start);

        ngx_memcpy(p, &eocdr64, sizeof(ngx_zip_zip64_end_of_central_directory_record_t));
        p += sizeof(ngx_zip_zip64_end_of_central_directory_record_t);

        locator64 = ngx_zip_zip64_end_of_central_directory_locator_template;
        locator64.signature = htole32(locator64.signature);
        locator64.disks_total_n = htole32(locator64.disks_total_n);
        locator64.cd_relative_offset = htole64(piece->range.start + cd_size);
        ngx_memcpy(p, &locator64, sizeof(ngx_zip_zip64_end_of_central_directory_locator_t));
        p += sizeof(ngx_zip_zip64_end_of_central_directory_locator_t);
    }

    ngx_memcpy(p, &eocdr, sizeof(ngx_zip_end_of_central_directory_record_t));

    ngx_http_zip_truncate_buffer(trailer->buf, &piece->range, range);
    return trailer;
}


u_char *
ngx_http_zip_write_central_directory_entry(u_char *p, ngx_http_zip_file_t *file,
        ngx_http_zip_ctx_t *ctx)
{
    ngx_zip_extra_field_central_t            extra_field_central;
    ngx_zip_central_directory_file_header_t  central_directory_file_header;
    ngx_zip_extra_field_zip64_offset_only_t extra_zip64_offset;
    ngx_zip_extra_field_zip64_sizes_offset_t extra_zip64_offset_size;
    ngx_zip_extra_field_zip64_sizes_only_t extra_zip64_size;
    ngx_zip_extra_field_unicode_path_t extra_field_unicode_path;
    void* extra_zip64_ptr = NULL; //!!
    size_t extra_zip64_ptr_size = 0;

    central_directory_file_header = ngx_zip_central_directory_file_header_template;
    central_directory_file_header.signature = htole32(central_directory_file_header.signature);
    central_directory_file_header.version_made_by = htole16(central_directory_file_header.version_made_by);
    central_directory_file_header.version_needed = htole16(central_directory_file_header.version_needed);
    central_directory_file_header.flags = htole16(central_directory_file_header.flags);
    central_directory_file_header.attr_external = htole32(central_directory_file_header.attr_external);
    central_directory_file_header.mtime = htole32(file->dos_time);
    central_directory_file_header.crc32 = htole32(file->crc32);

    if (ctx->native_charset) {
        central_directory_file_header.flags &= htole16(~zip_utf8_flag);
    }

    if (!file->need_zip64) {
        central_directory_file_header.compressed_size = htole32(file->size);
        central_directory_file_header.uncompressed_size = htole32(file->size);
    }
    central_directory_file_header.filename_len = htole16(file->filename.len);
    if (!file->need_zip64_offset)
        central_directory_file_header.offset = htole32(file->offset);
    if (!file->missing_crc32)
        central_directory_file_header.flags &= htole16(~zip_missing_crc32_flag);

    if (file->need_zip64) {
        central_directory_file_header.version_needed = zip_version_zip64;
        if (file->need_zip64_offset){
            extra_zip64_offset_size = ngx_zip_extra_field_zip64_sizes_offset_template;
            extra_zip64_offset_size.tag = htole16(extra_zip64_offset_size.tag);
            extra_zip64_offset_size.size = htole16(extra_zip64_offset_size.size);
            extra_zip64_offset_size.relative_header_offset = htole64(file->offset);
            extra_zip64_offset_size.compressed_size = extra_zip64_offset_size.uncompressed_size = htole64(file->size);
            extra_zip64_ptr = &extra_zip64_offset_size;
            extra_zip64_ptr_size = sizeof(extra_zip64_offset_size);
        } else { //zip64 only
            extra_zip64_size = ngx_zip_extra_field_zip64_sizes_only_template;
            extra_zip64_size.tag = htole16(extra_zip64_size.tag);
            extra_zip64_size.size = htole16(extra_zip64_size.size);
            extra_zip64_size.compressed_size = extra_zip64_size.uncompressed_size = htole64(file->size);
            extra_zip64_ptr = &extra_zip64_size;
            extra_zip64_ptr_size = sizeof(extra_zip64_size);
        }
    } else {
        if (file->need_zip64_offset){
            extra_zip64_offset = ngx_zip_extra_field_zip64_offset_only_template;
            extra_zip64_offset.tag = htole16(extra_zip64_offset.tag);
            extra_zip64_offset.size = htole16(extra_zip64_offset.size);
            extra_zip64_offset.relative_header_offset = htole64(file->offset);
            extra_zip64_ptr = &extra_zip64_offset;
            extra_zip64_ptr_size = sizeof(extra_zip64_offset);
        }
    }
    central_directory_file_header.extra_field_len=sizeof(ngx_zip_extra_field_central_t)+extra_zip64_ptr_size;
    extra_field_central = ngx_zip_extra_field_central_template;
    extra_field_central.tag = htole16(extra_field_central.tag);
    extra_field_central.size = htole16(extra_field_central.size);
    extra_field_central.mtime = htole32(file->unix_time);

    extra_field_unicode_path = ngx_zip_extra_field_unicode_path_template;
    extra_field_unicode_path.tag = htole16(extra_field_unicode_path.tag);

    if (ctx->unicode_path && file->filename_utf8.len) {
        extra_field_unicode_path.crc32 = htole32(file->filename_utf8_crc32);
        extra_field_unicode_path.size = htole16(sizeof(ngx_zip_extra_field_unicode_path_t) + file->filename_utf8.len);

        central_directory_file_header.extra_field_len += sizeof(ngx_zip_extra_field_unicode_path_t) + file->filename_utf8.len;
    }
    central_directory_file_header.extra_field_len = htole16(central_directory_file_header.extra_field_len);

    ngx_memcpy(p, &central_directory_file_header, sizeof(ngx_zip_central_directory_file_header_t));
    p += sizeof(ngx_zip_central_directory_file_header_t);

    ngx_memcpy(p, file->filename.data, file->filename.len);
    p += file->filename.len;

    ngx_memcpy(p, &extra_field_central, sizeof(ngx_zip_extra_field_central_t));
    p += sizeof(ngx_zip_extra_field_central_t);

    if (extra_zip64_ptr) {
        ngx_memcpy(p, extra_zip64_ptr, extra_zip64_ptr_size);
        p += extra_zip64_ptr_size;
    }

    if (ctx->unicode_path && file->filename_utf8.len) {
        ngx_memcpy(p, &extra_field_unicode_path, sizeof(ngx_zip_extra_field_unicode_path_t));
        p += sizeof(ngx_zip_extra_field_unicode_path_t);

        ngx_memcpy(p, file->filename_utf8.data, file->filename_utf8.len);
        p += file->filename_utf8.len;
    }
    return p;
}
