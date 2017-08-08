/* mmc.c: Emulation of the SD / MMC interface
   Copyright (c) 2017 Philip Kendall

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

   E-mail: Philip Kendall <philip-fuse@shadowmagic.org.uk>

*/

#include <config.h>

#include <stdio.h>
#include <string.h>

#include "internals.h"

/* The states while a command is being sent to the card */
enum command_state_t {
  WAITING_FOR_COMMAND,
  WAITING_FOR_DATA0,
  WAITING_FOR_DATA1,
  WAITING_FOR_DATA2,
  WAITING_FOR_DATA3,
  WAITING_FOR_CRC,
  WAITING_FOR_DATA_TOKEN,
  WAITING_FOR_DATA,
  WAITING_FOR_DATA_CRC1,
  WAITING_FOR_DATA_CRC2
};

/* The MMC commands we support */
enum command_byte {
  GO_IDLE_STATE = 0,
  SEND_IF_COND = 8,
  SEND_CSD = 9,
  SEND_CID = 10,
  READ_SINGLE_BLOCK = 17,
  WRITE_BLOCK = 24,
  APP_SEND_OP_COND = 41,
  APP_CMD = 55,
  READ_OCR = 58
};

typedef struct libspectrum_mmc_card {
  /* The actual "card" data */
  libspectrum_ide_drive drive;

  /* Cache of written sectors */
  GHashTable *cache;

  /* The C_SIZE field of the card CSD */
  libspectrum_word c_size;

  /* Is the MMC interface currently idle? */
  int is_idle;

  /* The current state of the command being transmitted to the card */
  enum command_state_t command_state;

  /* The most recent command byte sent to the MMC */
  libspectrum_byte current_command;

  /* The argument for the current MMC command */
  libspectrum_byte current_argument[4];

  /* How much data has been sent for the current command */
  size_t data_count;

  /* The data for the current command */
  libspectrum_byte send_buffer[512];

  /* The response to the most recent command */
  libspectrum_byte response_buffer[516];

  /* One past the last valid byte in response_buffer */
  libspectrum_byte *response_buffer_end;

  /* The next byte to be returned from response_buffer */
  libspectrum_byte *response_buffer_next;
} libspectrum_mmc_card;

libspectrum_mmc_card*
libspectrum_mmc_alloc( void )
{
  libspectrum_mmc_card *card = libspectrum_new( libspectrum_mmc_card, 1 );

  card->is_idle = 0;
  card->command_state = WAITING_FOR_COMMAND;
  card->response_buffer_next = card->response_buffer_end =
    card->response_buffer;

  card->cache = g_hash_table_new( g_int_hash, g_int_equal );

  return card;
}

void
libspectrum_mmc_free( libspectrum_mmc_card *card )
{
  libspectrum_mmc_eject( card );

  g_hash_table_destroy( card->cache );

  libspectrum_free( card );
}

libspectrum_error
libspectrum_mmc_insert( libspectrum_mmc_card *card, const char *filename )
{
  libspectrum_error error;
  libspectrum_dword total_sectors, c_size;

  libspectrum_mmc_eject( card );
  if( !filename ) return LIBSPECTRUM_ERROR_NONE;

  error = libspectrum_ide_insert_into_drive( &card->drive, filename );
  if( error ) return error;

  total_sectors = (libspectrum_dword)card->drive.cylinders *
    card->drive.heads * card->drive.sectors;

  /* We take C_SIZE_MULT to always be 7 for now, meaning we reduce
     the sector count by (7+2) = 9 bits. This gives us a minimum card size
     of 2^9 * 2^9 = 2^18 bytes = 256 Kb. Not too worried about that. */
  c_size = (total_sectors >> 9) - 1;

  card->c_size = c_size >= (1 << 12) ? (1 << 12) - 1 : c_size;

  return LIBSPECTRUM_ERROR_NONE;
}

void
libspectrum_mmc_eject( libspectrum_mmc_card *card )
{
  libspectrum_ide_eject_from_drive( &card->drive, card->cache );
}

int
libspectrum_mmc_dirty( libspectrum_mmc_card *card )
{
  return g_hash_table_size( card->cache ) != 0;
}

void
libspectrum_mmc_commit( libspectrum_mmc_card *card )
{
  libspectrum_ide_commit_drive( &card->drive, card->cache );
}

libspectrum_byte
libspectrum_mmc_read( libspectrum_mmc_card *card )
{
  libspectrum_byte r = card->response_buffer_next < card->response_buffer_end ?
    *(card->response_buffer_next)++ :
    0xff;

  printf("libspectrum_mmc_read() -> 0x%02x\n", r);

  return r;
}

