/* Automatically generated header (sfdc 1.11f)! Do not edit! */

#ifndef _INLINE_MPEGA_H
#define _INLINE_MPEGA_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef MPEGA_BASE_NAME
#define MPEGA_BASE_NAME MPEGABase
#endif /* !MPEGA_BASE_NAME */

#define MPEGA_open(___filename, ___ctrl) \
      LP2(0x1e, MPEGA_STREAM *, MPEGA_open , char *, ___filename, a0, MPEGA_CTRL *, ___ctrl, a1,\
      , MPEGA_BASE_NAME)

#define MPEGA_close(___mpds) \
      LP1NR(0x24, MPEGA_close , MPEGA_STREAM *, ___mpds, a0,\
      , MPEGA_BASE_NAME)

#define MPEGA_decode_frame(___mpds, ___pcm) \
      LP2(0x2a, LONG, MPEGA_decode_frame , MPEGA_STREAM *, ___mpds, a0, WORD **, ___pcm, a1,\
      , MPEGA_BASE_NAME)

#define MPEGA_seek(___mpds, ___ms_time_position) \
      LP2(0x30, LONG, MPEGA_seek , MPEGA_STREAM *, ___mpds, a0, ULONG, ___ms_time_position, d0,\
      , MPEGA_BASE_NAME)

#define MPEGA_time(___mpds, ___ms_time_position) \
      LP2(0x36, LONG, MPEGA_time , MPEGA_STREAM *, ___mpds, a0, ULONG *, ___ms_time_position, a1,\
      , MPEGA_BASE_NAME)

#define MPEGA_find_sync(___buffer, ___buffer_size) \
      LP2(0x3c, LONG, MPEGA_find_sync , BYTE *, ___buffer, a0, LONG, ___buffer_size, d0,\
      , MPEGA_BASE_NAME)

#define MPEGA_scale(___mpds, ___scale_percent) \
      LP2(0x42, LONG, MPEGA_scale , MPEGA_STREAM *, ___mpds, a0, LONG, ___scale_percent, d0,\
      , MPEGA_BASE_NAME)

#endif /* !_INLINE_MPEGA_H */
