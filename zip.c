/* zip.c: Routines for accessing zip archives
   Copyright (c) 2012 Sergio Baldoví

   Based on zip routines from ZXDS.
   Copyright (c) 2010 Patrik Rak

   $Id$

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

   Author contact information:

   E-mail: philip-fuse@shadowmagic.org.uk

*/

#include <string.h>
#include <zlib.h>

#include "internals.h"
#include "zip.h"

#ifndef MIN
#define MIN(x,y) (((x) < (y)) ? (x) : (y))
#endif

enum {
  ARCHIVE_CLOSED = 0,
  ARCHIVE_OPEN
};

void
read4( libspectrum_byte *out, const libspectrum_byte **in )
{
#ifdef WORDS_BIGENDIAN
  *out++ = (*in)[3];
  *out++ = (*in)[2];
  *out++ = (*in)[1];
  *out++ = (*in)[0];
#else
  memcpy( out, *in, 4 );
#endif

  (*in) += 4;
}

void
read2( libspectrum_byte *out, const libspectrum_byte **in )
{
#ifdef WORDS_BIGENDIAN
  *out++ = (*in)[1];
  *out++ = (*in)[0];
#else
  memcpy( out, *in, 2 );
#endif

  (*in) += 2;
}

/* Seek safely within ZIP archive */
int
seek( struct libspectrum_zip *z, long offset, int whence )
{
  const libspectrum_byte *ptr;

  switch( whence ) {
  case SEEK_SET:
    ptr = z->input_data + offset;
    break;

  case SEEK_CUR:
    ptr = z->ptr + offset;
    break;

  case SEEK_END:
    ptr = z->end + offset;
    break;

  default:
    return -1;
  }

  /* Validate address */
  if( ptr < z->input_data || ptr > z->end )
    return -1;

  z->ptr = ptr;

  return 0;
}

int
read_directory_info( zip_directory_info *info, const libspectrum_byte *buffer,
                     const libspectrum_byte *end )
{
  if( buffer + ZIP_DIRECTORY_INFO_SIZE > end ) {
    return 0;
  }

  read4( (libspectrum_byte *)&info->magic, &buffer );
  read2( (libspectrum_byte *)&info->disk_index, &buffer );
  read2( (libspectrum_byte *)&info->directory_disk_index, &buffer );
  read2( (libspectrum_byte *)&info->disk_file_count, &buffer );
  read2( (libspectrum_byte *)&info->file_count, &buffer );
  read4( (libspectrum_byte *)&info->directory_size, &buffer );
  read4( (libspectrum_byte *)&info->directory_offset, &buffer );
  read2( (libspectrum_byte *)&info->comment_size, &buffer );

  return ZIP_DIRECTORY_INFO_SIZE;
}

int
read_file_header( zip_file_header *file_info, const libspectrum_byte *buffer,
                  const libspectrum_byte *end )
{
  if( buffer + ZIP_FILE_HEADER_SIZE > end ) {
    return 0;
  }

  read4( (libspectrum_byte *)&file_info->magic, &buffer );
  read2( (libspectrum_byte *)&file_info->creator_version, &buffer );
  read2( (libspectrum_byte *)&file_info->required_version, &buffer );
  read2( (libspectrum_byte *)&file_info->flags, &buffer );
  read2( (libspectrum_byte *)&file_info->compression, &buffer );
  read2( (libspectrum_byte *)&file_info->mod_time, &buffer );
  read2( (libspectrum_byte *)&file_info->mod_date, &buffer );
  read4( (libspectrum_byte *)&file_info->crc, &buffer );
  read4( (libspectrum_byte *)&file_info->compressed_size, &buffer );
  read4( (libspectrum_byte *)&file_info->uncompressed_size, &buffer );
  read2( (libspectrum_byte *)&file_info->name_size, &buffer );
  read2( (libspectrum_byte *)&file_info->extra_field_size, &buffer );
  read2( (libspectrum_byte *)&file_info->comment_size, &buffer );
  read2( (libspectrum_byte *)&file_info->disk_index, &buffer );
  read2( (libspectrum_byte *)&file_info->internal_flags, &buffer );
  read4( (libspectrum_byte *)&file_info->external_flags, &buffer );
  read4( (libspectrum_byte *)&file_info->file_offset, &buffer );

  return ZIP_FILE_HEADER_SIZE;
}