static int
parse_command( libspectrum_mmc_card *card, libspectrum_byte b )
{
  libspectrum_byte command;
  int ok = 1;

  /* All commands should have start bit == 0 and transmitter bit == 1 */
  if( ( b & 0xc0 ) != 0x40 ) return 0;

  command = b & 0x3f;

  switch( command ) {
    case GO_IDLE_STATE:
    case SEND_IF_COND:
    case SEND_CSD:
    case SEND_CID:
    case READ_SINGLE_BLOCK:
    case WRITE_BLOCK:
    case APP_SEND_OP_COND:
    case APP_CMD:
    case READ_OCR:
      card->current_command = command;
      break;
    default:
      printf( "Unknown MMC command %d\n", command );
      abort();
      ok = 0;
      break;
  }

  return ok;
}

static void
set_response_buffer_r1( libspectrum_mmc_card *card )
{
  card->response_buffer[ 0 ] = card->is_idle;
  card->response_buffer_next = card->response_buffer;
  card->response_buffer_end = card->response_buffer + 1;
}

static void
set_response_buffer_r7( libspectrum_mmc_card *card, libspectrum_dword value )
{
  card->response_buffer[ 0 ] = card->is_idle;
  card->response_buffer[ 1 ] = ( value & 0xff000000 ) >> 24;
  card->response_buffer[ 2 ] = ( value & 0x00ff0000 ) >> 16;
  card->response_buffer[ 3 ] = ( value & 0x0000ff00 ) >>  8;
  card->response_buffer[ 4 ] = ( value & 0x000000ff );
  card->response_buffer_next = card->response_buffer;
  card->response_buffer_end = card->response_buffer + 5;
}

/* TODO: merge this with ide.c:read_hdf() */
static int
read_single_block( libspectrum_mmc_card *card )
{
  libspectrum_ide_drive *drv = &card->drive;
  GHashTable *cache = card->cache;
  libspectrum_byte *buffer, packed_buf[512];
  libspectrum_dword byte_offset, sector_number;

  byte_offset = 
    card->current_argument[ 3 ] +
    (card->current_argument[ 2 ] << 8) +
    (card->current_argument[ 1 ] << 16) +
    (card->current_argument[ 0 ] << 24);

  /* TODO: I don't believe this is right. The argument to READ_SINGLE_BLOCK
     is the byte offset, not the sector number, so we *should* be dividing
     this by 512. But this works and that doesn't - possibly a combination
     of bugs cancelling out?? */
  sector_number = byte_offset;

  /* First look in the write cache */
  buffer = g_hash_table_lookup( cache, &sector_number );

  /* If it's not in the write cache, read from the disk image */
  if( !buffer ) {

    long sector_position;

    sector_position =
      drv->data_offset + ( drv->sector_size * sector_number );

    /* Seek to the correct file position */
    if( fseek( drv->disk, sector_position, SEEK_SET ) ) return 1;

    /* Read the packed data into a temporary buffer */
    if ( fread( packed_buf, 1, drv->sector_size, drv->disk ) !=
	 drv->sector_size                                       )
      return 1;		/* read error */

    buffer = packed_buf;
  }

  /* Unpack or copy the data into the sector buffer */
  if( drv->sector_size == 256 ) {

    int i;
    
    for( i = 0; i < 256; i++ ) {
      card->response_buffer[ 2 + i * 2     ] = buffer[ i ];
      card->response_buffer[ 2 + i * 2 + 1 ] = 0xff;
    }

  } else {
    memcpy( &card->response_buffer[ 2 ], buffer, 512 );
  }
  
  card->response_buffer[ 0 ] = card->is_idle;
  card->response_buffer[ 1 ] = 0xfe;

  /* CRC */
  memset( &card->response_buffer[ 514 ], 0x00, 2 );

  card->response_buffer_next = card->response_buffer;
  card->response_buffer_end = card->response_buffer + 516;

  return 0;
}

