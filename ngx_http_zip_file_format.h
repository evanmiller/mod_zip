/* useful byte arrays for rolling a ZIP archive */
/* see http://www.pkware.com/documents/casestudies/APPNOTE.TXT */
/* also http://www.unix-ag.uni-kl.de/~conrad/krypto/pkcrack/doc/appnote.iz.txt */

/* byte-align structs to conform to ZIP format */
#pragma pack(push, 1)

#define zip_version_default 10
#define zip_version_zip64 45
#define zip_utf8_flag 0x0800
#define zip_missing_crc32_flag 0x08

typedef struct {
    uint16_t   tag; //0x5455
    uint16_t   size;
    uint8_t    info;
    uint32_t   mtime;
    uint32_t   atime;
} ngx_zip_extra_field_local_t; // extended timestamp

typedef struct {
    uint16_t   tag; //0x5455
    uint16_t   size;
    uint8_t    info;
    uint32_t   mtime;
} ngx_zip_extra_field_central_t;

typedef struct {
    uint16_t   tag; //0x7075
    uint16_t   size;
    uint8_t   version; //1
    uint32_t   crc32;
} ngx_zip_extra_field_unicode_path_t;

typedef struct { // not entirely writen...
    uint16_t   tag; //0x0001
    uint16_t   size; // size of this record (32)
    uint64_t   uncompressed_size; //!! in all other places in spec uncompressed follow compressed!
    uint64_t   compressed_size;
    //these for CD:
    uint64_t   relative_header_offset; //offset of local header record (cd)
    uint32_t   disc_start; // no of disc where file starts (cd)
} ngx_zip_extra_field_zip64_full_t;

typedef struct {
    uint16_t   tag; //0x0001
    uint16_t   size; //0x14
    uint64_t   uncompressed_size;
    uint64_t   compressed_size;
} ngx_zip_extra_field_zip64_sizes_only_t;

typedef struct {
    uint16_t   tag; //0x0001
    uint16_t   size; //0x0C
    uint64_t   relative_header_offset;
} ngx_zip_extra_field_zip64_offset_only_t;

typedef struct {
    uint16_t   tag; //0x0001
    uint16_t   size; //0x1C
    uint64_t   uncompressed_size;
    uint64_t   compressed_size;
    uint64_t   relative_header_offset;
} ngx_zip_extra_field_zip64_sizes_offset_t;


typedef struct {
    uint32_t   signature; //0x08074b50
    uint32_t   crc32;
    uint32_t   compressed_size;
    uint32_t   uncompressed_size;
} ngx_zip_data_descriptor_t;

typedef struct {
    uint32_t   signature;
    uint32_t   crc32;
    uint64_t   compressed_size;
    uint64_t   uncompressed_size;
} ngx_zip_data_descriptor_zip64_t;


typedef struct {
    uint32_t   signature; //0x04034b50
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
    uint32_t   signature; //0x02014b50
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
    uint32_t   signature; //0x06054b50
    uint16_t   disk_n;
    uint16_t   cd_disk_n;
    uint16_t   disk_entries_n;
    uint16_t   entries_n;
    uint32_t   size;
    uint32_t   offset;
    uint16_t   comment_len;
} ngx_zip_end_of_central_directory_record_t;

typedef struct {
    uint32_t   signature; // 0x06064b50
    uint64_t   size; //of this record (+variable fields, but minus signature and this size field), Size = SizeOfFixedFields + SizeOfVariableData - 12
    uint16_t   version_made_by;
    uint16_t   version_needed;
    uint32_t   disk_n;
    uint32_t   cd_disk_n; // num of disk with start of CD
    uint64_t   cd_n_entries_on_this_disk;
    uint64_t   cd_n_entries_total;
    uint64_t   cd_size;
    uint64_t   cd_offset; // cd offset with respect to starting disk number
    //variable fields go here
} ngx_zip_zip64_end_of_central_directory_record_t;

typedef struct {
    uint32_t signature; //0x07064b50
    uint32_t z64_cd_disk_n; // number of disk with start of zip64 end of central directory
    uint64_t cd_relative_offset;
    uint32_t disks_total_n;
} ngx_zip_zip64_end_of_central_directory_locator_t;
#pragma pack(pop)
