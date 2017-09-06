/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2017 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1998-2009   The R Core Team
 *  Copyright (C) 2004        The R Foundation
 *
 *  The changes in pqR from R-2.15.0 distributed by the R Core Team are
 *  documented in the NEWS and MODS files in the top-level source directory.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define USE_FAST_PROTECT_MACROS
#include <Defn.h> /* => Utils.h with the protos from here */
#include <Rmath.h>
#include <R_ext/RS.h>  /* for Calloc/Free */

			/*--- Part I: Comparison Utilities ---*/

static int icmp(int x, int y, Rboolean nalast)
{
    if (x == NA_INTEGER && y == NA_INTEGER) return 0;
    if (x == NA_INTEGER)return nalast?1:-1;
    if (y == NA_INTEGER)return nalast?-1:1;
    if (x < y)		return -1;
    if (x > y)		return 1;
    return 0;
}

static int rcmp(double x, double y, Rboolean nalast)
{
    int nax = ISNAN(x), nay = ISNAN(y);
    if (nax && nay)	return 0;
    if (nax)		return nalast?1:-1;
    if (nay)		return nalast?-1:1;
    if (x < y)		return -1;
    if (x > y)		return 1;
    return 0;
}

static int ccmp(Rcomplex x, Rcomplex y, Rboolean nalast)
{
    int nax = ISNAN(x.r), nay = ISNAN(y.r);
				/* compare real parts */
    if (nax && nay)	return 0;
    if (nax)		return nalast?1:-1;
    if (nay)		return nalast?-1:1;
    if (x.r < y.r)	return -1;
    if (x.r > y.r)	return 1;
				/* compare complex parts */
    nax = ISNAN(x.i); nay = ISNAN(y.i);
    if (nax && nay)	return 0;
    if (nax)		return nalast?1:-1;
    if (nay)		return nalast?-1:1;
    if (x.i < y.i)	return -1;
    if (x.i > y.i)	return 1;

    return 0;		/* equal */
}

static int scmp(SEXP x, SEXP y, Rboolean nalast)
{
    if (x == NA_STRING && y == NA_STRING) return 0;
    if (x == NA_STRING) return nalast?1:-1;
    if (y == NA_STRING) return nalast?-1:1;
    if (x == y) return 0;  /* same string in cache */
    return Scollate(x, y);
}

Rboolean isUnsorted(SEXP x, Rboolean strictly)
{
    int n, i;

    if (!isVectorAtomic(x))
	error(_("only atomic vectors can be tested to be sorted"));
    n = LENGTH(x);
    if(n >= 2)
	switch (TYPEOF(x)) {

	    /* NOTE: x must have no NAs {is.na(.) in R};
	       hence be faster than `rcmp()', `icmp()' for these two cases */

	    /* The only difference between strictly and not is '>' vs '>='
	       but we want the if() outside the loop */
	case LGLSXP:
	case INTSXP:
	    if(strictly) {
		for(i = 0; i+1 < n ; i++)
		    if(INTEGER(x)[i] >= INTEGER(x)[i+1])
			return TRUE;

	    } else {
		for(i = 0; i+1 < n ; i++)
		    if(INTEGER(x)[i] > INTEGER(x)[i+1])
			return TRUE;
	    }
	    break;
	case REALSXP:
	    if(strictly) {
		for(i = 0; i+1 < n ; i++)
		    if(REAL(x)[i] >= REAL(x)[i+1])
			return TRUE;
	    } else {
		for(i = 0; i+1 < n ; i++)
		    if(REAL(x)[i] > REAL(x)[i+1])
			return TRUE;
	    }
	    break;
	case CPLXSXP:
	    if(strictly) {
		for(i = 0; i+1 < n ; i++)
		    if(ccmp(COMPLEX(x)[i], COMPLEX(x)[i+1], TRUE) >= 0)
			return TRUE;
	    } else {
		for(i = 0; i+1 < n ; i++)
		    if(ccmp(COMPLEX(x)[i], COMPLEX(x)[i+1], TRUE) > 0)
			return TRUE;
	    }
	    break;
	case STRSXP:
	    if(strictly) {
		for(i = 0; i+1 < n ; i++)
		    if(scmp(STRING_ELT(x, i ),
			    STRING_ELT(x,i+1), TRUE) >= 0)
			return TRUE;
	    } else {
		for(i = 0; i+1 < n ; i++)
		    if(scmp(STRING_ELT(x, i ),
			    STRING_ELT(x,i+1), TRUE) > 0)
			return TRUE;
	    }
	    break;
	default:
	    UNIMPLEMENTED_TYPE("isUnsorted", x);
	}
    return FALSE;/* sorted */
}

