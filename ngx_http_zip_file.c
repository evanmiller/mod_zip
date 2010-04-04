
#include "ngx_http_zip_module.h"
#include "ngx_http_zip_file.h"
#include "ngx_http_zip_file_format.h"


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


// make our proposed ZIP-file chunk map
ngx_int_t
ngx_http_zip_generate_pieces(ngx_http_request_t *r, ngx_http_zip_ctx_t *ctx)
{
    ngx_uint_t i, piece_i;
    off_t offset = 0;
    ngx_http_zip_file_t  *file;
    ngx_http_zip_piece_t *header_piece, *file_piece, *trailer_piece, *cd_piece;

    // pieces: for each file: header, data, footer (if needed) -> 2 or 3 per file
    // plus file footer (CD + [zip64 end + zip64 locator +] end of cd) in one chunk
    ctx->pieces_n = ctx->files.nelts * (2 + (!!ctx->missing_crc32)) + 1;

    if ((ctx->pieces = ngx_palloc(r->pool, sizeof(ngx_http_zip_piece_t) * ctx->pieces_n)) == NULL) 
        return NGX_ERROR;
    
    ctx->cd_size = 0;
    for (piece_i = i = 0; i < ctx->files.nelts; i++) {
        file = &((ngx_http_zip_file_t *)ctx->files.elts)[i];
        file->offset = offset;

        if(offset >= NGX_MAX_UINT32_VALUE)
            ctx->zip64_used = file->need_zip64_offset = 1;
        if(file->size >= NGX_MAX_UINT32_VALUE) 
            ctx->zip64_used = file->need_zip64 = 1;

        ctx->cd_size += sizeof(ngx_zip_central_directory_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_central_t) 
            + (file->need_zip64_offset ? 
                    (file->need_zip64 ? sizeof(ngx_zip_extra_field_zip64_sizes_offset_t) : sizeof(ngx_zip_extra_field_zip64_offset_only_t)) :
                    (file->need_zip64 ? sizeof(ngx_zip_extra_field_zip64_sizes_only_t) : 0)
              );

        header_piece = &ctx->pieces[piece_i++];
        header_piece->type = zip_header_piece;
        header_piece->file = file;
        header_piece->range.start = offset;
        header_piece->range.end = offset += sizeof(ngx_zip_local_file_header_t)
            + file->filename.len + sizeof(ngx_zip_extra_field_local_t) + (file->need_zip64? sizeof(ngx_zip_extra_field_zip64_sizes_only_t):0);

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

    ctx->zip64_used |= offset >= NGX_MAX_UINT32_VALUE || ctx->files.nelts >= NGX_MAX_UINT16_VALUE;

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

    size_t len = sizeof(ngx_zip_local_file_header_t) + file->filename.len 
        + sizeof(ngx_zip_extra_field_local_t) + (file->need_zip64? sizeof(ngx_zip_extra_field_zip64_sizes_only_t):0);

    if ((link = ngx_alloc_chain_link(r->pool)) == NULL || (b = ngx_calloc_buf(r->pool)) == NULL 
            || (b->pos = ngx_pcalloc(r->pool, len)) == NULL)
        return NULL;
    
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
    local_file_header.filename_len = file->filename.len;
    if (file->need_zip64) {
        local_file_header.version = zip_version_zip64;
        local_file_header.extra_field_len = sizeof(ngx_zip_extra_field_zip64_sizes_only_t) + sizeof(ngx_zip_extra_field_local_t);
        extra_field_zip64 = ngx_zip_extra_field_zip64_sizes_only_template;
        extra_field_zip64.uncompressed_size = extra_field_zip64.compressed_size = file->size;
    } else {
        local_file_header.compressed_size = file->size;
        local_file_header.uncompressed_size = file->size;
    }

    if (!file->missing_crc32) {
        local_file_header.flags &= ~zip_missing_crc32_flag;
        local_file_header.crc32 = file->crc32;
    }

    extra_field_local = ngx_zip_extra_field_local_template;
    extra_field_local.mtime = file->unix_time;
    extra_field_local.atime = file->unix_time;

    ngx_memcpy(b->pos, &local_file_header, sizeof(ngx_zip_local_file_header_t));
    ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t), file->filename.data, file->filename.len);
    ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len, 
            &extra_field_local, sizeof(ngx_zip_extra_field_local_t));
    if (file->need_zip64)
        ngx_memcpy(b->pos + sizeof(ngx_zip_local_file_header_t) + file->filename.len + sizeof(ngx_zip_extra_field_local_t), 
                &extra_field_zip64, sizeof(ngx_zip_extra_field_zip64_sizes_only_t));

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
        data.descriptor.crc32 = file->crc32;
        data.descriptor.compressed_size = data.descriptor.uncompressed_size = file->size;
    } else {
        data.descriptor64 = ngx_zip_data_descriptor_zip64_template;
        data.descriptor64.crc32 = file->crc32;
        data.descriptor64.compressed_size = data.descriptor64.uncompressed_size = file->size;
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
    if (files->nelts < NGX_MAX_UINT16_VALUE) {
        eocdr.disk_entries_n = files->nelts;
        eocdr.entries_n = files->nelts;
    }

    cd_size = ctx->cd_size - sizeof(ngx_zip_end_of_central_directory_record_t) 
        - (!!ctx->zip64_used)*(sizeof(ngx_zip_zip64_end_of_central_directory_record_t) 
                + sizeof(ngx_zip_zip64_end_of_central_directory_locator_t));

    if (cd_size < NGX_MAX_UINT32_VALUE)
        eocdr.size = cd_size;
    if (piece->range.start < NGX_MAX_UINT32_VALUE)
        eocdr.offset = piece->range.start;

    if (ctx->zip64_used) {
        eocdr64 = ngx_zip_zip64_end_of_central_directory_record_template;
        eocdr64.cd_n_entries_on_this_disk = eocdr64.cd_n_entries_total = files->nelts;
        eocdr64.cd_size = cd_size;
        eocdr64.cd_offset = piece->range.start;

        ngx_memcpy(p, &eocdr64, sizeof(ngx_zip_zip64_end_of_central_directory_record_t));
        p += sizeof(ngx_zip_zip64_end_of_central_directory_record_t);

        locator64 = ngx_zip_zip64_end_of_central_directory_locator_template;
        locator64.cd_relative_offset = piece->range.start + eocdr64.cd_size;
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
    void* extra_zip64_ptr = NULL; //!!
    size_t extra_zip64_ptr_size = 0;

    central_directory_file_header = ngx_zip_central_directory_file_header_template;
    central_directory_file_header.mtime = file->dos_time;
    central_directory_file_header.crc32 = file->crc32;

    if (!file->need_zip64) {
        central_directory_file_header.compressed_size = file->size;
        central_directory_file_header.uncompressed_size = file->size;
    }
    central_directory_file_header.filename_len = file->filename.len;
    if (!file->need_zip64_offset)
        central_directory_file_header.offset = file->offset;
    if (!file->missing_crc32)
        central_directory_file_header.flags &= ~zip_missing_crc32_flag;

    if (file->need_zip64) {
        central_directory_file_header.version_needed = zip_version_zip64;
        if (file->need_zip64_offset){
            extra_zip64_offset_size = ngx_zip_extra_field_zip64_sizes_offset_template;
            extra_zip64_offset_size.relative_header_offset = file->offset;
            extra_zip64_offset_size.compressed_size = extra_zip64_offset_size.uncompressed_size = file->size;
            extra_zip64_ptr = &extra_zip64_offset_size;
            extra_zip64_ptr_size = sizeof(extra_zip64_offset_size);
        } else { //zip64 only
            extra_zip64_size = ngx_zip_extra_field_zip64_sizes_only_template;
            extra_zip64_size.compressed_size = extra_zip64_size.uncompressed_size = file->size;
            extra_zip64_ptr = &extra_zip64_size;
            extra_zip64_ptr_size = sizeof(extra_zip64_size);
        }
    } else {
        if (file->need_zip64_offset){
            extra_zip64_offset = ngx_zip_extra_field_zip64_offset_only_template;
            extra_zip64_offset.relative_header_offset = file->offset;
            extra_zip64_ptr = &extra_zip64_offset;
            extra_zip64_ptr_size = sizeof(extra_zip64_offset);
        }
    }
    central_directory_file_header.extra_field_len=sizeof(ngx_zip_extra_field_central_t)+extra_zip64_ptr_size;
    extra_field_central = ngx_zip_extra_field_central_template;
    extra_field_central.mtime = file->unix_time;

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

    return p;
}
