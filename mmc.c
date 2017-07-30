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

/* Is the MMC interface currently idle? */
static int is_idle;

/* The most recent command byte sent to the MMC */
static libspectrum_byte current_command;

/* The argument for the current MMC command */
static libspectrum_byte current_argument[4];

/* The response to the most recent command */
static libspectrum_byte response_buffer[20];

/* One past the last valid byte in response_buffer */
static libspectrum_byte *response_buffer_end;

/* The next byte to be returned from response_buffer */
static libspectrum_byte *response_buffer_next;

enum command_state_t {
  WAITING_FOR_COMMAND,
  WAITING_FOR_DATA0,
  WAITING_FOR_DATA1,
  WAITING_FOR_DATA2,
  WAITING_FOR_DATA3,
  WAITING_FOR_CRC
};

enum command_state_t command_state;

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

void
libspectrum_mmc_card_select( libspectrum_byte data )
{
  printf("libspectrum_mmc_card_select( 0x%02x )\n", data );

  command_state = WAITING_FOR_COMMAND;
  response_buffer_next = response_buffer_end = response_buffer;
  is_idle = 0;
}

libspectrum_byte
libspectrum_mmc_read( void )
{
  libspectrum_byte r = response_buffer_next < response_buffer_end ?
    *response_buffer_next++ :
    0xff;

  printf("libspectrum_mmc_read() -> 0x%02x\n", r);

  return r;
}

static int
parse_command( libspectrum_byte b )
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
      current_command = command;
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
set_response_buffer_r1( void )
{
  response_buffer[ 0 ] = is_idle;
  response_buffer_next = response_buffer;
  response_buffer_end = response_buffer + 1;
}

static void
set_response_buffer_r7( libspectrum_dword value )
{
  response_buffer[ 0 ] = is_idle;
  response_buffer[ 1 ] = ( value & 0xff000000 ) >> 24;
  response_buffer[ 2 ] = ( value & 0x00ff0000 ) >> 16;
  response_buffer[ 3 ] = ( value & 0x0000ff00 ) >>  8;
  response_buffer[ 4 ] = ( value & 0x000000ff );
  response_buffer_next = response_buffer;
  response_buffer_end = response_buffer + 5;
}

static void
do_command( void )
{
  switch( current_command ) {
    case GO_IDLE_STATE:
      printf("Executing GO_IDLE_STATE\n");
      set_response_buffer_r1();
      break;
    case SEND_IF_COND:
      printf("Executing SEND_IF_COND\n");
      set_response_buffer_r7( 0x000001aa );
      break;
    case SEND_CSD:
      printf("Executing SEND_CSD\n");
      response_buffer[ 0 ] = is_idle;
      response_buffer[ 1 ] = 0xfe;

      /* TODO */
      memset( &response_buffer[ 2 ], 0x00, 16 );
      memset( &response_buffer[ 18 ], 0x00, 2 );

      response_buffer_next = response_buffer;
      response_buffer_end = response_buffer + 20;
      break;
    case SEND_CID:
      printf("Executing SEND_CID\n");
      response_buffer[ 0 ] = is_idle;
      response_buffer[ 1 ] = 0xfe;

      /* TODO */
      memset( &response_buffer[ 2 ], 0x00, 16 );
      memset( &response_buffer[ 18 ], 0x00, 2 );

      response_buffer_next = response_buffer;
      response_buffer_end = response_buffer + 20;
      break;
    case APP_SEND_OP_COND:
      printf("Executing APP_SEND_OP_COND\n");
      is_idle = 0;
      set_response_buffer_r1();
      break;
    case APP_CMD:
      printf("Executing APP_CMD\n");
      set_response_buffer_r1();
      break;
    case READ_OCR:
      printf("Executing READ_OCR\n");

      /* TODO */
      set_response_buffer_r7( 0xc0000000 );

      break;
    default:
      printf("Attempted to execute unknown MMC command %d\n", current_command );
      abort();
      break;
  }
}

void
libspectrum_mmc_write( libspectrum_byte data )
{
  int ok = 1;

  printf("libspectrum_mmc_write( 0x%02x )\n", data );

  switch( command_state ) {
    case WAITING_FOR_COMMAND:
      ok = parse_command( data );
      break;
    case WAITING_FOR_DATA0:
    case WAITING_FOR_DATA1:
    case WAITING_FOR_DATA2:
    case WAITING_FOR_DATA3:
      current_argument[ command_state - 1 ] = data;
      break;
    case WAITING_FOR_CRC:
      /* We ignore the CRC */
      break;
    }

  if( ok ) {
    if( command_state == WAITING_FOR_CRC ) {
      do_command();
      command_state = WAITING_FOR_COMMAND;
    } else {
      command_state++;
    }
  } else {
    /* TODO: Error handling */
  }
}