static SEXP do_isunsorted(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int strictly, n;
    SEXP x, ans;
    int res = TRUE;

    checkArity(op, args);
    x = CAR(args);
    strictly = asLogical(CADR(args));
    if(strictly == NA_LOGICAL)
	errorcall(call, _("invalid '%s' argument"), "strictly");
    n = length(x);
    if(n < 2) return ScalarLogicalMaybeConst(FALSE);
    if(isVectorAtomic(x))
	return ScalarLogicalMaybeConst(isUnsorted(x, strictly));
    if(isObject(x)) {
	/* try dispatch */
	SEXP call;
	PROTECT(call = lang3(install(".gtn"), x, CADR(args)));
	ans = eval(call, rho);
	UNPROTECT(1);
	return ans;
    } else res = NA_LOGICAL;
    return ScalarLogicalMaybeConst(res);
}


			/*--- Part II: Complete (non-partial) Sorting ---*/


/* SHELLsort -- corrected from R. Sedgewick `Algorithms in C'
 *		(version of BDR's lqs():*/
#define sort_body					\
    Rboolean nalast=TRUE;				\
    int i, j, h;					\
							\
    for (h = 1; h <= n / 9; h = 3 * h + 1);		\
    for (; h > 0; h /= 3)				\
	for (i = h; i < n; i++) {			\
	    v = x[i];					\
	    j = i;					\
	    while (j >= h && TYPE_CMP(x[j - h], v, nalast) > 0)	\
		 { x[j] = x[j - h]; j -= h; }		\
	    x[j] = v;					\
	}

void R_isort(int *x, int n)
{
    int v;
#define TYPE_CMP icmp
    sort_body
#undef TYPE_CMP
}

void R_rsort(double *x, int n)
{
    double v;
#define TYPE_CMP rcmp
    sort_body
#undef TYPE_CMP
}

void R_csort(Rcomplex *x, int n)
{
    Rcomplex v;
#define TYPE_CMP ccmp
    sort_body
#undef TYPE_CMP
}


/* used in platform.c */
void attribute_hidden ssort(SEXP *x, int n)
{
    SEXP v;
#define TYPE_CMP scmp
    sort_body
#undef TYPE_CMP
}

void rsort_with_index(double *x, int *indx, int n)
{
    double v;
    int i, j, h, iv;

    for (h = 1; h <= n / 9; h = 3 * h + 1);
    for (; h > 0; h /= 3)
	for (i = h; i < n; i++) {
	    v = x[i]; iv = indx[i];
	    j = i;
	    while (j >= h && rcmp(x[j - h], v, TRUE) > 0)
		 { x[j] = x[j - h]; indx[j] = indx[j-h]; j -= h; }
	    x[j] = v; indx[j] = iv;
	}
}

void revsort(double *a, int *ib, int n)
{
/* Sort a[] into descending order by "heapsort";
 * sort ib[] alongside;
 * if initially, ib[] = 1...n, it will contain the permutation finally
 */

    int l, j, ir, i;
    double ra;
    int ii;

    if (n <= 1) return;

    a--; ib--;

    l = (n >> 1) + 1;
    ir = n;

    for (;;) {
	if (l > 1) {
	    l = l - 1;
	    ra = a[l];
	    ii = ib[l];
	}
	else {
	    ra = a[ir];
	    ii = ib[ir];
	    a[ir] = a[1];
	    ib[ir] = ib[1];
	    if (--ir == 1) {
		a[1] = ra;
		ib[1] = ii;
		return;
	    }
	}
	i = l;
	j = l << 1;
	while (j <= ir) {
	    if (j < ir && a[j] > a[j + 1]) ++j;
	    if (ra > a[j]) {
		a[i] = a[j];
		ib[i] = ib[j];
		j += (i = j);
	    }
	    else
		j = ir + 1;
	}
	a[i] = ra;
	ib[i] = ii;
    }
}


static SEXP do_sort(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP ans;
    int decreasing;

    checkArity(op, args);

    decreasing = asLogical(CADR(args));
    if(decreasing == NA_LOGICAL)
	error(_("'decreasing' must be TRUE or FALSE"));
    if(CAR(args) == R_NilValue) return R_NilValue;
    if(!isVectorAtomic(CAR(args)))
	error(_("only atomic vectors can be sorted"));
    if(TYPEOF(CAR(args)) == RAWSXP)
	error(_("raw vectors cannot be sorted"));
    /* we need consistent behaviour here, including dropping attibutes,
       so as from 2.3.0 we always duplicate. */
    PROTECT(ans = duplicate(CAR(args)));
    SET_ATTRIB(ans, R_NilValue);  /* this is never called with names */
    SET_OBJECT(ans, 0);		  /* we may have just stripped off the class */
    sortVector(ans, decreasing);
    UNPROTECT(1);
    return(ans);
}

