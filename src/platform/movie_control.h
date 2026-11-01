#pragma once
#include <stdint.h>

/* The title's uMovie player thread publishes its own object through the
 * thread argument. These addresses are captured read-only from the locked
 * thread constructor in sdk_dso.c and let the video backend observe or
 * request movie stop. Zero means the thread has not been identified. */
extern volatile uint32_t vp_movie_umovie_thread;
extern volatile uint32_t vp_movie_umovie_state;
