#line 1 "ngx_http_zip_parsers.rl"
/* Ragel Parser definitions for mod_zip64 */

#include "ngx_http_zip_module.h"
#include "ngx_http_zip_parsers.h"

static void
ngx_http_zip_file_init(ngx_http_zip_file_t *parsing_file)
{
	ngx_str_null(&parsing_file->uri);
	ngx_str_null(&parsing_file->args);
	ngx_str_null(&parsing_file->filename);
	ngx_str_null(&parsing_file->filename_utf8);
	
	parsing_file->header_sent = 0;
	parsing_file->trailer_sent = 0;
	
	parsing_file->crc32 = 0;
	parsing_file->size = 0;
	
	parsing_file->missing_crc32 = 0;
	parsing_file->need_zip64 = 0;
	parsing_file->need_zip64_offset = 0;
	parsing_file->is_directory = 0;
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


#line 55 "ngx_http_zip_parsers.c"
static const signed char _request_actions[] = {
	0, 1, 2, 1, 3, 1, 4, 1,
	6, 1, 7, 1, 8, 1, 9, 2,
	0, 7, 2, 3, 1, 2, 5, 1,
	0
};

static const signed char _request_key_offsets[] = {
	0, 0, 1, 4, 7, 9, 11, 12,
	15, 22, 23, 24, 31, 0
};

static const char _request_trans_keys[] = {
	32, 32, 48, 57, 32, 48, 57, 32,
	63, 32, 63, 32, 0, 10, 13, 32,
	48, 57, 65, 70, 97, 102, 32, 32,
	45, 48, 57, 65, 70, 97, 102, 10,
	13, 45, 48, 57, 65, 70, 97, 102,
	0
};

static const signed char _request_single_lengths[] = {
	0, 1, 1, 1, 2, 2, 1, 3,
	1, 1, 1, 1, 3, 0
};

static const signed char _request_range_lengths[] = {
	0, 0, 1, 1, 0, 0, 0, 0,
	3, 0, 0, 3, 3, 0
};

static const signed char _request_index_offsets[] = {
	0, 0, 2, 5, 8, 11, 14, 16,
	20, 25, 27, 29, 34, 0
};

static const signed char _request_cond_targs[] = {
	2, 0, 2, 3, 0, 4, 3, 0,
	4, 0, 5, 6, 9, 5, 6, 7,
	0, 12, 12, 7, 2, 8, 8, 8,
	0, 0, 10, 6, 10, 1, 8, 8,
	8, 0, 12, 12, 1, 8, 8, 8,
	0, 0, 1, 2, 3, 4, 5, 6,
	7, 8, 9, 10, 11, 12, 0
};

static const signed char _request_cond_actions[] = {
	0, 0, 0, 7, 0, 0, 7, 0,
	0, 0, 1, 18, 3, 0, 0, 11,
	0, 13, 13, 0, 0, 9, 9, 9,
	0, 0, 5, 21, 0, 15, 15, 15,
	15, 0, 0, 0, 15, 15, 15, 15,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0
};

static const int request_start = 11;

static const int request_en_main = 11;


#line 57 "ngx_http_zip_parsers.rl"


ngx_int_t
ngx_http_zip_parse_request(ngx_http_zip_ctx_t *ctx)
{
	int cs;
	u_char *p = ctx->unparsed_request.elts;
	u_char *pe = p + ctx->unparsed_request.nelts;
	ngx_http_zip_file_t *parsing_file = NULL;
	

#line 126 "ngx_http_zip_parsers.c"
	{
		cs = (int)request_start;
	}

#line 129 "ngx_http_zip_parsers.c"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe )
			goto _out;
		_keys = ( _request_trans_keys + (_request_key_offsets[cs]));
		_trans = (unsigned int)_request_index_offsets[cs];
		
		_klen = (int)_request_single_lengths[cs];
		if ( _klen > 0 ) {
			const char *_lower = _keys;
			const char *_upper = _keys + _klen - 1;
			const char *_mid;
			while ( 1 ) {
				if ( _upper < _lower ) {
					_keys += _klen;
					_trans += (unsigned int)_klen;
					break;
				}
				
				_mid = _lower + ((_upper-_lower) >> 1);
				if ( ( (*( p))) < (*( _mid)) )
					_upper = _mid - 1;
				else if ( ( (*( p))) > (*( _mid)) )
					_lower = _mid + 1;
				else {
					_trans += (unsigned int)(_mid - _keys);
					goto _match;
				}
			}
		}
		
		_klen = (int)_request_range_lengths[cs];
		if ( _klen > 0 ) {
			const char *_lower = _keys;
			const char *_upper = _keys + (_klen<<1) - 2;
			const char *_mid;
			while ( 1 ) {
				if ( _upper < _lower ) {
					_trans += (unsigned int)_klen;
					break;
				}
				
				_mid = _lower + (((_upper-_lower) >> 1) & ~1);
				if ( ( (*( p))) < (*( _mid)) )
					_upper = _mid - 2;
				else if ( ( (*( p))) > (*( _mid + 1)) )
					_lower = _mid + 2;
				else {
					_trans += (unsigned int)((_mid - _keys)>>1);
					break;
				}
			}
		}
		
		_match: {}
		cs = (int)_request_cond_targs[_trans];
		
		if ( _request_cond_actions[_trans] != 0 ) {
			
			_acts = ( _request_actions + (_request_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 69 "ngx_http_zip_parsers.rl"
							
							parsing_file = ngx_array_push(&ctx->files);
							ngx_http_zip_file_init(parsing_file);
							
							parsing_file->index = ctx->files.nelts - 1;
						}
						
#line 209 "ngx_http_zip_parsers.c"

						break; 
					}
					case 1:  {
							{
#line 76 "ngx_http_zip_parsers.rl"
							
							if (parsing_file->args.len == 0
							&& parsing_file->uri.len == sizeof("@directory") - 1
							&& ngx_strncmp(parsing_file->uri.data, "@directory", parsing_file->uri.len) == 0) {
								parsing_file->is_directory = 1;
								// Directory has no content.
								parsing_file->size = 0;
								parsing_file->crc32 = 0;
								parsing_file->missing_crc32 = 0;
								parsing_file->uri.data = NULL;
								parsing_file->uri.len = 0;
								parsing_file->args.data = NULL;
								parsing_file->args.len = 0;
							}
						}
						
#line 231 "ngx_http_zip_parsers.c"

						break; 
					}
					case 2:  {
							{
#line 92 "ngx_http_zip_parsers.rl"
							
							parsing_file->uri.data = p;
							parsing_file->uri.len = 1;
						}
						
#line 242 "ngx_http_zip_parsers.c"

						break; 
					}
					case 3:  {
							{
#line 97 "ngx_http_zip_parsers.rl"
							
							parsing_file->uri.len = p - parsing_file->uri.data;
						}
						
#line 252 "ngx_http_zip_parsers.c"

						break; 
					}
					case 4:  {
							{
#line 100 "ngx_http_zip_parsers.rl"
							
							parsing_file->args.data = p;
						}
						
#line 262 "ngx_http_zip_parsers.c"

						break; 
					}
					case 5:  {
							{
#line 103 "ngx_http_zip_parsers.rl"
							
							parsing_file->args.len = p - parsing_file->args.data;
						}
						
#line 272 "ngx_http_zip_parsers.c"

						break; 
					}
					case 6:  {
							{
#line 106 "ngx_http_zip_parsers.rl"
							
							parsing_file->size = parsing_file->size * 10 + ((( (*( p)))) - '0');
						}
						
#line 282 "ngx_http_zip_parsers.c"

						break; 
					}
					case 7:  {
							{
#line 109 "ngx_http_zip_parsers.rl"
							
							if ((( (*( p)))) == '-') {
								ctx->missing_crc32 = 1;
								parsing_file->missing_crc32 = 1;
								ngx_crc32_init(parsing_file->crc32);
							} else {
								parsing_file->crc32 *= 16;
								parsing_file->crc32 += ngx_hextoi(p, 1);
							}
						}
						
#line 299 "ngx_http_zip_parsers.c"

						break; 
					}
					case 8:  {
							{
#line 119 "ngx_http_zip_parsers.rl"
							
							parsing_file->filename.data = p;
						}
						
#line 309 "ngx_http_zip_parsers.c"

						break; 
					}
					case 9:  {
							{
#line 122 "ngx_http_zip_parsers.rl"
							
							parsing_file->filename.len = p - parsing_file->filename.data;
						}
						
#line 319 "ngx_http_zip_parsers.c"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( cs != 0 ) {
			p += 1;
			goto _resume;
		}
		_out: {}
	}
	
#line 142 "ngx_http_zip_parsers.rl"

	
	/* suppress warning */
	(void)request_en_main;
	
	if (cs < 
#line 341 "ngx_http_zip_parsers.c"
11
#line 147 "ngx_http_zip_parsers.rl"
) {
		return NGX_ERROR;
	}
	
	ctx->parsed = 1;
	
	return NGX_OK;
}


#line 351 "ngx_http_zip_parsers.c"
static const signed char _range_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 2,
	0, 1, 2, 3, 1, 0
};