/* faster versions of shellsort, following Sedgewick (1986) */

static const int incs[] = {1073790977, 268460033, 67121153, 16783361, 4197377,
		       1050113, 262913, 65921, 16577, 4193, 1073, 281, 77,
		       23, 8, 1, 0};

#define sort2_body \
    for (h = incs[t]; h != 0; h = incs[++t]) \
	for (i = h; i < n; i++) { \
	    v = x[i]; \
	    j = i; \
	    while (j >= h && x[j - h] less v) { x[j] = x[j - h]; j -= h; } \
	    x[j] = v; \
	}


static void R_isort2(int *x, int n, Rboolean decreasing)
{
    int v;
    int i, j, h, t;

    for (t = 0; incs[t] > n; t++);
    if(decreasing)
#define less <
	sort2_body
#undef less
    else
#define less >
	sort2_body
#undef less
}

static void R_rsort2(double *x, int n, Rboolean decreasing)
{
    double v;
    int i, j, h, t;

    for (t = 0; incs[t] > n; t++);
    if(decreasing)
#define less <
	sort2_body
#undef less
    else
#define less >
	sort2_body
#undef less
}

static void R_csort2(Rcomplex *x, int n, Rboolean decreasing)
{
    Rcomplex v;
    int i, j, h, t;

    for (t = 0; incs[t] > n; t++);
    for (h = incs[t]; h != 0; h = incs[++t])
	for (i = h; i < n; i++) {
	    v = x[i];
	    j = i;
	    if(decreasing)
		while (j >= h && (x[j - h].r < v.r ||
				  (x[j - h].r == v.r && x[j - h].i < v.i)))
		{ x[j] = x[j - h]; j -= h; }
	    else
		while (j >= h && (x[j - h].r > v.r ||
				  (x[j - h].r == v.r && x[j - h].i > v.i)))
		{ x[j] = x[j - h]; j -= h; }
	    x[j] = v;
	}
}

static void ssort2(SEXP *x, int n, Rboolean decreasing)
{
    SEXP v;
    int i, j, h, t;

    for (t = 0; incs[t] > n; t++);
    for (h = incs[t]; h != 0; h = incs[++t])
	for (i = h; i < n; i++) {
	    v = x[i];
	    j = i;
	    if(decreasing)
		while (j >= h && scmp(x[j - h], v, TRUE) < 0)
		{ x[j] = x[j - h]; j -= h; }
	    else
		while (j >= h && scmp(x[j - h], v, TRUE) > 0)
		{ x[j] = x[j - h]; j -= h; }
	    x[j] = v;
	}
}

void sortVector(SEXP s, Rboolean decreasing)
{
    int n = LENGTH(s);
    if (n >= 2 && (decreasing || isUnsorted(s, FALSE)))
	switch (TYPEOF(s)) {
	case LGLSXP:
	case INTSXP:
	    R_isort2(INTEGER(s), n, decreasing);
	    break;
	case REALSXP:
	    R_rsort2(REAL(s), n, decreasing);
	    break;
	case CPLXSXP:
	    R_csort2(COMPLEX(s), n, decreasing);
	    break;
	case STRSXP:
	    ssort2(STRING_PTR(s), n, decreasing);
	    break;
	default:
	    UNIMPLEMENTED_TYPE("sortVector", s);
	}
}


			/*--- Part III: Partial Sorting ---*/

/*
   Partial sort so that x[k] is in the correct place, smaller to left,
   larger to right

   NOTA BENE:  k < n  required, and *not* checked here but in do_psort();
	       -----  infinite loop possible otherwise!
 */
#define psort_body						\
    Rboolean nalast=TRUE;					\
    int L, R, i, j;						\
								\
    for (L = lo, R = hi; L < R; ) {				\
	v = x[k];						\
	for(i = L, j = R; i <= j;) {				\
	    while (TYPE_CMP(x[i], v, nalast) < 0) i++;			\
	    while (TYPE_CMP(v, x[j], nalast) < 0) j--;			\
	    if (i <= j) { w = x[i]; x[i++] = x[j]; x[j--] = w; }\
	}							\
	if (j < k) L = i;					\
	if (k < i) R = j;					\
    }


static void iPsort2(int *x, int lo, int hi, int k)
{
    int v, w;
#define TYPE_CMP icmp
    psort_body
#undef TYPE_CMP
}

static void rPsort2(double *x, int lo, int hi, int k)
{
    double v, w;
#define TYPE_CMP rcmp
    psort_body
#undef TYPE_CMP
}

static void cPsort2(Rcomplex *x, int lo, int hi, int k)
{
    Rcomplex v, w;
#define TYPE_CMP ccmp
    psort_body
#undef TYPE_CMP
}


