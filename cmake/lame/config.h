/*
 * Build configuration for the LAME MP3 encoder, used instead of the one its configure script
 * would generate. Only the encoder library (libmp3lame) is built; the decoder (mpglib), the
 * command-line program and the hand-written assembly are left out.
 *
 * This file belongs to Tone of Hell (MIT license). LAME itself is LGPL; see
 * third_party/README.md.
 */
#ifndef TOH_LAME_CONFIG_H
#define TOH_LAME_CONFIG_H

#define STDC_HEADERS 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_LIMITS_H 1
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_STRCHR 1
#define HAVE_MEMCPY 1

#define PACKAGE "lame"
#define PROTOTYPES 1

/* A faster logarithm that is accurate enough for the psychoacoustic model. */
#define USE_FAST_LOG 1

#define LAME_LIBRARY_BUILD 1

#include <stdint.h>

typedef float ieee754_float32_t;
typedef double ieee754_float64_t;
typedef long double ieee854_float80_t;

#endif