static const signed char _range_key_offsets[] = {
	0, 0, 1, 2, 3, 4, 5, 6,
	9, 11, 14, 17, 0
};

static const char _range_trans_keys[] = {
	98, 121, 116, 101, 115, 61, 45, 48,
	57, 48, 57, 45, 48, 57, 44, 48,
	57, 44, 48, 57, 0
};

static const signed char _range_single_lengths[] = {
	0, 1, 1, 1, 1, 1, 1, 1,
	0, 1, 1, 1, 0
};

static const signed char _range_range_lengths[] = {
	0, 0, 0, 0, 0, 0, 0, 1,
	1, 1, 1, 1, 0
};

static const signed char _range_index_offsets[] = {
	0, 0, 2, 4, 6, 8, 10, 12,
	15, 17, 20, 23, 0
};

static const signed char _range_cond_targs[] = {
	2, 0, 3, 0, 4, 0, 5, 0,
	6, 0, 7, 0, 8, 9, 0, 10,
	0, 11, 9, 0, 7, 10, 0, 7,
	11, 0, 0, 1, 2, 3, 4, 5,
	6, 7, 8, 9, 10, 11, 0
};

static const signed char _range_cond_actions[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 1, 7, 0, 10,
	0, 0, 3, 0, 0, 3, 0, 0,
	5, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0
};

