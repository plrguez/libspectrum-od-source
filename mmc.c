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

  card->cache = g_hash_table_new( g_int_hash, g_int_equal );

  libspectrum_mmc_reset( card );

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

  if( card->drive.sector_size != 512 || total_sectors % 1024 ) {
      libspectrum_print_error( LIBSPECTRUM_ERROR_UNKNOWN,
        "Image size not supported" );
    return LIBSPECTRUM_ERROR_UNKNOWN;
  }

  /* memory capacity = (C_SIZE+1) * 512K bytes
     we reduce the sector count by 1024. This gives us a minimum card size
     of 512 Kb. Not too worried about that. */
  c_size = (total_sectors >> 10) - 1;

  /* We emulate a SDHC card which has a maximum size of (32 Gb - 80 Mb) */
  card->c_size = c_size >= 65375 ? 65375 : c_size;

  return LIBSPECTRUM_ERROR_NONE;
}

void
libspectrum_mmc_eject( libspectrum_mmc_card *card )
{
  libspectrum_ide_eject_from_drive( &card->drive, card->cache );
}

void
libspectrum_mmc_reset( libspectrum_mmc_card *card )
{
  card->is_idle = 0;
  card->command_state = WAITING_FOR_COMMAND;
  card->response_buffer_next = card->response_buffer;
  card->response_buffer_end = card->response_buffer;
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
      libspectrum_print_error(
          LIBSPECTRUM_ERROR_UNKNOWN, "Unknown MMC command %d received",
          command );
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

static void
read_single_block( libspectrum_mmc_card *card )
{
  libspectrum_dword sector_number = 
    card->current_argument[ 3 ] +
    (card->current_argument[ 2 ] << 8) +
    (card->current_argument[ 1 ] << 16) +
    (card->current_argument[ 0 ] << 24);

  int error = libspectrum_ide_read_sector_from_hdf(
      &card->drive, card->cache, sector_number, &card->response_buffer[ 2 ]
  );
  if( error ) return;

  card->response_buffer[ 0 ] = card->is_idle;
  card->response_buffer[ 1 ] = 0xfe;

  /* CRC */
  memset( &card->response_buffer[ 514 ], 0x00, 2 );

  card->response_buffer_next = card->response_buffer;
  card->response_buffer_end = card->response_buffer + 516;
}

static void
do_command( libspectrum_mmc_card *card )
{
  /* No card inserted => no change in state */
  if( !card->drive.disk ) return;

  switch( card->current_command ) {
    case GO_IDLE_STATE:
      card->is_idle = 1;
      set_response_buffer_r1( card );
      break;
    case SEND_IF_COND:
      /* return echo back pattern */
      set_response_buffer_r7( card, 0x00000100 | card->current_argument[ 3 ] );
      break;
    case SEND_CSD:
      card->response_buffer[ 0 ] = card->is_idle; /* R1 command response */
      card->response_buffer[ 1 ] = 0xfe;          /* data token */

      memset( &card->response_buffer[ 2 ], 0x00, 16 );

      /* CSD_STRUCTURE version 2.0 */
      card->response_buffer[ 2 ] = 0x40;

      /* READ_BL_LEN = 9 => 2 ^ 9 = 512 byte sectors */
      card->response_buffer[ 2 +  5 ] = 0x09;

      /* C_SIZE (spread 6 bits, 8 bits, 8 bits across three bytes),
         first 6 bits set to zero */
      card->response_buffer[ 2 +  8 ] = ( card->c_size >> 8 ) & 0xff;
      card->response_buffer[ 2 +  9 ] = card->c_size & 0xff;

      /* WRITE_BL_LEN = 9 => 2 ^ 9 = 512 byte sectors
         (spread 2 bits, 2 bits across two bytes) */
      card->response_buffer[ 2 +  12 ] = 0x10;
      card->response_buffer[ 2 +  13 ] = 0x01;

      /* Bit 0, not used, always 1 */
      card->response_buffer[ 2 +  15 ] = 0x01;

      /* CRC */
      memset( &card->response_buffer[ 18 ], 0x00, 2 );

      card->response_buffer_next = card->response_buffer;
      card->response_buffer_end = card->response_buffer + 20;
      break;
    case SEND_CID:
      card->response_buffer[ 0 ] = card->is_idle; /* R1 command response */
      card->response_buffer[ 1 ] = 0xfe;          /* data token */

      /* For now, we return an empty CID. This seems to work. */
      memset( &card->response_buffer[ 2 ], 0x00, 16 );

      /* Bit 0, not used, always 1 */
      card->response_buffer[ 2 +  15 ] = 0x01;

      /* CRC */
      memset( &card->response_buffer[ 18 ], 0x00, 2 );

      card->response_buffer_next = card->response_buffer;
      card->response_buffer_end = card->response_buffer + 20;
      break;
    case READ_SINGLE_BLOCK:
      read_single_block( card );
      break;
    case WRITE_BLOCK:
      set_response_buffer_r1( card );
      break;
    case APP_SEND_OP_COND:
      card->is_idle = 0;
      set_response_buffer_r1( card );
      break;
    case APP_CMD:
      set_response_buffer_r1( card );
      break;
    case READ_OCR:
      /* We set only the card capacity status (CCS, bit 30) and card power up
         status bits (bit 31). CCS set indicates an SDHC card. */
      set_response_buffer_r7( card, 0xc0000000 );
      break;
    default:
      /* This should never happen as we've already filtered the commands in
         parse_command() */
      libspectrum_print_error(
          LIBSPECTRUM_ERROR_LOGIC,
          "Attempted to execute unknown MMC command %d\n",
          card->current_command );
      break;
  }
}

static void
write_single_block( libspectrum_mmc_card *card )
{
  libspectrum_dword sector_number =
    card->current_argument[ 3 ] +
    (card->current_argument[ 2 ] << 8) +
    (card->current_argument[ 1 ] << 16) +
    (card->current_argument[ 0 ] << 24);

  libspectrum_ide_write_sector_to_hdf( &card->drive, card->cache, sector_number, card->send_buffer );
}

static void
do_command_data( libspectrum_mmc_card *card )
{
  switch( card->current_command ) {
    case WRITE_BLOCK:
      write_single_block( card );
      card->response_buffer[ 0 ] = 0x05;
      card->response_buffer[ 1 ] = 0x05;
      card->response_buffer_next = card->response_buffer;
      card->response_buffer_end = card->response_buffer + 2;
      break;
    default:
      /* This should never happen as it indicates a failure in our state machine */
      libspectrum_print_error(
          LIBSPECTRUM_ERROR_LOGIC,
          "Attempting to execute unknown MMC data command %d\n",
          card->current_command );
      break;
  }
}

void
libspectrum_mmc_write( libspectrum_mmc_card *card, libspectrum_byte data )
{
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