int
read_local_header( zip_local_header *local_info,
                   const libspectrum_byte *buffer,
                   const libspectrum_byte *end )
{
  if( buffer + ZIP_LOCAL_HEADER_SIZE > end ) {
    return 0;
  }

  read4( (libspectrum_byte *)&local_info->magic, &buffer );
  read2( (libspectrum_byte *)&local_info->required_version, &buffer );
  read2( (libspectrum_byte *)&local_info->flags, &buffer );
  read2( (libspectrum_byte *)&local_info->compression, &buffer );
  read2( (libspectrum_byte *)&local_info->mod_time, &buffer );
  read2( (libspectrum_byte *)&local_info->mod_date, &buffer );
  read4( (libspectrum_byte *)&local_info->crc, &buffer );
  read4( (libspectrum_byte *)&local_info->compressed_size, &buffer );
  read4( (libspectrum_byte *)&local_info->uncompressed_size, &buffer );
  read2( (libspectrum_byte *)&local_info->name_size, &buffer );
  read2( (libspectrum_byte *)&local_info->extra_field_size, &buffer );

  return ZIP_LOCAL_HEADER_SIZE;
}

int
match_file_names( const char *a, const char *b, const int ignore_case )
{
  if( !a || !b ) return 0;

  return ( ignore_case ? ( strcasecmp( a, b ) == 0 ) :
                          ( strcmp( a, b ) == 0 ) );
}

/* Close the ZIP archive */
static void
close_zip( struct libspectrum_zip *z )
{
  z->state = ARCHIVE_CLOSED;
  z->input_data = NULL;
  z->data_size = 0;
  z->ptr = NULL;
  z->end = NULL;
}

/* Locate the ZIP central directory info */
int
locate_directory_info( struct libspectrum_zip *z, zip_directory_info *info )
{
  if( seek( z, -ZIP_DIRECTORY_INFO_SIZE, SEEK_END ) ) {
    return 1;
  }

  /* Such a horrible mess just because of the stupid variable size comment at
     the end. Sigh. */
  while( z->ptr >= z->input_data ) {
    if( z->ptr[0] == 'P' &&
        z->ptr[1] == 'K' &&
        z->ptr[2] == 5  &&
        z->ptr[3] == 6 )
    {
      if( read_directory_info( info, z->ptr, z->end ) != 0 ) {
        return 0;
      }
    }

    z->ptr--;
  }

  return 1;
}

/* Locate the ZIP central directory */
int
locate_directory( struct libspectrum_zip *z )
{
  if( z->directory_offset != 0 ) {
    return 0;
  }

  zip_directory_info info;
  if( locate_directory_info( z, &info ) ) {
    return 1;
  }

  if( info.disk_index != info.directory_disk_index ) {
    return 1;
  }

  z->directory_offset = info.directory_offset;
  z->file_count = MIN( info.disk_file_count, info.file_count );

  return 0;
}

/* Get the number of entries in the archive (files and directories) */
unsigned int
libspectrum_zip_num_entries( struct libspectrum_zip *z )
{
  return (z)? z->file_count : 0;
}

/* Rewind to beginning of ZIP directory */
libspectrum_error
libspectrum_zip_rewind( struct libspectrum_zip *z )
{
  if( !z || z->state == ARCHIVE_CLOSED )
    return LIBSPECTRUM_ERROR_INVALID;

  if( seek( z, z->directory_offset, SEEK_SET ) ) {
    return LIBSPECTRUM_ERROR_CORRUPT;
  }

  z->file_index = 0;
  z->state = ARCHIVE_OPEN;

  return LIBSPECTRUM_ERROR_NONE;
}

/* Read next entry in the ZIP central directory */
int
read_directory( struct libspectrum_zip *z )
{
  int retry, retval;

  do {
    retry = 0;

    /* Stop when we have read it all */
    if( z->file_index >= z->file_count ) {
      return 1;
    }
    z->file_index++;

    /* Read the info */
    retval = read_file_header( &z->file_info, z->ptr, z->end );
    if( !retval ) {
      return 1;
    } else {
      z->ptr += retval;
    }

    /* Verify the header */
    if( z->file_info.magic != ZIP_FILE_HEADER_SIG ) {
      return 1;
    }

    /* Unix file names are case sensitive, the rest is not (or I don't know) */
    z->file_ignore_case = ( ( z->file_info.creator_version >> 8 ) != 3 );

    /* Read the name, but skip files with too long names */
    libspectrum_dword skip = z->file_info.comment_size +
                             z->file_info.extra_field_size;
    libspectrum_dword name_size = z->file_info.name_size;

    if( z->ptr + name_size > z->end ) {
      return 1;
    }

    if( name_size < sizeof( z->file_name ) ) {
      memcpy( z->file_name, z->ptr, name_size );
      z->file_name[ name_size ] = 0;
    }
    else {
      retry = 1;
    }

    skip += name_size;

    /* Skip to the next file header if necessary */
    if( skip > 0 ) {
      if( seek( z, skip, SEEK_CUR ) ) {
        return 1;
      }
    }

  } while( retry );

  return 0;
}

