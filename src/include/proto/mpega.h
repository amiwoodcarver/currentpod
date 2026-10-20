/* Automatically generated header (sfdc 1.11f)! Do not edit! */

#ifndef PROTO_MPEGA_H
#define PROTO_MPEGA_H

#include <clib/mpega_protos.h>

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/mpega.h>
#  else
#   include <inline/mpega.h>
#  endif
# else
#  include <pragmas/mpega_pragmas.h>
# endif
#endif /* _NO_INLINE */

#ifdef __amigaos4__
# include <interfaces/mpega.h>
# ifndef __NOGLOBALIFACE__
   extern struct MPEGAIFace *IMPEGA;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern struct Library *
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  MPEGABase;
#endif /* !__NOLIBBASE__ */

#endif /* !PROTO_MPEGA_H */