static void
do_command( libspectrum_mmc_card *card )
{
  /* No card inserted => no change in state */
  if( !card->drive.disk ) return;

  switch( card->current_command ) {
    case GO_IDLE_STATE:
      printf("Executing GO_IDLE_STATE\n");
      card->is_idle = 1;
      set_response_buffer_r1( card );
      break;
    case SEND_IF_COND:
      printf("Executing SEND_IF_COND\n");
      set_response_buffer_r7( card, 0x000001aa );
      break;
    case SEND_CSD:
      printf("Executing SEND_CSD\n");
      card->response_buffer[ 0 ] = card->is_idle;
      card->response_buffer[ 1 ] = 0xfe;

      memset( &card->response_buffer[ 2 ], 0x00, 16 );
      /* READ_BL_LEN = 9 => 2 ^ 9 = 512 byte sectors */
      card->response_buffer[ 2 +  5 ] = 0x09;

      /* C_SIZE (spread 2 bits, 8 bits, 2 bits across three bytes) */
      card->response_buffer[ 2 +  6 ] = card->c_size >> 10;
      card->response_buffer[ 2 +  7 ] = (card->c_size >> 2) & 0xff;
      card->response_buffer[ 2 +  8 ] = (card->c_size & 0x03) << 6;

      /* C_SIZE_MULT = 7 => 2 ^ (2+7) = 512x size multiplier
         (spread 2 bits, 1 bit across two bytes) */
      card->response_buffer[ 2 +  9 ] = 0x03;
      card->response_buffer[ 2 + 10 ] = 0x80;

      /* CRC */
      memset( &card->response_buffer[ 18 ], 0x00, 2 );

      card->response_buffer_next = card->response_buffer;
      card->response_buffer_end = card->response_buffer + 20;
      break;
    case SEND_CID:
      printf("Executing SEND_CID\n");
      card->response_buffer[ 0 ] = card->is_idle;
      card->response_buffer[ 1 ] = 0xfe;

      /* TODO */
      memset( &card->response_buffer[ 2 ], 0x00, 16 );
      memset( &card->response_buffer[ 18 ], 0x00, 2 );

      card->response_buffer_next = card->response_buffer;
      card->response_buffer_end = card->response_buffer + 20;
      break;
    case READ_SINGLE_BLOCK:
      printf("Executing READ_SINGLE_BLOCK\n");
      if( read_single_block( card ) ) abort();
      break;
    case WRITE_BLOCK:
      printf("Executing WRITE_BLOCK\n");
      set_response_buffer_r1( card );
      break;
    case APP_SEND_OP_COND:
      printf("Executing APP_SEND_OP_COND\n");
      card->is_idle = 0;
      set_response_buffer_r1( card );
      break;
    case APP_CMD:
      printf("Executing APP_CMD\n");
      set_response_buffer_r1( card );
      break;
    case READ_OCR:
      printf("Executing READ_OCR\n");

      /* TODO */
      set_response_buffer_r7( card, 0xc0000000 );

      break;
    default:
      printf("Attempted to execute unknown MMC command %d\n", card->current_command );
      abort();
      break;
  }
}

static void
write_single_block( libspectrum_mmc_card *card )
{
  libspectrum_ide_drive *drv = &card->drive;
  GHashTable *cache = card->cache;
  libspectrum_byte *buffer;
  libspectrum_dword sector_number;

  sector_number =
    card->current_argument[ 3 ] +
    (card->current_argument[ 2 ] << 8) +
    (card->current_argument[ 1 ] << 16) +
    (card->current_argument[ 0 ] << 24);

  buffer = g_hash_table_lookup( cache, &sector_number );

  /* Add this sector to the write cache if it's not already present */
  if( !buffer ) {

    gint *key;

    key = libspectrum_new( gint, 1 );
    buffer = libspectrum_new( libspectrum_byte, drv->sector_size );

    *key = sector_number;
    g_hash_table_insert( cache, key, buffer );

  }

  /* Pack or copy the data into the write cache */
  if ( drv->sector_size == 256 ) {
    int i;
    for( i = 0; i < 256; i++ ) buffer[i] = card->send_buffer[ i * 2 ];
  } else {
    memcpy( buffer, card->send_buffer, 512 );
  }
}

static void
do_command_data( libspectrum_mmc_card *card )
{
  switch( card->current_command ) {
    case WRITE_BLOCK:
      printf("Handling WRITE_BLOCK data\n");
      write_single_block( card );
      card->response_buffer[ 0 ] = 0x05;
      card->response_buffer[ 1 ] = 0x05;
      card->response_buffer_next = card->response_buffer;
      card->response_buffer_end = card->response_buffer + 2;
      break;
    default:
      printf("Attempting to execute unknown MMC data command %d\n", card->current_command );
      abort();
  }
}

void
libspectrum_mmc_write( libspectrum_mmc_card *card, libspectrum_byte data )
{
  printf("libspectrum_mmc_write( 0x%02x )\n", data );

  switch( card->command_state ) {
    case WAITING_FOR_COMMAND:
      if( parse_command( card, data ) )
        card->command_state = WAITING_FOR_DATA0;
      break;
    case WAITING_FOR_DATA0:
    case WAITING_FOR_DATA1:
    case WAITING_FOR_DATA2:
    case WAITING_FOR_DATA3:
      card->current_argument[ card->command_state - 1 ] = data;
      card->command_state++;
      break;
    case WAITING_FOR_CRC:
      /* We ignore the CRC */
      do_command( card );
      card->command_state = card->current_command == WRITE_BLOCK ?
        WAITING_FOR_DATA_TOKEN :
        WAITING_FOR_COMMAND;
      break;
    case WAITING_FOR_DATA_TOKEN:
      if( data == 0xfe ) {
        card->command_state = WAITING_FOR_DATA;
        card->data_count = 0;
      }
      break;
    case WAITING_FOR_DATA:
      card->send_buffer[ card->data_count++ ] = data;
      if( card->data_count == 512 )
        card->command_state = WAITING_FOR_DATA_CRC1;
      break;
    case WAITING_FOR_DATA_CRC1:
      /* We ignore the data CRC as well */
      card->command_state = WAITING_FOR_DATA_CRC2;
      break;
    case WAITING_FOR_DATA_CRC2:
      do_command_data( card );
      card->command_state = WAITING_FOR_COMMAND;
      break;
    }
}