/* Open a ZIP archive from memory */
struct libspectrum_zip *
libspectrum_zip_open( const libspectrum_byte *buffer, size_t length )
{
  struct libspectrum_zip *z;

  if( !buffer || !length ) return NULL;

  z = libspectrum_calloc( 1, sizeof( struct libspectrum_zip ) );
  z->input_data = buffer;
  z->ptr = buffer;
  z->end = buffer + length;
  z->data_size = length;
  z->state = ARCHIVE_OPEN;

  if( locate_directory( z ) ) {
    fprintf( stderr, "Unrecognized ZIP archive\n" );

    libspectrum_zip_close( z );
    return NULL;
  }

  if( libspectrum_zip_rewind( z ) != LIBSPECTRUM_ERROR_NONE ) {
    libspectrum_zip_close( z );
    return NULL;
  }

  return z;
}

void
dump_entry_stat( struct libspectrum_zip *z, zip_stat *info )
{
  char *slash;
  size_t length;

  strcpy( info->name, z->file_name );
  slash = strrchr( info->name, '/' );
  info->filename = ( slash )? slash + 1 : info->name;

  length = strlen( z->file_name );
  info->is_dir = ( z->file_name[ length - 1 ] == '/' ) ? 1 : 0;

  info->size = z->file_info.uncompressed_size;
  info->index = z->file_index -1;
}

/* Jump to next entry in the archive */
int
libspectrum_zip_next( struct libspectrum_zip *z, zip_stat *info )
{
  int retval;

  if( !z || z->state == ARCHIVE_CLOSED ) {
    return LIBSPECTRUM_ERROR_INVALID;
  }

  retval = read_directory( z );
  if( retval ) return LIBSPECTRUM_ERROR_UNKNOWN;

  dump_entry_stat( z, info );

  return LIBSPECTRUM_ERROR_NONE;
}

/* Locate a file in the archive (non-sequential acces) */
int
libspectrum_zip_locate( struct libspectrum_zip *z, const char *filename, 
                        int flags, zip_stat *info )
{
  int ignore_dir, ignore_case;
  size_t length;

  if( !z || z->state == ARCHIVE_CLOSED ) {
    return -1;
  }

  if( !filename || strlen( filename ) == 0 ) return -1;

  if( libspectrum_zip_rewind( z ) != LIBSPECTRUM_ERROR_NONE ) {
    close_zip( z );
    return -1;
  }

  ignore_dir = flags & ZIPFLAG_NODIR;
  ignore_case = ( flags & ZIPFLAG_AUTOCASE )? z->file_ignore_case :
                                              ( flags & ZIPFLAG_NOCASE );

  while( read_directory( z ) == 0 ) {
    const char *fname, *slash;

    /* Ignore directories in path */
    fname = NULL;
    if( ignore_dir ) {
      slash = strrchr( z->file_name, '/' );
      fname = ( slash )? slash + 1 : z->file_name;
    } else {
      fname = z->file_name;
    }

    if( !fname || strlen( fname ) == 0 ) continue;

    /* Skip entry if it is a directory */
    length = strlen( fname );
    if( fname[ length - 1 ] == '/' ) continue;

    if( match_file_names( filename, fname, ignore_case ) ) {
      dump_entry_stat( z, info );
      return info->index;
    }
  }

  return -1;
}

/* Close ZIP archive */
void
libspectrum_zip_close( struct libspectrum_zip *z )
{
  if( z ) {
    close_zip( z );
    libspectrum_free( z );
  }
}

/* Prepare stream for reading from ZIP archive */
int
prepare_stream( struct libspectrum_zip *z )
{
  zip_local_header header;
  int retval;
  libspectrum_dword skip;
  libspectrum_word version;

  /* Seek to the local header and read it */
  if( seek( z, z->file_info.file_offset, SEEK_SET ) ) {
    return 1;
  }

  retval = read_local_header( &header, z->ptr, z->end );
  if( !retval ) {
    return 1;
  } else {
    z->ptr += retval;
  }

  /* Verify the header */
  if( header.magic != ZIP_LOCAL_HEADER_SIG ) {
    return 1;
  }

  version = ( header.required_version & 0xff );
  if( version > ZIP_SUPPORTED_VERSION ) {
    fprintf( stderr, "Unsupported ZIP version %u.%u", version / 10,
             version % 10 );
    return 1;
  }

  /* Skip the variable fields. Note that we don't bother with matching the rest
     against the central directory header. The local header version may be
     masked out anyway, so we rather use the central directory version as
     authorative. */
  skip = header.name_size + header.extra_field_size;

  if( seek( z, skip, SEEK_CUR ) ) {
    return 1;
  }

  return 0;
}