static void sPsort2(SEXP *x, int lo, int hi, int k)
{
    SEXP v, w;
#define TYPE_CMP scmp
    psort_body
#undef TYPE_CMP
}

/* elements of ind are 1-based, lo and hi are 0-based */

static void Psort(SEXP x, int lo, int hi, int k)
{
    /* Rprintf("looking for index %d in (%d, %d)\n", k, lo, hi);*/
    switch (TYPEOF(x)) {
    case LGLSXP:
    case INTSXP:
	iPsort2(INTEGER(x), lo, hi, k);
	break;
    case REALSXP:
	rPsort2(REAL(x), lo, hi, k);
	break;
    case CPLXSXP:
	cPsort2(COMPLEX(x), lo, hi, k);
	break;
    case STRSXP:
	sPsort2(STRING_PTR(x), lo, hi, k);
	break;
    default:
	UNIMPLEMENTED_TYPE("Psort", x);
    }
}

static void Psort0(SEXP x, int lo, int hi, int *ind, int k)
{
    if(k < 1 || hi-lo < 1) return;
    if(k <= 1)
	Psort(x, lo, hi, ind[0]-1);
    else {
    /* Look for index nearest the centre of the range */
	int i, This = 0, mid = (lo+hi)/2, z;
	for(i = 0; i < k; i++)
	    if(ind[i]-1 <= mid) This = i;
	z = ind[This]-1;
	Psort(x, lo, hi, z);
	Psort0(x, lo, z-1, ind, This);
	Psort0(x, z+1, hi, ind + This + 1, k - This -1);
    }
}

/* Needed for mistaken decision to put these in the API */
void iPsort(int *x, int n, int k)
{
    iPsort2(x, 0, n-1, k);
}

void rPsort(double *x, int n, int k)
{
    rPsort2(x, 0, n-1, k);
}

void cPsort(Rcomplex *x, int n, int k)
{
    cPsort2(x, 0, n-1, k);
}



/* FUNCTION psort(x, indices) */
static SEXP do_psort(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int i, k, n;
    int *l;
    checkArity(op, args);

    if (!isVectorAtomic(CAR(args)))
	error(_("only atomic vectors can be sorted"));
    if(TYPEOF(CAR(args)) == RAWSXP)
	error(_("raw vectors cannot be sorted"));
    n = LENGTH(CAR(args));
    SETCADR(args, coerceVector(CADR(args), INTSXP));
    l = INTEGER(CADR(args));
    k = LENGTH(CADR(args));
    for (i = 0; i < k; i++) {
	if (l[i] == NA_INTEGER)
	    error(_("NA index"));
	if (l[i] < 1 || l[i] > n)
	    error(_("index %d outside bounds"), l[i]);
    }
    SETCAR(args, duplicate(CAR(args)));
    SET_ATTRIB(CAR(args), R_NilValue);  /* remove all attributes */
    SET_OBJECT(CAR(args), 0);           /* and the object bit    */
    Psort0(CAR(args), 0, n - 1, l, k);
    return CAR(args);
}


			/*--- Part IV : Rank & Order ---*/

static int equal(int i, int j, SEXP x, Rboolean nalast, SEXP rho)
{
    int c=-1;

    if (isObject(x) && !isNull(rho)) { /* so never any NAs */
	/* evaluate .gt(x, i, j) */
	SEXP si, sj, call;
	PROTECT(si = ScalarInteger(i+1));
	PROTECT(sj = ScalarInteger(j+1));
	PROTECT(call = lang4(install(".gt"), x, si, sj));
	c = asInteger(eval(call, rho));
	UNPROTECT(3);
    } else {
	switch (TYPEOF(x)) {
	case LGLSXP:
	case INTSXP:
	    c = icmp(INTEGER(x)[i], INTEGER(x)[j], nalast);
	    break;
	case REALSXP:
	    c = rcmp(REAL(x)[i], REAL(x)[j], nalast);
	    break;
	case CPLXSXP:
	    c = ccmp(COMPLEX(x)[i], COMPLEX(x)[j], nalast);
	    break;
	case STRSXP:
	    c = scmp(STRING_ELT(x, i), STRING_ELT(x, j), nalast);
	    break;
	default:
	    UNIMPLEMENTED_TYPE("equal", x);
	    break;
	}
    }
    if (c == 0)
	return 1;
    return 0;
}

