/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2009, 2010  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GRUB_POSIX_STRING_H
#define GRUB_POSIX_STRING_H	1

#include <grub/misc.h>
#include <sys/types.h>

#define HAVE_STRCASECMP 1

static inline grub_size_t
strlen (const char *s)
{
  return grub_strlen (s);
}

static inline int
strcmp (const char *s1, const char *s2)
{
  return grub_strcmp (s1, s2);
}

static inline int
strcasecmp (const char *s1, const char *s2)
{
  return grub_strcasecmp (s1, s2);
}

static inline void
bcopy (const void *src, void *dest, grub_size_t n)
{
  grub_memcpy (dest, src, n);
}

static inline char *
strcpy (char *dest, const char *src)
{
  return grub_strcpy (dest, src);
}

static inline char *
strstr (const char *haystack, const char *needle)
{
  return grub_strstr (haystack, needle);
}

static inline char *
strchr (const char *s, int c)
{
  return grub_strchr (s, c);
}

static inline char *
strncpy (char *dest, const char *src, grub_size_t n)
{
  return grub_strncpy (dest, src, n);
}

static inline int
strcoll (const char *s1, const char *s2)
{
  return grub_strcmp (s1, s2);
}

static inline void *
memchr (const void *s, int c, grub_size_t n)
{
  return grub_memchr (s, c, n);
}

static inline char *
strncat (char *dest, const char *src, grub_size_t n)
{
  const char *end;
  char *str = dest;
  grub_size_t src_len;

  dest += grub_strlen (dest);

  end = grub_memchr (src, '\0', n);
  if (end != NULL)
    src_len = (grub_size_t) (end - src);
  else
    src_len = n;

  dest[src_len] = '\0';
  grub_memcpy (dest, src, src_len);

  return str;
}

#define memcmp grub_memcmp
#define memcpy grub_memcpy
#define memmove grub_memmove
#define memset grub_memset

#endif