/* Decompress the zlib compressed data */
int
decompress_stream( struct libspectrum_zip *z, libspectrum_byte **buffer,
                   size_t *buffer_size )
{
  int error;
  size_t file_compressed_left;

  /* Note that we take the sizes from central directory rather than
     the local header, as those may be 0 in case of non-seekable compressed
     streams */
  file_compressed_left = z->file_info.compressed_size;

  /* Nothing to do */
  if( file_compressed_left == 0 ) {
    return 1;
  }

  /* Bad archive? */
  if( z->ptr + file_compressed_left > z->end ) {
    return 1;
  }

  error = libspectrum_zip_inflate( z->ptr, file_compressed_left, buffer,
                                   buffer_size );

  if( error != LIBSPECTRUM_ERROR_NONE ) {
    return 1;
  }

  z->ptr += file_compressed_left;

  return 0;
}

/* Read file from ZIP archive */
libspectrum_error
libspectrum_zip_read( struct libspectrum_zip *z, libspectrum_byte **buffer,
                      size_t *size )
{
  const libspectrum_byte *last = z->ptr;

  if( prepare_stream( z ) ) {
    z->ptr = last;
    return LIBSPECTRUM_ERROR_UNKNOWN;
  }

  /* Report EOF when there is no more to read */
  *size = z->file_info.uncompressed_size;

  if( *size == 0 ) {
    return LIBSPECTRUM_ERROR_UNKNOWN;
  }

  /* Now read the data depending on the compression method used */
  const libspectrum_word compression = z->file_info.compression;

  switch( compression ) {

  case 0: /* store */
    if( z->ptr + *size > z->end ) return 1;
    *buffer = libspectrum_malloc( *size );
    memcpy( *buffer, z->ptr, *size );
    break;

  case 8: /* deflate */
    if( decompress_stream( z, buffer, size ) ) {
      fprintf( stderr, "ZIP decompression failed" );
      z->ptr = last;
      return LIBSPECTRUM_ERROR_CORRUPT;
    }
    break;

  default:
    z->ptr = last;
    fprintf( stderr, "Unsupported compression method %u.", compression );
    return 1;
  }

  /* Restore position to allow reading next header in central directory */
  z->ptr = last;

  /* Update the CRC, and report an error when it doesn't match at end */
  libspectrum_dword file_crc = crc32( 0, *buffer, *size );

  if( file_crc != z->file_info.crc ) {
    fprintf( stderr, "ZIP CRC mismatch" );
    return LIBSPECTRUM_ERROR_CORRUPT;
  }

  return LIBSPECTRUM_ERROR_NONE;
}

/* Make 'best guesses' as to what to uncompress from the archive */
libspectrum_error
libspectrum_zip_blind_read( const libspectrum_byte *zipptr, size_t ziplength,
                            libspectrum_byte **outptr, size_t *outlength )
{
  struct libspectrum_zip *z;
  zip_stat info;
  int retval;

  z = libspectrum_zip_open( zipptr, ziplength );
  if( !z ) return LIBSPECTRUM_ERROR_INVALID;

  while( libspectrum_zip_next( z, &info ) == LIBSPECTRUM_ERROR_NONE ) {

    /* Skip directories and empty files */
    if( !info.size ) continue; 

    libspectrum_id_t type;
    libspectrum_class_t class;
    int error;

    /* Try to identify the file by the filename */
    error = libspectrum_identify_file_raw( &type, info.filename, NULL, 0 );
    if( error ) continue;

    error = libspectrum_identify_class( &class, type );
    if( error ) continue;

    /* Skip files not likely to be loaded in a emulator */
    if( class != LIBSPECTRUM_CLASS_UNKNOWN &&
        class != LIBSPECTRUM_CLASS_COMPRESSED &&
        class != LIBSPECTRUM_CLASS_AUXILIARY ) {

      retval = libspectrum_zip_read( z, outptr, outlength );
      libspectrum_zip_close( z );

      return retval;
    }
  }

  libspectrum_zip_close( z );

  return LIBSPECTRUM_ERROR_UNKNOWN;
}
