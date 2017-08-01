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
  WAITING_FOR_CRC
};

/* The MMC commands we support */
enum command_byte {
  GO_IDLE_STATE = 0,
  SEND_IF_COND = 8,
  SEND_CSD = 9,
  SEND_CID = 10,
  READ_SINGLE_BLOCK = 17,
  APP_SEND_OP_COND = 41,
  APP_CMD = 55,
  READ_OCR = 58
};

typedef struct libspectrum_mmc_card {
  /* The actual "card" data */
  libspectrum_ide_drive drive;

  /* Cache of written sectors */
  GHashTable *cache;

  /* Is the MMC interface currently idle? */
  int is_idle;

  /* The current state of the command being transmitted to the card */
  enum command_state_t command_state;

  /* The most recent command byte sent to the MMC */
  libspectrum_byte current_command;

  /* The argument for the current MMC command */
  libspectrum_byte current_argument[4];

  /* The response to the most recent command */
  libspectrum_byte response_buffer[20];

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
  libspectrum_mmc_eject( card );
  if ( !filename ) return LIBSPECTRUM_ERROR_NONE;

  return libspectrum_ide_insert_into_drive( &card->drive, filename );
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

      /* TODO */
      memset( &card->response_buffer[ 2 ], 0x00, 16 );
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

void
libspectrum_mmc_write( libspectrum_mmc_card *card, libspectrum_byte data )
{
  int ok = 1;

  printf("libspectrum_mmc_write( 0x%02x )\n", data );

  switch( card->command_state ) {
    case WAITING_FOR_COMMAND:
      ok = parse_command( card, data );
      break;
    case WAITING_FOR_DATA0:
    case WAITING_FOR_DATA1:
    case WAITING_FOR_DATA2:
    case WAITING_FOR_DATA3:
      card->current_argument[ card->command_state - 1 ] = data;
      break;
    case WAITING_FOR_CRC:
      /* We ignore the CRC */
      break;
    }

  if( ok ) {
    if( card->command_state == WAITING_FOR_CRC ) {
      do_command( card );
      card->command_state = WAITING_FOR_COMMAND;
    } else {
      card->command_state++;
    }
  } else {
    /* TODO: Error handling */
  }
}
