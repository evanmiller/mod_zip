
#include "ngx_http_zip_module.h"
#include "ngx_http_zip_file.h"
#include "ngx_http_zip_file_format.h"

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

static ngx_zip_data_descriptor_t ngx_zip_data_descriptor_template = {
    0x08074b50,  /* data descriptor signature */
    0,           /* crc-32 */
    0,           /* compressed size */
    0            /* uncompressed size */
};

static ngx_zip_local_file_header_t ngx_zip_local_file_header_template = {
    0x04034b50,  /* local file header signature */
    0x0a,        /* version needed to extract */
    0x08,        /* general purpose bit flag */
    0,           /* compression method */
    0,           /* last mod file date/time */
    0,           /* crc-32 */
    0,           /* compressed size */
    0,           /* uncompressed size */
    0,           /* file name length */
    sizeof(ngx_zip_extra_field_local_t),
                 /* extra field length */
};

static ngx_zip_central_directory_file_header_t ngx_zip_central_directory_file_header_template = {
    0x02014b50,  /* central file header signature */
    0x0300,      /* version made by */
    0x0a,        /* version needed to extract */
    0x08,        /* general purpose bit flag */
    0,           /* compression method */
    0,           /* last mod file time */
    0,           /* crc-32 */
    0,           /* compressed size */
    0,           /* uncompressed size */
    0,           /* file name length */
    sizeof(ngx_zip_extra_field_central_t), 
                 /* extra field length */
    0,           /* file comment length */
    0,           /* disk number start */
    0,           /* internal file attributes */
    0x81a40000,  /* external file attributes */
    0            /* relative offset of local header */
};

static ngx_zip_end_of_central_directory_record_t ngx_zip_end_of_central_directory_record_template = {
    0x06054b50,  /* end of central dir signature */
    0,           /* number of this disk */
    0,           /* number of the disk with the start of the central directory */
    0,           /* total number of entries in the central directory on this disk */
    0,           /* total number of entries in the central directory */
    0,           /* size of the central directory */
    0,           /* offset of start of central directory w.r.t. starting disk # */
    0            /* .ZIP file comment length */
};

static ngx_uint_t ngx_dos_time();
static void ngx_http_zip_truncate_buffer(ngx_buf_t *b, 
        ngx_http_zip_range_t *piece_range, ngx_http_zip_range_t *req_range);

/* 
 * Takes a UNIX timestamp and returns a DOS timestamp
 */
