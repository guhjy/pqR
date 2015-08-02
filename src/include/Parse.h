/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1998-2005 R Development Core Team
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

#ifndef R_PARSE_H
#define R_PARSE_H

#include <R_ext/Parse.h>
#include <IOStuff.h>

/* Public interface */
/* SEXP R_ParseVector(SEXP, int, ParseStatus *, SEXP); in R_ext/Parse.h */

/* Private interface */

typedef struct {

    Rboolean keepSrcRefs;	/* Whether to attach srcrefs to objects as they are parsed */
    Rboolean didAttach;		/* Record of whether a srcref was attached */
    SEXP SrcFile;		/* The srcfile object currently being parsed */
    SEXP Original;		/* The underlying srcfile object */
    PROTECT_INDEX SrcFileProt;	/* The SrcFile may change */
    PROTECT_INDEX OriginalProt; /* ditto */
    				/* Position information about the current parse */
    int xxlineno;		/* Line number according to #line directives */
    int xxcolno;		/* Character number on line */
    int xxbyteno;		/* Byte number on line */
    int xxparseno;              /* Line number ignoring #line directives */
} SrcRefState;

void R_InitSrcRefState(SrcRefState *state);
void R_FinalizeSrcRefState(SrcRefState *state);

SEXP R_Parse1Stream (int (*)(void *), void *, ParseStatus *, SrcRefState *);

	/* Report a parse error */
	
void parseError(SEXP call, int linenum);

#endif /* not R_PARSE_H */