static int greater(int i, int j, SEXP x, Rboolean nalast, Rboolean decreasing,
		   SEXP rho)
{
    int c = -1;

    if (isObject(x) && !isNull(rho)) { /* so never any NAs */
	/* evaluate .gt(x, i, j) */
	SEXP si, sj, call;
	PROTECT(si = ScalarInteger(i+1));
	PROTECT(sj = ScalarInteger(j+1));
	PROTECT(call = lang4(install(".gt"), x, si, sj));
	c = asInteger(eval(call, rho));
	UNPROTECT(3);
    } else {
	switch (TYPEOF(x)) {
	case LGLSXP:
	case INTSXP:
	    c = icmp(INTEGER(x)[i], INTEGER(x)[j], nalast);
	    break;
	case REALSXP:
	    c = rcmp(REAL(x)[i], REAL(x)[j], nalast);
	    break;
	case CPLXSXP:
	    c = ccmp(COMPLEX(x)[i], COMPLEX(x)[j], nalast);
	    break;
	case STRSXP:
	    c = scmp(STRING_ELT(x, i), STRING_ELT(x, j), nalast);
	    break;
	default:
	    UNIMPLEMENTED_TYPE("greater", x);
	    break;
	}
    }
    if (decreasing) c = -c;
    if (c > 0 || (c == 0 && j < i)) return 1; else return 0;
}

/* Merge sort with multiple keys.  Returns indexes (from 1) for sorted order. */

#undef  merge_sort
#define merge_sort merge_sort_key
#undef  merge_value
#define merge_value int
#undef  merge_greater
#define merge_greater merge_greater_key

static SEXP merge_key;     /* Parameters of comparison, for merge_greater_key */
static int  merge_nalast;
static int  merge_decreasing;

static int merge_greater_key (int i, int j)
{
    SEXP key = merge_key;
    int nalast = merge_nalast;
    int decreasing = merge_decreasing;

    int c = -1;

    while (key != R_NilValue) {
        SEXP x = CAR(key);
        switch (TYPEOF(x)) {
        case LGLSXP:
        case INTSXP:
            c = icmp(INTEGER(x)[i-1], INTEGER(x)[j-1], nalast);
            break;
        case REALSXP:
            c = rcmp(REAL(x)[i-1], REAL(x)[j-1], nalast);
            break;
        case CPLXSXP:
            c = ccmp(COMPLEX(x)[i-1], COMPLEX(x)[j-1], nalast);
            break;
        case STRSXP:
            c = scmp(STRING_ELT(x, i-1), STRING_ELT(x, j-1), nalast);
            break;
        default:
            UNIMPLEMENTED_TYPE("merge_greater", x);
        }
        if (decreasing) c = -c;
        if (c > 0)
            return 1;
        if (c < 0)
            return 0;
        key = CDR(key);
    }
    if (c == 0 && i < j) return 0; else return 1;
}

#include "merge-sort.c"

static void orderMerge (int *indx, int n, SEXP key,
                        Rboolean nalast, Rboolean decreasing)
{
    void *vmax = VMAXGET();

    int *ti;
    ti = (int *) R_alloc (n, sizeof *ti);
    for (int i = 0; i < n; i++) ti[i] = i+1;

    merge_key = key;
    merge_nalast = nalast^decreasing;
    merge_decreasing = decreasing;
    merge_sort_key (indx, ti, n);

    VMAXSET(vmax);
}


/* Shell sort with multiple keys.  Returns indexes (from 1) for sorted order. */

static int listgreater(int i, int j, SEXP key, Rboolean nalast,
		       Rboolean decreasing)
{
    SEXP x;
    int c = -1;

    while (key != R_NilValue) {
	x = CAR(key);
	switch (TYPEOF(x)) {
	case LGLSXP:
	case INTSXP:
	    c = icmp(INTEGER(x)[i], INTEGER(x)[j], nalast);
	    break;
	case REALSXP:
	    c = rcmp(REAL(x)[i], REAL(x)[j], nalast);
	    break;
	case CPLXSXP:
	    c = ccmp(COMPLEX(x)[i], COMPLEX(x)[j], nalast);
	    break;
	case STRSXP:
	    c = scmp(STRING_ELT(x, i), STRING_ELT(x, j), nalast);
	    break;
	default:
	    UNIMPLEMENTED_TYPE("listgreater", x);
	}
	if (decreasing) c = -c;
	if (c > 0)
	    return 1;
	if (c < 0)
	    return 0;
	key = CDR(key);
    }
    if (c == 0 && i < j) return 0; else return 1;
}

static void orderVector (int *indx, int n, SEXP key, Rboolean nalast,
                         Rboolean decreasing)
{
    int t;
    for (int i = 0; i < n; i++) indx[i] = i+1;
    for (t = 0; incs[t] > n; t++) ;
    for (int h = incs[t]; h != 0; h = incs[++t])
        for (int i = h; i < n; i++) {
            int itmp = indx[i];
            int j = i;
            while (j >= h && listgreater (indx[j-h] - 1, itmp - 1, key, 
                                          nalast^decreasing, decreasing)) {
                indx[j] = indx[j-h];
                j -= h;
            }
            indx[j] = itmp;
        }
}