static const int range_start = 1;

static const int range_en_main = 1;


#line 159 "ngx_http_zip_parsers.rl"


ngx_int_t
ngx_http_zip_parse_range(ngx_http_request_t *r, ngx_str_t *range_str, ngx_http_zip_ctx_t *ctx)
{
	int cs, prefix = 0, suffix = 0;
	
	ngx_http_zip_range_t *range = NULL;
	u_char *p = range_str->data;
	u_char *pe = range_str->data + range_str->len;
	

#line 414 "ngx_http_zip_parsers.c"
	{
		cs = (int)range_start;
	}

#line 417 "ngx_http_zip_parsers.c"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe )
			goto _out;
		_keys = ( _range_trans_keys + (_range_key_offsets[cs]));
		_trans = (unsigned int)_range_index_offsets[cs];
		
		_klen = (int)_range_single_lengths[cs];
		if ( _klen > 0 ) {
			const char *_lower = _keys;
			const char *_upper = _keys + _klen - 1;
			const char *_mid;
			while ( 1 ) {
				if ( _upper < _lower ) {
					_keys += _klen;
					_trans += (unsigned int)_klen;
					break;
				}
				
				_mid = _lower + ((_upper-_lower) >> 1);
				if ( ( (*( p))) < (*( _mid)) )
					_upper = _mid - 1;
				else if ( ( (*( p))) > (*( _mid)) )
					_lower = _mid + 1;
				else {
					_trans += (unsigned int)(_mid - _keys);
					goto _match;
				}
			}
		}
		
		_klen = (int)_range_range_lengths[cs];
		if ( _klen > 0 ) {
			const char *_lower = _keys;
			const char *_upper = _keys + (_klen<<1) - 2;
			const char *_mid;
			while ( 1 ) {
				if ( _upper < _lower ) {
					_trans += (unsigned int)_klen;
					break;
				}
				
				_mid = _lower + (((_upper-_lower) >> 1) & ~1);
				if ( ( (*( p))) < (*( _mid)) )
					_upper = _mid - 2;
				else if ( ( (*( p))) > (*( _mid + 1)) )
					_lower = _mid + 2;
				else {
					_trans += (unsigned int)((_mid - _keys)>>1);
					break;
				}
			}
		}
		
		_match: {}
		cs = (int)_range_cond_targs[_trans];
		
		if ( _range_cond_actions[_trans] != 0 ) {
			
			_acts = ( _range_actions + (_range_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 171 "ngx_http_zip_parsers.rl"
							
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
						
#line 504 "ngx_http_zip_parsers.c"

						break; 
					}
					case 1:  {
							{
#line 185 "ngx_http_zip_parsers.rl"
							range->start = range->start * 10 + ((( (*( p)))) - '0'); }
						
#line 512 "ngx_http_zip_parsers.c"

						break; 
					}
					case 2:  {
							{
#line 187 "ngx_http_zip_parsers.rl"
							range->end = range->end * 10 + ((( (*( p)))) - '0'); prefix = 0; }
						
#line 520 "ngx_http_zip_parsers.c"

						break; 
					}
					case 3:  {
							{
#line 189 "ngx_http_zip_parsers.rl"
							suffix = 1; }
						
#line 528 "ngx_http_zip_parsers.c"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( cs != 0 ) {
			p += 1;
			goto _resume;
		}
		_out: {}
	}
	
#line 202 "ngx_http_zip_parsers.rl"

	
	/* suppress warning */
	(void)range_en_main;
	
	if (cs < 
#line 550 "ngx_http_zip_parsers.c"
10
#line 207 "ngx_http_zip_parsers.rl"
) {
		return NGX_ERROR;
	}
	
	if (range) {
		if (ngx_http_zip_clean_range(range, prefix, suffix, ctx) == NGX_ERROR) {
			return NGX_ERROR;
		}
	}
	
	return NGX_OK;
}