static ngx_uint_t ngx_dos_time(time_t t)
{
    ngx_tm_t tm;

    /* ngx_gmtime does the mon++ and year += 1900 for us */
    ngx_gmtime(t, &tm);

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

off_t 
ngx_http_zip_calculate_central_directory_size(off_t files_n,
        off_t filename_s)
{
    return 
        files_n * sizeof(ngx_zip_central_directory_file_header_t)
        + filename_s
        + files_n * sizeof(ngx_zip_extra_field_central_t)
        + sizeof(ngx_zip_end_of_central_directory_record_t);
}

ngx_int_t
ngx_http_zip_generate_pieces(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx)
{
    ngx_uint_t i, piece_i;
    off_t offset = 0;
    off_t filenames_len = 0;
    ngx_http_zip_file_t  *file;
    ngx_http_zip_piece_t *header_piece, *file_piece, *trailer_piece, *cd_piece;

    ctx->pieces_n = ctx->files.nelts * (2 + ctx->missing_crc32) + 1;

    if ((ctx->pieces = ngx_palloc(r->pool, sizeof(ngx_http_zip_piece_t) * 
                    ctx->pieces_n)) == NULL) {
        return NGX_ERROR;
    }

    for (piece_i=i=0; i<ctx->files.nelts; i++) {
        file = &((ngx_http_zip_file_t *)ctx->files.elts)[i];
        filenames_len += file->filename.len;
        file->offset = offset;

        header_piece = &ctx->pieces[piece_i++];
        header_piece->type = zip_header_piece;
        header_piece->file = file;
        header_piece->range.start = offset;
        header_piece->range.end = offset += sizeof(ngx_zip_local_file_header_t)
            + file->filename.len + sizeof(ngx_zip_extra_field_local_t);

        file_piece = &ctx->pieces[piece_i++];
        file_piece->type = zip_file_piece;
        file_piece->file = file;
        file_piece->range.start = offset;
        file_piece->range.end = offset += file->size;

        if (ctx->missing_crc32) {
            trailer_piece = &ctx->pieces[piece_i++];
            trailer_piece->type = zip_trailer_piece;
            trailer_piece->file = file;
            trailer_piece->range.start = offset;
            trailer_piece->range.end = offset += sizeof(ngx_zip_data_descriptor_t);
        }
    }

    cd_piece = &ctx->pieces[piece_i];
    cd_piece->type = zip_central_directory_piece;
    cd_piece->range.start = offset;
    cd_piece->range.end = offset +=
        ngx_http_zip_calculate_central_directory_size(ctx->files.nelts,
                filenames_len);

    ctx->archive_size = offset;

    return NGX_OK;
}

/* Craft a header for a file in a ZIP archive
 */
ngx_chain_t*
ngx_http_zip_file_header_chain_link(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range)
{
    ngx_chain_t *link;
    ngx_buf_t   *b;
    size_t       len;
    ngx_http_zip_file_t *file = piece->file;
    ngx_zip_extra_field_local_t   extra_field_local;
    ngx_zip_local_file_header_t   local_file_header;

    if ((link = ngx_alloc_chain_link(r->pool)) == NULL) {
        return NULL;
    }

    if ((b = ngx_calloc_buf(r->pool)) == NULL) {
        return NULL;
    }

    len = sizeof(ngx_zip_local_file_header_t) 
            + file->filename.len + sizeof(ngx_zip_extra_field_local_t);

    if ((b->pos = ngx_pcalloc(r->pool, len)) == NULL) {
        return NULL;
    }

    b->memory = 1;
    b->last = b->pos + len;

    file->unix_time = time(NULL);
    file->dos_time = ngx_dos_time(file->unix_time);

    /* A note about the ZIP format: in order to appease all ZIP software I
     * could find, the local file header contains the file sizes but not the
     * CRC-32, even though setting the third bit of the general purpose bit
     * flag would indicate that all three fields should be zeroed out.
     */

    local_file_header = ngx_zip_local_file_header_template;
    local_file_header.mtime = file->dos_time;
    local_file_header.compressed_size = file->size;
    local_file_header.uncompressed_size = file->size;
    local_file_header.filename_len = file->filename.len;
    if (!ctx->missing_crc32) {
        local_file_header.flags = 0;
        local_file_header.crc32 = file->crc32;
    }

    extra_field_local = ngx_zip_extra_field_local_template;
    extra_field_local.mtime = file->unix_time;
    extra_field_local.atime = file->unix_time;

    ngx_memcpy(b->pos, 
            &local_file_header, sizeof(ngx_zip_local_file_header_t));
    ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t),
            file->filename.data, file->filename.len);
    ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len, 
            &extra_field_local, sizeof(ngx_zip_extra_field_local_t));

    ngx_http_zip_truncate_buffer(b, &piece->range, range);

    link->buf = b;
    link->next = NULL;

    return link;
}

/* Craft a trailer for a file in a ZIP archive
 */