#define sort2_with_index \
    for (int h = incs[t]; h != 0; h = incs[++t]) \
        for (int i = lo + h; i <= hi; i++) { \
            int itmp = indx[i]; \
            int j = i; \
            while (j >= lo + h && less(indx[j-h]-1, itmp-1)) { \
                indx[j] = indx[j-h]; \
                j -= h; \
            } \
            indx[j] = itmp; \
        }

/* Returns indexes (from 1) for sorted order.
   Also used by do_options, src/gnuwin32/extra.c
   Called with rho!=R_NilValue only from do_rank, when NAs are not involved. */

void attribute_hidden orderVector1 (int *indx, int n, SEXP key, 
                        Rboolean nalast, Rboolean decreasing, SEXP rho)
{
    int c, i, j, k, l, t;
    int *ix = NULL /* -Wall */;
    double *x = NULL /* -Wall */;
    Rcomplex *cx = NULL /* -Wall */;
    SEXP *sx = NULL /* -Wall */;

    switch (TYPEOF(key)) {
    case LGLSXP:
    case INTSXP:
        ix = INTEGER(key);
        break;
    case REALSXP:
        x = REAL(key);
        break;
    case STRSXP:
        sx = STRING_PTR(key);
        break;
    case CPLXSXP:
        cx = COMPLEX(key);
        break;
    }

    int lo, hi;
    int e;

    if (rho != R_NilValue) {  /* NAs not an issue, just initialize to 1..n */
        for (i = 0; i < n; i++) indx[i] = i+1;
        e = n;
        lo = 0;
        hi = n-1;
    }

    else if (nalast) { /* Initialize sequentially but with all NAs at end */
        e = 0;
        switch (TYPEOF(key)) {
        case LGLSXP:
        case INTSXP:
            for (i = 0; i < n; i++) 
                if (ix[i] != NA_INTEGER) indx[e++] = i+1;
            break;
        case REALSXP:
            for (i = 0; i < n; i++) 
                if (!ISNAN(x[i])) indx[e++] = i+1;
            break;
        case STRSXP:
            for (i = 0; i < n; i++) 
                if (sx[i] != NA_STRING) indx[e++] = i+1;
            break;
        case CPLXSXP:
            for (i = 0; i < n; i++) 
                if (!ISNAN(cx[i].r) && !ISNAN(cx[i].i)) indx[e++] = i+1;
            break;
        default:
            UNIMPLEMENTED_TYPE("orderVector1", key);
        }
        lo = 0;
        hi = e-1;
    }

    else { /* Initialize sequentially but with all NAs at beginning */
        e = 0;
        switch (TYPEOF(key)) {
        case LGLSXP:
        case INTSXP:
            for (i = 0; i < n; i++) 
                if (ix[i] == NA_INTEGER) indx[e++] = i+1;
            break;
        case REALSXP:
            for (i = 0; i < n; i++) 
                if (ISNAN(x[i])) indx[e++] = i+1;
            break;
        case STRSXP:
            for (i = 0; i < n; i++) 
                if (sx[i] == NA_STRING) indx[e++] = i+1;
            break;
        case CPLXSXP:
            for (i = 0; i < n; i++) 
                if (ISNAN(cx[i].r) || ISNAN(cx[i].i)) indx[e++] = i+1;
            break;
        default:
            UNIMPLEMENTED_TYPE("orderVector1", key);
        }
        lo = e;
        hi = n-1;
    }

    /* Now indx is initialized as needed from 0 to e.  Put the missing indexes
       after this, in order. */

    if (e == 0) {
        for (i = 0; i < n; i++) indx[i] = i+1;
    }
    else {
        j = e;
        l = indx[0];
        for (k = 1; k < l; k++) 
            indx[j++] = k;
        for (i = 1; i < e && j < n; i++) {
            l = indx[i];
            for (k = indx[i-1] + 1; k < l; k++)
                indx[j++] = k;
        }
        for (k = indx[e-1]; k < n; k++) /* avoid k ever being > n (overflow) */
            indx[j++] = k+1;
        if (j != n) abort();
    }
    for (t = 0; incs[t] > hi-lo+1; t++) ;
    
    if (isObject(key) && !isNull(rho)) {

        /* Only reached from do_rank. */

#define less(a, b) greater(a, b, key, nalast^decreasing, decreasing, rho)
            sort2_with_index
#undef less
    }
    else {
    
        /* Shell sort isn't stable, so add test on index */

        switch (TYPEOF(key)) {
	case LGLSXP:
	case INTSXP:
	    if (decreasing) {
#define less(a, b) (ix[a] < ix[b] || (ix[a] == ix[b] && a > b))
		sort2_with_index
#undef less
	    } else {
#define less(a, b) (ix[a] > ix[b] || (ix[a] == ix[b] && a > b))
		sort2_with_index
#undef less
	    }
	    break;
	case REALSXP:
	    if (decreasing) {
#define less(a, b) (x[a] < x[b] || (x[a] == x[b] && a > b))
		sort2_with_index
#undef less
	    } else {
#define less(a, b) (x[a] > x[b] || (x[a] == x[b] && a > b))
		sort2_with_index
#undef less
	    }
	    break;
	case CPLXSXP:
	    if (decreasing) {
#define less(a, b) (ccmp(cx[a], cx[b], 0) < 0 || (cx[a].r == cx[b].r && cx[a].i == cx[b].i && a > b))
		sort2_with_index
#undef less
	    } else {
#define less(a, b) (ccmp(cx[a], cx[b], 0) > 0 || (cx[a].r == cx[b].r && cx[a].i == cx[b].i && a > b))
		sort2_with_index
#undef less
	    }
	    break;
	case STRSXP:
	    if (decreasing)
#define less(a, b) (c=Scollate(sx[a], sx[b]), c < 0 || (c == 0 && a > b))
		sort2_with_index
#undef less
	    else
#define less(a, b) (c=Scollate(sx[a], sx[b]), c > 0 || (c == 0 && a > b))
		sort2_with_index
#undef less
	    break;
    	default:  /* only reached from do_rank */
#define less(a, b) greater(a, b, key, nalast^decreasing, decreasing, rho)
	    sort2_with_index
#undef less
	}
    }
}

