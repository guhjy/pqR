/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1998-2006 R Development Core Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, a copy is available at
 *  http://www.r-project.org/Licenses/
 */

/* NOTE:
   This file exports a part of the current internal parse interface.
   It is subject to change at any minor (x.y.0) version of R.
 */

#ifndef R_EXT_PARSE_H_
#define R_EXT_PARSE_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Possible status returns for R_ParseVector and R_ParseStream.  
   In actuality, only PARSE_OK and PARSE_ERROR are possible status
   returns from these functions, though the others except
   PARSE_INCOMPLETE are used for internal interfaces. */

typedef enum {
    PARSE_NULL,
    PARSE_OK,
    PARSE_INCOMPLETE,  /* No longer ever returned, but might be referenced */
    PARSE_ERROR,
    PARSE_EOF
} ParseStatus;

SEXP R_ParseVector(SEXP, int, ParseStatus *, SEXP);
SEXP R_ParseStream(int (*)(void *), void *, int, ParseStatus *, SEXP);

#ifdef __cplusplus
}
#endif

#endif