ngx_chain_t*
ngx_http_zip_data_descriptor_chain_link(ngx_http_request_t *r,
        ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range)
{
    ngx_chain_t *link;
    ngx_buf_t   *b;
    ngx_http_zip_file_t *file = piece->file;
    ngx_zip_data_descriptor_t  data_descriptor;

    if ((link = ngx_alloc_chain_link(r->pool)) == NULL) {
        return NULL;
    }

    if ((b = ngx_calloc_buf(r->pool)) == NULL) {
        return NULL;
    }

    b->pos = ngx_palloc(r->pool, sizeof(ngx_zip_data_descriptor_t));
    if (b->pos == NULL) {
        return NULL;
    }

    b->memory = 1;
    b->last = b->pos + sizeof(ngx_zip_data_descriptor_t);

    data_descriptor = ngx_zip_data_descriptor_template;
    data_descriptor.crc32 = file->crc32;
    data_descriptor.compressed_size = file->size;
    data_descriptor.uncompressed_size = file->size;

    ngx_memcpy(b->pos, &data_descriptor, sizeof(ngx_zip_data_descriptor_t));

    ngx_http_zip_truncate_buffer(b, &piece->range, range);

    link->buf = b;
    link->next = NULL;

    return link;
}

/* Attach the trailer to the whole ZIP archive */
ngx_chain_t *
ngx_http_zip_central_directory_chain_link(ngx_http_request_t *r, 
        ngx_http_zip_ctx_t *ctx, ngx_http_zip_piece_t *piece, ngx_http_zip_range_t *range)
{
    ngx_chain_t           *trailer;
    ngx_buf_t             *trailer_buf;
    size_t                 cd_len = 0, filenames_len = 0;
    u_char                *p;
    ngx_uint_t             i;
    ngx_http_zip_file_t   *file;
    ngx_array_t           *files;
    ngx_zip_end_of_central_directory_record_t  eocdr;

    if (ctx == NULL) {
        return NULL;
    }

    files = &ctx->files;

    if ((trailer = ngx_alloc_chain_link(r->pool)) == NULL) {
        return NULL;
    }

    if ((trailer_buf = ngx_calloc_buf(r->pool)) == NULL) {
        return NULL;
    }

    trailer->buf = trailer_buf;
    trailer->next = NULL;

    for (i=0; i < files->nelts; i++) {
        file = &((ngx_http_zip_file_t *)files->elts)[i];
        filenames_len += file->filename.len;
    }

    cd_len = ngx_http_zip_calculate_central_directory_size(files->nelts,
            filenames_len);

    if ((p = ngx_palloc(r->pool, cd_len)) == NULL) {
        return NULL;
    }

    trailer_buf->pos = p;
    trailer_buf->last = p + cd_len;
    trailer_buf->last_buf = 1;
    trailer_buf->sync = 1;
    trailer_buf->memory = 1;

    for (i=0; i < files->nelts; i++) {
        p = ngx_http_zip_write_central_directory_entry(p, 
                &((ngx_http_zip_file_t *)files->elts)[i], ctx);
    }

    eocdr = ngx_zip_end_of_central_directory_record_template;
    eocdr.disk_entries_n = files->nelts;
    eocdr.entries_n = files->nelts;
    eocdr.size = cd_len - sizeof(ngx_zip_end_of_central_directory_record_t);
    eocdr.offset = piece->range.start;

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

    central_directory_file_header = ngx_zip_central_directory_file_header_template;
    central_directory_file_header.mtime = file->dos_time;
    central_directory_file_header.crc32 = file->crc32;
    central_directory_file_header.compressed_size = file->size;
    central_directory_file_header.uncompressed_size = file->size;
    central_directory_file_header.filename_len = file->filename.len;
    central_directory_file_header.offset = file->offset;
    if (!ctx->missing_crc32) {
        central_directory_file_header.flags = 0;
    }

    extra_field_central = ngx_zip_extra_field_central_template;
    extra_field_central.mtime = file->unix_time;

    ngx_memcpy(p, &central_directory_file_header, 
            sizeof(ngx_zip_central_directory_file_header_t));

    ngx_memcpy(p += sizeof(ngx_zip_central_directory_file_header_t),
            file->filename.data, file->filename.len);

    ngx_memcpy(p += file->filename.len, 
            &extra_field_central, sizeof(ngx_zip_extra_field_central_t));

    return p + sizeof(ngx_zip_extra_field_central_t);
}