/* Internal 'order' function.  New form has "merge" or "shell" as first
   argument.  Accomodate any possible calls assuming old form. */

static SEXP do_order(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP ap, ans;
    int i, n = -1, narg = 0;
    int nalast, decreasing, merge;

    /* Check for "merge" or "shell" as first argument. */
    if (TYPEOF(CAR(args)) == STRSXP && LENGTH(CAR(args)) == 1) {
        SEXP chr = CHAR(STRING_ELT(CAR(args),0));
        if (strcmp(chr,"merge") == 0) 
            merge = TRUE;
        else if (strcmp(chr,"shell") == 0) 
            merge = FALSE;
        else
            error(_("invalid '%s' value"), "method");
        args = CDR(args);
    }
    else 
        merge = FALSE;

    nalast = asLogical(CAR(args));
    if (nalast == NA_LOGICAL)
	error(_("invalid '%s' value"), "na.last");
    args = CDR(args);
    decreasing = asLogical(CAR(args));
    if (decreasing == NA_LOGICAL)
	error(_("'decreasing' must be TRUE or FALSE"));
    args = CDR(args);
    if (args == R_NilValue)
	return R_NilValue;

    if (isVector(CAR(args)))
	n = LENGTH(CAR(args));
    for (ap = args; ap != R_NilValue; ap = CDR(ap), narg++) {
	if (!isVector(CAR(ap)))
	    error(_("argument %d is not a vector"), narg + 1);
	if (LENGTH(CAR(ap)) != n)
	    error(_("argument lengths differ"));
    }
    /* NB: collation functions such as Scollate might allocate */
    PROTECT(ans = allocVector(INTSXP, n));
    if (n != 0) {
	if (narg == 1)
	    orderVector1 (INTEGER(ans), n, CAR(args), nalast, decreasing, 
			  R_NilValue);
	else if (merge)
            orderMerge (INTEGER(ans), n, args, nalast, decreasing);
        else
            orderVector (INTEGER(ans), n, args, nalast, decreasing);
    }
    UNPROTECT(1);
    return ans;
}

