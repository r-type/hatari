/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2010 Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

#ifdef SDL_JOYSTICK_LIBRETRO
#include "../../video/libretro/libretro.h"

//extern retro_input_state_t input_state_cb;
extern short int libretro_input_state_cb(unsigned port,unsigned device,unsigned index,unsigned id);
//#define libretro_input_state_cb  input_state_cb
/* This is the system specific header for the SDL joystick API */

#include "SDL_events.h"
#include "SDL_joystick.h"
#include "../SDL_sysjoystick.h"
#include "../SDL_joystick_c.h"


#define pdprintf(x) SDL_SetError(x)

#define NAMESIZE 10

#define SDL_zero(x)	SDL_memset(&(x), 0, sizeof((x)))
#define SDL_zerop(x)	SDL_memset((x), 0, sizeof(*(x)))


typedef struct SDL_LIBRETRO_JoyData
{
	char name[NAMESIZE];
} SDL_LIBRETRO_JoyData;

struct joystick_hwdata
{
    char but[16];
    int an[4];// 0/1 left analog 2/3 right analog
};

static SDL_LIBRETRO_JoyData joy_data[4];

/* Function to scan the system for joysticks.
 * This function should set SDL_numjoysticks to the number of available
 * joysticks.  Joystick 0 should be the system default joystick.
 * It should return 0, or -1 on an unrecoverable fatal error.
 */
int
SDL_SYS_JoystickInit(void)
{
	int i = 0;
	SDL_numjoysticks = 4;

	SDL_zero( joy_data);


	for(i = 0; i < SDL_numjoysticks; i++)
	{

		{
			sprintf( joy_data[i].name, "PAD%02X", i);			
		} 
	}

	return SDL_numjoysticks;	
}

/* Function to get the device-dependent name of a joystick */
const char *
SDL_SYS_JoystickName(int index)
{
	char * name = NULL;
	if( index <  SDL_numjoysticks)
		name = joy_data[index].name;
	else
		SDL_SetError("No joystick available with that index");
    return name;
}

/* Function to open a joystick for use.
   The joystick to open is specified by the index field of the joystick.
   This should fill the nbuttons and naxes fields of the joystick structure.
   It returns 0, or -1 if there is an error.
 */
int
SDL_SYS_JoystickOpen(SDL_Joystick * joystick)
{
    if (!(joystick->hwdata = SDL_malloc(sizeof(struct joystick_hwdata))))
        return -1;
    int i;
    for(i=0;i<16;i++)joystick->hwdata->but[i]=0;

    joystick->hwdata->an[0]=0;
    joystick->hwdata->an[1]=0;
    joystick->hwdata->an[2]=0;
    joystick->hwdata->an[3]=0;

	joystick->naxes = 4;
	joystick->nhats = 0;
	joystick->nballs = 0;
	joystick->nbuttons = 16;

    return 0;
}


/* Function to update the state of a joystick - called as a device poll.
 * This function shouldn't update the joystick structure directly,
 * but instead should call SDL_PrivateJoystick*() to deliver events
 * and update joystick device state.
 */

void
SDL_SYS_JoystickUpdate(SDL_Joystick * joystick)
{
      int new_but[16];
      int new_an[4];
      int i;

      for(i=0;i<16;i++){
            new_but[i]=0;
           if( libretro_input_state_cb(joystick->index, RETRO_DEVICE_JOYPAD, 0, i))
                new_but[i]=1;
      }            

      for(i=0;i<16;i++){

            if( new_but[i] != joystick->hwdata->but[i]) {
                if( new_but[i]== 0){ 
		            SDL_PrivateJoystickButton( joystick, (i), SDL_RELEASED); 
                }
		        else {
		            SDL_PrivateJoystickButton( joystick, (i), SDL_PRESSED); 
                }
            }
            joystick->hwdata->but[i]=new_but[i];
      }

      new_an[0] = (libretro_input_state_cb(joystick->index, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_X));
      new_an[1] = (libretro_input_state_cb(joystick->index, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,  RETRO_DEVICE_ID_ANALOG_Y));
      new_an[2] = (libretro_input_state_cb(joystick->index, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X));
      new_an[3] = (libretro_input_state_cb(joystick->index, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y));

      for(i=0;i<4;i++){
	        if( new_an[i] != joystick->hwdata->an[i]) {
		        SDL_PrivateJoystickAxis( joystick, i, new_an[i]); 
	        } 
	        joystick->hwdata->an[i] = new_an[i];
      }



    return;
} 

/* Function to close a joystick after use */
void
SDL_SYS_JoystickClose(SDL_Joystick * joystick)
{
    if (joystick->hwdata)
        SDL_free(joystick->hwdata);
    return;
}

/* Function to perform any system-specific joystick related cleanup */
void
SDL_SYS_JoystickQuit(void)
{
    SDL_numjoysticks = 0;
    return;
}

#endif /* SDL_JOYSTICK_LIBRETRO */

/* vi: set ts=4 sw=4 expandtab: */
