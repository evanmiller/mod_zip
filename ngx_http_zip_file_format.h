/* useful byte arrays for rolling a ZIP archive */
/* see http://www.pkware.com/documents/casestudies/APPNOTE.TXT */
/* also http://www.unix-ag.uni-kl.de/~conrad/krypto/pkcrack/doc/appnote.iz.txt */

/* byte-align structs to conform to ZIP format */
#pragma pack(push, 1)

typedef struct {
    uint16_t   tag;
    uint16_t   size;
    uint8_t    info;
    uint32_t   mtime;
    uint32_t   atime;
} ngx_zip_extra_field_local_t;

typedef struct {
    uint16_t   tag;
    uint16_t   size;
    uint8_t    info;
    uint32_t   mtime;
} ngx_zip_extra_field_central_t;

typedef struct {
    uint32_t   signature;
    uint32_t   crc32;
    uint32_t   compressed_size;
    uint32_t   uncompressed_size;
} ngx_zip_data_descriptor_t;

typedef struct {
    uint32_t   signature;
    uint16_t   version;
    uint16_t   flags;
    uint16_t   compression_method;
    uint32_t   mtime;
    uint32_t   crc32;
    uint32_t   compressed_size;
    uint32_t   uncompressed_size;
    uint16_t   filename_len;
    uint16_t   extra_field_len;
} ngx_zip_local_file_header_t;

typedef struct {
    uint32_t   signature;
    uint16_t   version_made_by;
    uint16_t   version_needed;
    uint16_t   flags;
    uint16_t   compression_method;
    uint32_t   mtime;
    uint32_t   crc32;
    uint32_t   compressed_size;
    uint32_t   uncompressed_size;
    uint16_t   filename_len;
    uint16_t   extra_field_len;
    uint16_t   comment_len;
    uint16_t   disk_n;
    uint16_t   attr_internal;
    uint32_t   attr_external;
    uint32_t   offset;
} ngx_zip_central_directory_file_header_t;

typedef struct {
    uint32_t   signature;
    uint16_t   disk_n;
    uint16_t   cd_disk_n;
    uint16_t   disk_entries_n;
    uint16_t   entries_n;
    uint32_t   size;
    uint32_t   offset;
    uint16_t   comment_len;
} ngx_zip_end_of_central_directory_record_t;

#pragma pack(pop)