/* FUNCTION: rank(x) */
static SEXP do_rank(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP rank, indx, x;
    int *in, *ik = NULL /* -Wall */;
    double *rk = NULL /* -Wall */;
    int i, j, k, n;
    const char *ties_str;
    enum {AVERAGE, MAX, MIN} ties_kind = AVERAGE;

    checkArity(op, args);
    if (args == R_NilValue)
	return R_NilValue;
    x = CAR(args);
    if(TYPEOF(x) == RAWSXP)
	error(_("raw vectors cannot be sorted"));
    n = length(x); // FIXME: mignt need to dispatch to length() method
    ties_str = CHAR(asChar(CADR(args)));
    if(!strcmp(ties_str, "average"))	ties_kind = AVERAGE;
    else if(!strcmp(ties_str, "max"))	ties_kind = MAX;
    else if(!strcmp(ties_str, "min"))	ties_kind = MIN;
    else error(_("invalid ties.method for rank() [should never happen]"));
    PROTECT(indx = allocVector(INTSXP, n));
    if (ties_kind == AVERAGE) {
	PROTECT(rank = allocVector(REALSXP, n));
	rk = REAL(rank);
    } else {
	PROTECT(rank = allocVector(INTSXP, n));
	ik = INTEGER(rank);
    }
    if (n > 0) {
	in = INTEGER(indx);
	orderVector1 (in, n, x, TRUE, FALSE, rho);
	for (i = 0; i < n; i = j+1) {
	    j = i;
	    while (j < n-1 && equal(in[j]-1, in[j+1]-1, x, TRUE, rho))
                j += 1;
	    switch (ties_kind) {
	    case AVERAGE:
		for (k = i; k <= j; k++)
		    rk[in[k]-1] = (i + j + 2) / 2.0; break;
	    case MAX:
		for (k = i; k <= j; k++) ik[in[k]-1] = j+1; break;
	    case MIN:
		for (k = i; k <= j; k++) ik[in[k]-1] = i+1; break;
	    }
	}
    }
    UNPROTECT(2);
    return rank;
}

#include <R_ext/RS.h>

static SEXP do_radixsort(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP x, ans;
    int nalast, decreasing;
    R_len_t i, n;
    int tmp, xmax = NA_INTEGER, xmin = NA_INTEGER, off, napos;

    checkArity(op, args);

    x = CAR(args);
    nalast = asLogical(CADR(args));
    if(nalast == NA_LOGICAL)
	error(_("invalid '%s' value"), "na.last");
    decreasing = asLogical(CADDR(args));
    if(decreasing == NA_LOGICAL)
	error(_("'decreasing' must be TRUE or FALSE"));
    off = nalast^decreasing ? 0 : 1;
    n = LENGTH(x);
    PROTECT(ans = allocVector(INTSXP, n));
    for(i = 0; i < n; i++) {
	tmp = INTEGER(x)[i];
	if(tmp == NA_INTEGER) continue;
	if(tmp < 0) error(_("negative value in 'x'"));
	if(xmax == NA_INTEGER || tmp > xmax) xmax = tmp;
	if(xmin == NA_INTEGER || tmp < xmin) xmin = tmp;
    }
    if(xmin == NA_INTEGER) {  /* all NAs, so nothing to do */
	for(i = 0; i < n; i++) INTEGER(ans)[i] = i+1;
	UNPROTECT(1);
	return ans;
    }

    xmax -= xmin;
    if(xmax > 100000) error(_("too large a range of values in 'x'"));
    napos = off ? 0 : xmax + 1;
    off -= xmin;
    /* automatic allocation is fine here: we know this is small */
    int cnts[xmax+2];

    for(i = 0; i <= xmax+1; i++) cnts[i] = 0;
    for(i = 0; i < n; i++) {
	if(INTEGER(x)[i] == NA_INTEGER) cnts[napos]++;
	else cnts[off+INTEGER(x)[i]]++;
    }

    for(i = 1; i <= xmax+1; i++) cnts[i] += cnts[i-1];
    if(decreasing)
	for(i = 0; i < n; i++){
	    tmp = INTEGER(x)[i];
	    INTEGER(ans)[n-(cnts[(tmp==NA_INTEGER) ? napos : off+tmp]--)] = i+1;
	}
    else
	for(i = n-1; i >= 0; i--) {
	    tmp = INTEGER(x)[i];
	    INTEGER(ans)[--cnts[(tmp==NA_INTEGER) ? napos : off+tmp]] = i+1;
	}

    UNPROTECT(1);
    return ans;
}

static SEXP do_xtfrm(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP fn, prargs, ans;

    checkArity(op, args);
    check1arg_x (args, call);

    if(DispatchOrEval(call, op, "xtfrm", args, rho, &ans, 0, 1)) return ans;
    /* otherwise dispatch the default method */
    PROTECT(fn = findFun(install("xtfrm.default"), rho));
    PROTECT(prargs = promiseArgsWithValues(CDR(call), rho, args));
    ans = applyClosure(call, fn, prargs, rho, NULL);
    UNPROTECT(2);
    return ans;
    
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_sort[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"is.unsorted",	do_isunsorted,	0,	11,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"sort",	do_sort,	1,	11,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"psort",	do_psort,	0,	11,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"order",	do_order,	0,	11,	-1,	{PP_FUNCALL, PREC_FN,	0}},
{"rank",	do_rank,	0,	11,	2,	{PP_FUNCALL, PREC_FN,	0}},
{"radixsortold",do_radixsort,	0,	11,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"xtfrm",	do_xtfrm,	0,	1,	1,	{PP_FUNCALL, PREC_FN,	0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}}
};
