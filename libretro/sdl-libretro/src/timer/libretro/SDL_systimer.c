/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997-2012 Sam Lantinga

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

#ifdef SDL_TIMER_LIBRETRO

#include <stdio.h>
#include <sys/time.h>
//#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "SDL_timer.h"
#include "../SDL_timer_c.h"

/* The clock_gettime provides monotonous time, so we should use it if
   it's available. The clock_gettime function is behind ifdef
   for __USE_POSIX199309
   Tommi Kyntola (tommi.kyntola@ray.fi) 27/09/2005
*/
#if HAVE_NANOSLEEP || HAVE_CLOCK_GETTIME
#include <time.h>
#endif

#if SDL_THREAD_PTH
#include <pth.h>
#endif

#if SDL_THREADS_DISABLED
#define USE_ITIMER
#endif

/* The first ticks value of the application */
#ifdef HAVE_CLOCK_GETTIME
static struct timespec start;
#elif defined(WIIU)
#include <features_cpu.h>
static unsigned long start;
#else
static struct timeval start;
#endif /* HAVE_CLOCK_GETTIME */


void SDL_StartTicks(void)
{
	/* Set first ticks value */
#if HAVE_CLOCK_GETTIME
	clock_gettime(CLOCK_MONOTONIC,&start);
#elif defined(WIIU)
	start=cpu_features_get_time_usec();
#else
	gettimeofday(&start, NULL);
#endif
}

Uint32 SDL_GetTicks (void)
{
#if HAVE_CLOCK_GETTIME
	Uint32 ticks;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC,&now);
	ticks=(now.tv_sec-start.tv_sec)*1000+(now.tv_nsec-start.tv_nsec)/1000000;
	return(ticks);
#elif defined(WIIU)
        return (cpu_features_get_time_usec()-start)/1000;
#else
	Uint32 ticks;
	struct timeval now;
	gettimeofday(&now, NULL);
	ticks=(now.tv_sec-start.tv_sec)*1000+(now.tv_usec-start.tv_usec)/1000;
	return(ticks);
#endif
}

void SDL_Delay (Uint32 ms)
{
#warning DELAY LIBRETRO
#if SDL_THREAD_PTH
	pth_time_t tv;
	tv.tv_sec  =  ms/1000;
	tv.tv_usec = (ms%1000)*1000;
	pth_nap(tv);
#else
	int was_error;

#if HAVE_NANOSLEEP
	struct timespec elapsed, tv;
#else
	struct timeval tv;
	Uint32 then, now, elapsed;
#endif

	/* Set the timeout interval */
#if HAVE_NANOSLEEP
	elapsed.tv_sec = ms/1000;
	elapsed.tv_nsec = (ms%1000)*1000000;
#else
	then = SDL_GetTicks();
#endif
	do {
		errno = 0;

#if HAVE_NANOSLEEP
		tv.tv_sec = elapsed.tv_sec;
		tv.tv_nsec = elapsed.tv_nsec;
		was_error = nanosleep(&tv, &elapsed);
#else
		/* Calculate the time interval left (in case of interrupt) */
		now = SDL_GetTicks();
		elapsed = (now-then);
		then = now;
		if ( elapsed >= ms ) {
			break;
		}
		ms -= elapsed;
		tv.tv_sec = ms/1000;
		tv.tv_usec = (ms%1000)*1000;

		was_error = select(0, NULL, NULL, NULL, &tv);
#endif /* HAVE_NANOSLEEP */
	} while ( was_error && (errno == EINTR) );
#endif /* SDL_THREAD_PTH */
}

#include "SDL_thread.h"

/* Data to handle a single periodic alarm */
static int timer_alive = 0;
static SDL_Thread *timer = NULL;

static int RunTimer(void *unused)
{
	while ( timer_alive ) {
		if ( SDL_timer_running ) {
			SDL_ThreadedTimerCheck();
		}
		SDL_Delay(1);
	}
	return(0);
}

/* This is only called if the event thread is not running */
int SDL_SYS_TimerInit(void)
{
	timer_alive = 1;
	timer = SDL_CreateThread(RunTimer, NULL);
	if ( timer == NULL )
		return(-1);
	return(SDL_SetTimerThreaded(1));
}

void SDL_SYS_TimerQuit(void)
{
	timer_alive = 0;
	if ( timer ) {
		SDL_WaitThread(timer, NULL);
		timer = NULL;
	}
}

int SDL_SYS_StartTimer(void)
{
	SDL_SetError("Internal logic error: Linux uses threaded timer");
	return(-1);
}

void SDL_SYS_StopTimer(void)
{
	return;
}

#endif /* SDL_TIMER_UNIX */
