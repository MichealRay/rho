/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1998-2014   The R Core Team
 *  Copyright (C) 2004        The R Foundation
 *  Copyright (C) 2008-2014  Andrew R. Runnalls.
 *  Copyright (C) 2014 and onwards the CXXR Project Authors.
 *
 *  CXXR is not part of the R project, and bugs and other issues should
 *  not be reported via r-bugs or other R project channels; instead refer
 *  to the CXXR website.
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

#include <Defn.h> /* => Utils.h with the protos from here; Rinternals.h */
#include <Internal.h>
#include <Rmath.h>
#include <R_ext/RS.h>  /* for Calloc/Free */

#include "CXXR/RAllocStack.h"

// 'using namespace std' causes ambiguity of 'greater'
using namespace CXXR;

			/*--- Part I: Comparison Utilities ---*/

static int icmp(int x, int y, Rboolean nalast)
{
    if (x == NA_INTEGER && y == NA_INTEGER) return 0;
    if (x == NA_INTEGER)return nalast ? 1 : -1;
    if (y == NA_INTEGER)return nalast ? -1 : 1;
    if (x < y)		return -1;
    if (x > y)		return 1;
    return 0;
}

static int rcmp(double x, double y, Rboolean nalast)
{
    int nax = ISNAN(x), nay = ISNAN(y);
    if (nax && nay)	return 0;
    if (nax)		return nalast ? 1 : -1;
    if (nay)		return nalast ? -1 : 1;
    if (x < y)		return -1;
    if (x > y)		return 1;
    return 0;
}

static int ccmp(Rcomplex x, Rcomplex y, Rboolean nalast)
{
    int nax = ISNAN(x.r), nay = ISNAN(y.r);
				/* compare real parts */
    if (nax && nay)	return 0;
    if (nax)		return nalast ? 1 : -1;
    if (nay)		return nalast ? -1 : 1;
    if (x.r < y.r)	return -1;
    if (x.r > y.r)	return 1;
				/* compare complex parts */
    nax = ISNAN(x.i); nay = ISNAN(y.i);
    if (nax && nay)	return 0;
    if (nax)		return nalast ? 1 : -1;
    if (nay)		return nalast ? -1 : 1;
    if (x.i < y.i)	return -1;
    if (x.i > y.i)	return 1;

    return 0;		/* equal */
}

static int scmp(SEXP x, SEXP y, Rboolean nalast)
{
    if (x == NA_STRING && y == NA_STRING) return 0;
    if (x == NA_STRING) return nalast ? 1 : -1;
    if (y == NA_STRING) return nalast ? -1 : 1;
    if (x == y) return 0;  /* same string in cache */
    return Scollate(x, y);
}

bool CXXR::String::Comparator::operator()(const String* l,
					  const String* r) const
{
    return scmp(const_cast<String*>(l), const_cast<String*>(r),
		Rboolean(m_na_last)) < 0;
}

Rboolean isUnsorted(SEXP x, Rboolean strictly)
{
    R_xlen_t n, i;

    if (!isVectorAtomic(x))
	error(_("only atomic vectors can be tested to be sorted"));
    n = XLENGTH(x);
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
	case RAWSXP: // being compatible with raw_relop() in ./relop.c
	    if(strictly) {
		for(i = 0; i+1 < n ; i++)
		    if(RAW(x)[i] >= RAW(x)[i+1])
			return TRUE;

	    } else {
		for(i = 0; i+1 < n ; i++)
		    if(RAW(x)[i] > RAW(x)[i+1])
			return TRUE;
	    }
	    break;
	default:
	    UNIMPLEMENTED_TYPE("isUnsorted", x);
	}
    return FALSE;/* sorted */
} // isUnsorted()

SEXP attribute_hidden do_isunsorted(/*const*/ CXXR::Expression* call, const CXXR::BuiltInFunction* op, CXXR::Environment* rho, CXXR::RObject* const* args, int num_args, const CXXR::PairList* tags)
{
    op->checkNumArgs(num_args, call);

    auto dispatched = op->InternalDispatch(call, "is.unsorted",
					   num_args, args, tags, rho);
    if (dispatched.first)
	return dispatched.second;

    RObject* x = PROTECT(args[0]);
    int strictly = asLogical(args[1]);
    if(strictly == NA_LOGICAL)
	errorcall(call, _("invalid '%s' argument"), "strictly");
    if(isVectorAtomic(x)) {
	UNPROTECT(1);
	return (xlength(x) < 2) ? ScalarLogical(FALSE) :
	    ScalarLogical(isUnsorted(x, Rboolean(strictly)));
    }
    if(isObject(x)) {
	// try dispatch -- fails entirely for S4: need "DispatchOrEval()" ?
	SEXP call;
	PROTECT(call = 	// R>  .gtn(x, strictly) :
		lang3(install(".gtn"), x, args[1]));
	SEXP ans = eval(call, rho);
	UNPROTECT(2);
	return ans;
    } // else
	UNPROTECT(1);
    return ScalarLogical(NA_LOGICAL);
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
void attribute_hidden ssort(StringVector* sv, int n)
{
    StringVector& x(*sv);
    String* v;
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


SEXP attribute_hidden do_sort(/*const*/ CXXR::Expression* call, const CXXR::BuiltInFunction* op, CXXR::Environment* rho, CXXR::RObject* const* args, int num_args, const CXXR::PairList* tags)
{
    SEXP ans;
    Rboolean decreasing;

    op->checkNumArgs(num_args, call);

    decreasing = CXXRCONSTRUCT(Rboolean, asLogical(args[1]));
    if(decreasing == NA_LOGICAL)
	error(_("'decreasing' must be TRUE or FALSE"));
    if(args[0] == R_NilValue) return R_NilValue;
    if(!isVectorAtomic(args[0]))
	error(_("only atomic vectors can be sorted"));
    if(TYPEOF(args[0]) == RAWSXP)
	error(_("raw vectors cannot be sorted"));
    /* we need consistent behaviour here, including dropping attibutes,
       so as from 2.3.0 we always duplicate. */
    PROTECT(ans = duplicate(args[0]));
    ans->clearAttributes();  /* this is never called with names */
    sortVector(ans, decreasing);
    UNPROTECT(1);
    return(ans);
}

/* faster versions of shellsort, following Sedgewick (1986) */

/* c(1, 4^k +3*2^(k-1)+1) */
#ifdef LONG_VECTOR_SUPPORT
// This goes up to 2^38: extend eventually.
#define NI 20
static const R_xlen_t incs[NI + 1] = {
    274878693377L, 68719869953L, 17180065793L, 4295065601L,
    1073790977L, 268460033L, 67121153L, 16783361L, 4197377L, 1050113L,
    262913L, 65921L, 16577L, 4193L, 1073L, 281L, 77L, 23L, 8L, 1L, 0L
};
#else
#define NI 16
static const int incs[NI + 1] = {
    1073790977, 268460033, 67121153, 16783361, 4197377, 1050113,
    262913, 65921, 16577, 4193, 1073, 281, 77, 23, 8, 1, 0
};
#endif

#define sort2_body \
    for (h = incs[t]; t < NI; h = incs[++t]) \
	for (i = h; i < n; i++) { \
	    v = x[i]; \
	    j = i; \
	    while (j >= h && x[j - h] less v) { x[j] = x[j - h]; j -= h; } \
	    x[j] = v; \
	}

/* These are only called with n >= 2 */
static void R_isort2(int *x, R_xlen_t n, Rboolean decreasing)
{
    int v;
    R_xlen_t i, j, h, t;

    if (n < 2) error("'n >= 2' is required");
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

static void R_rsort2(double *x, R_xlen_t n, Rboolean decreasing)
{
    double v;
    R_xlen_t i, j, h, t;

    if (n < 2) error("'n >= 2' is required");
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

static void R_csort2(Rcomplex *x, R_xlen_t n, Rboolean decreasing)
{
    Rcomplex v;
    R_xlen_t i, j, h, t;

    if (n < 2) error("'n >= 2' is required");
    for (t = 0; incs[t] > n; t++);
    for (h = incs[t]; t < NI; h = incs[++t])
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

static void ssort2(StringVector* sv, R_xlen_t n, Rboolean decreasing)
{
    String* v;
    R_xlen_t i, j, h, t;

    if (n < 2) error("'n >= 2' is required");
    for (t = 0; incs[t] > n; t++);
    for (h = incs[t]; t < NI; h = incs[++t])
	for (i = h; i < n; i++) {
	    v = (*sv)[i];
	    j = i;
	    if(decreasing)
		while (j >= h && scmp((*sv)[j - h], v, TRUE) < 0)
		{ (*sv)[j] = (*sv)[j - h]; j -= h; }
	    else
		while (j >= h && scmp((*sv)[j - h], v, TRUE) > 0)
		{ (*sv)[j] = (*sv)[j - h]; j -= h; }
	    (*sv)[j] = v;
	}
}

/* The meat of sort.int() */
void sortVector(SEXP s, Rboolean decreasing)
{
    R_xlen_t n = XLENGTH(s);
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
	    {
		StringVector* sv = static_cast<StringVector*>(s);
		ssort2(sv, n, decreasing);
		break;
	    }
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
    R_xlen_t L, R, i, j;					\
								\
    for (L = lo, R = hi; L < R; ) {				\
	v = x[k];						\
	for(i = L, j = R; i <= j;) {				\
	    while (TYPE_CMP(x[i], v, nalast) < 0) i++;		\
	    while (TYPE_CMP(v, x[j], nalast) < 0) j--;		\
	    if (i <= j) { w = x[i]; x[i++] = x[j]; x[j--] = w; }\
	}							\
	if (j < k) L = i;					\
	if (k < i) R = j;					\
    }


static void iPsort2(int *x, R_xlen_t lo, R_xlen_t hi, R_xlen_t k)
{
    int v, w;
#define TYPE_CMP icmp
    psort_body
#undef TYPE_CMP
}

static void rPsort2(double *x, R_xlen_t lo, R_xlen_t hi, R_xlen_t k)
{
    double v, w;
#define TYPE_CMP rcmp
    psort_body
#undef TYPE_CMP
}

static void cPsort2(Rcomplex *x, R_xlen_t lo, R_xlen_t hi, R_xlen_t k)
{
    Rcomplex v, w;
#define TYPE_CMP ccmp
    psort_body
#undef TYPE_CMP
}


static void sPsort2(StringVector* sv, R_xlen_t lo, R_xlen_t hi, R_xlen_t k)
{
    StringVector& x(*sv);
    String *v, *w;
#define TYPE_CMP scmp
    psort_body
#undef TYPE_CMP
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

/* lo, hi, k are 0-based */
static void Psort(SEXP x, R_xlen_t lo, R_xlen_t hi, R_xlen_t k)
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
	{
	    StringVector* sv = static_cast<StringVector*>(x);
	    sPsort2(sv, lo, hi, k);
	    break;
	}
    default:
	UNIMPLEMENTED_TYPE("Psort", x);
    }
}

/* Here ind are 1-based indices passed from R */
static void
Psort0(SEXP x, R_xlen_t lo, R_xlen_t hi, R_xlen_t *ind, int nind)
{
    if(nind < 1 || hi - lo < 1) return;
    if(nind <= 1)
	Psort(x, lo, hi, ind[0] - 1);
    else {
    /* Look for index nearest the centre of the range */
	int This = 0;
	R_xlen_t mid = (lo+hi)/2, z;
	for(int i = 0; i < nind; i++) if(ind[i] - 1 <= mid) This = i;
	z = ind[This] - 1;
	Psort(x, lo, hi, z);
	Psort0(x, lo, z-1, ind, This);
	Psort0(x, z+1, hi, ind + This + 1, nind - This -1);
    }
}


/* FUNCTION psort(x, indices) */
SEXP attribute_hidden do_psort(/*const*/ CXXR::Expression* call, const CXXR::BuiltInFunction* op, CXXR::Environment* rho, CXXR::RObject* const* args, int num_args, const CXXR::PairList* tags)
{
    op->checkNumArgs(num_args, call);
    SEXP x = args[0], p = args[1];

    if (!isVectorAtomic(x))
	error(_("only atomic vectors can be sorted"));
    if(TYPEOF(x) == RAWSXP)
	error(_("raw vectors cannot be sorted"));
    R_xlen_t n = XLENGTH(x);
#ifdef LONG_VECTOR_SUPPORT
    if(!IS_LONG_VEC(x) || TYPEOF(p) != REALSXP)
	p = coerceVector(p, INTSXP);
    int nind = LENGTH(p);
    R_xlen_t *l = static_cast<R_xlen_t *>( CXXR_alloc(nind, sizeof(R_xlen_t)));
    if (TYPEOF(p) == REALSXP) {
	double *rl = REAL(p);
	for (int i = 0; i < nind; i++) {
	    if (!R_FINITE(rl[i])) error(_("NA or infinite index"));
	    l[i] = R_xlen_t( rl[i]);
	    if (l[i] < 1 || l[i] > n)
		error(_("index %ld outside bounds"), l[i]);
	}
    } else {
	int *il = INTEGER(p);
	for (int i = 0; i < nind; i++) {
	    if (il[i] == NA_INTEGER) error(_("NA index"));
	    if (il[i] < 1 || il[i] > n)
		error(_("index %d outside bounds"), il[i]);
	    l[i] = il[i];
	}
    }
#else
    p = coerceVector(p, INTSXP);
    int nind = LENGTH(p);
    int *l = INTEGER(p);
    for (int i = 0; i < nind; i++) {
	if (l[i] == NA_INTEGER)
	    error(_("NA index"));
	if (l[i] < 1 || l[i] > n)
	    error(_("index %d outside bounds"), l[i]);
    }
#endif
    x = duplicate(x);
    x->clearAttributes();  /* remove all attributes */
                           /* and the object bit    */
    Psort0(x, 0, n - 1, l, nind);
    return x;
}


			/*--- Part IV : Rank & Order ---*/

static int equal(R_xlen_t i, R_xlen_t j, SEXP x, Rboolean nalast, SEXP rho)
{
    int c = -1;

    if (isObject(x) && !isNull(rho)) { /* so never any NAs */
	/* evaluate .gt(x, i, j) */
	GCStackRoot<> si, sj, call;
	si = ScalarInteger(int(i)+1);
	sj = ScalarInteger(int(j)+1);
	call = lang4(install(".gt"), x, si, sj);
	c = asInteger(eval(call, rho));
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

static int greater(R_xlen_t i, R_xlen_t j, SEXP x, Rboolean nalast,
		   Rboolean decreasing, SEXP rho)
{
    int c = -1;

    if (isObject(x) && !isNull(rho)) { /* so never any NAs */
	/* evaluate .gt(x, i, j) */
	GCStackRoot<> si, sj, call;
	si = ScalarInteger(int(i)+1);
	sj = ScalarInteger(int(j)+1);
	call = lang4(install(".gt"), x, si, sj);
	c = asInteger(eval(call, rho));
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

/* listgreater(): used as greater_sub in orderVector() in do_order(...) */
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

#define GREATER_2_SUB_DEF(FNAME, TYPE_1, TYPE_2, CMP_FN_1, CMP_FN_2)	\
static int FNAME(int i, int j,						\
		 TYPE_1 *x, TYPE_2 *y,					\
		 Rboolean nalast, Rboolean decreasing)			\
{									\
    int CMP_FN_1(TYPE_1, TYPE_1, Rboolean);				\
    int CMP_FN_2(TYPE_2, TYPE_2, Rboolean);				\
									\
    int c = CMP_FN_1(x[i], x[j], nalast);				\
    if(c) {								\
	if (decreasing) c = -c;						\
	if (c > 0) return 1;						\
	/* else: (c < 0) */ return 0;					\
    }									\
    else {/* have a tie in x -- use  y[]: */				\
	c = CMP_FN_2(y[i], y[j], nalast);				\
	if(c) {								\
	    if (decreasing) c = -c;					\
	    if (c > 0) return 1;					\
	    /* else: (c < 0) */ return 0;				\
	}								\
	else { /* tie in both x[] and y[] : */				\
	    if (i < j) return 0;					\
	    /* else */ return 1;					\
	}								\
    }									\
}

static const int sincs[17] = {
    1073790977, 268460033, 67121153, 16783361, 4197377, 1050113,
    262913, 65921, 16577, 4193, 1073, 281, 77, 23, 8, 1, 0
};

// Needs indx set to  0:(n-1)  initially :
static void
orderVector(int *indx, int n, SEXP key, Rboolean nalast,
	    Rboolean decreasing,
	    int greater_sub(int, int, SEXP, Rboolean, Rboolean))
{
    int i, j, h, t;
    int itmp;

    if (n < 2) return;
    for (t = 0; sincs[t] > n; t++);
    for (h = sincs[t]; t < 16; h = sincs[++t]) {
	R_CheckUserInterrupt();
	for (i = h; i < n; i++) {
	    itmp = indx[i];
	    j = i;
	    while (j >= h &&
		   greater_sub(indx[j - h], itmp, key, CXXRCONSTRUCT(Rboolean, nalast^decreasing),
			       decreasing)) {
		indx[j] = indx[j - h];
		j -= h;
	    }
	    indx[j] = itmp;
	}
    }
}

#ifdef LONG_VECTOR_SUPPORT
static int listgreaterl(R_xlen_t i, R_xlen_t j, SEXP key, Rboolean nalast,
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

static void
orderVectorl(R_xlen_t *indx, R_xlen_t n, SEXP key, Rboolean nalast,
	     Rboolean decreasing,
	     int greater_sub(R_xlen_t, R_xlen_t, SEXP, Rboolean, Rboolean))
{
    int t;
    R_xlen_t i, j, h;
    R_xlen_t itmp;

    if (n < 2) return;
    for (t = 0; incs[t] > n; t++);
    for (h = incs[t]; t < NI; h = incs[++t])
	R_CheckUserInterrupt();
	for (i = h; i < n; i++) {
	    itmp = indx[i];
	    j = i;
	    while (j >= h &&
		   greater_sub(indx[j - h], itmp, key, CXXRCONSTRUCT(Rboolean, nalast^decreasing),
			       decreasing)) {
		indx[j] = indx[j - h];
		j -= h;
	    }
	    indx[j] = itmp;
	}
}
#endif

#ifdef UNUSED
#define ORD_2_BODY(FNAME, TYPE_1, TYPE_2, GREATER_2_SUB)		\
    void FNAME(int *indx, int n, TYPE_1 *x, TYPE_2 *y,			\
	   Rboolean nalast, Rboolean decreasing)			\
{									\
    int t;								\
    for(t = 0; t < n; t++) indx[t] = t; /* indx[] <- 0:(n-1) */		\
    if (n < 2) return;							\
    for(t = 0; sincs[t] > n; t++);					\
    for (int h = sincs[t]; t < 16; h = sincs[++t])			\
	for (int i = h; i < n; i++) {					\
	    int itmp = indx[i], j = i;					\
	    while (j >= h &&						\
		   GREATER_2_SUB(indx[j - h], itmp, x, y,		\
				 nalast^decreasing, decreasing)) {	\
		indx[j] = indx[j - h];					\
		j -= h;							\
	    }								\
	    indx[j] = itmp;						\
	}								\
}

ORD_2_BODY(R_order2double , double, double, double2greater)
ORD_2_BODY(R_order2int    ,    int,    int,    int2greater)
ORD_2_BODY(R_order2dbl_int, double,    int, dblint2greater)
ORD_2_BODY(R_order2int_dbl,    int, double, intdbl2greater)


GREATER_2_SUB_DEF(double2greater, double, double, rcmp, rcmp)
GREATER_2_SUB_DEF(int2greater,       int,    int, icmp, icmp)
GREATER_2_SUB_DEF(dblint2greater, double,    int, rcmp, icmp)
GREATER_2_SUB_DEF(intdbl2greater,    int, double, icmp, rcmp)
#endif

#define sort2_with_index \
    for (h = sincs[t]; t < 16; h = sincs[++t]) { \
	R_CheckUserInterrupt();	 \
	for (i = lo + h; i <= hi; i++) {	 \
	    itmp = indx[i];			 \
	    j = i;						     \
	    while (j >= lo + h && less(indx[j - h], itmp)) {	     \
		indx[j] = indx[j - h]; j -= h; }		     \
	    indx[j] = itmp;					     \
	}							     \
    }


// Usage:  R_orderVector(indx, n,  Rf_lang2(x,y),  nalast, decreasing)
void R_orderVector(int *indx, // must be pre-allocated to length >= n
		   int n,
		   SEXP arglist, // <- e.g.  Rf_lang2(x,y)
		   Rboolean nalast, Rboolean decreasing)
{
    // idx[] <- 0:(n-1) :
    for(int i = 0; i < n; i++) indx[i] = i;
    orderVector(indx, n, arglist, nalast, decreasing, listgreater);
    return;
}



/* Needs indx set to 0:(n-1) initially.
   Also used by do_options and ../gnuwin32/extra.c
   Called with rho != R_NilValue only from do_rank, when NAs are not involved.
 */
void attribute_hidden
orderVector1(int *indx, int n, SEXP key, Rboolean nalast, Rboolean decreasing,
	     SEXP rho)
{
    int c, i, j, h, t, lo = 0, hi = n-1;
    int itmp, *isna = nullptr, numna = 0;
    int *ix = nullptr /* -Wall */;
    double *x = nullptr /* -Wall */;
    Rcomplex *cx = nullptr /* -Wall */;
    StringVector* sv = nullptr /* -Wall */;

    if (n < 2) return;
    switch (TYPEOF(key)) {
    case LGLSXP:
    case INTSXP:
	ix = INTEGER(key);
	break;
    case REALSXP:
	x = REAL(key);
	break;
    case STRSXP:
	sv = static_cast<StringVector*>(key);
	break;
    case CPLXSXP:
	cx = COMPLEX(key);
	break;
    default:  // -Wswitch
	break;
    }

    if(isNull(rho)) {
	/* First sort NAs to one end */
	isna = Calloc(n, int);
	switch (TYPEOF(key)) {
	case LGLSXP:
	case INTSXP:
	    for (i = 0; i < n; i++) isna[i] = (ix[i] == NA_INTEGER);
	    break;
	case REALSXP:
	    for (i = 0; i < n; i++) isna[i] = ISNAN(x[i]);
	    break;
	case STRSXP:
	    for (i = 0; i < n; i++) isna[i] = ((*sv)[i] == NA_STRING);
	    break;
	case CPLXSXP:
	    for (i = 0; i < n; i++) isna[i] = ISNAN(cx[i].r) || ISNAN(cx[i].i);
	    break;
	default:
	    UNIMPLEMENTED_TYPE("orderVector1", key);
	}
	for (i = 0; i < n; i++) numna += isna[i];

	if(numna)
	    switch (TYPEOF(key)) {
	    case LGLSXP:
	    case INTSXP:
	    case REALSXP:
	    case STRSXP:
	    case CPLXSXP:
		if (!nalast) for (i = 0; i < n; i++) isna[i] = !isna[i];
		for (t = 0; sincs[t] > n; t++);
#define less(a, b) (isna[a] > isna[b] || (isna[a] == isna[b] && a > b))
		sort2_with_index
#undef less
		    if(nalast) hi -= numna; else lo += numna;
	    default:  // -Wswitch
		break;
	    }
    }

    /* Shell sort isn't stable, so add test on index */

    /* FIXME: check hi-lo + 1 > 1 ? */
    for (t = 0; sincs[t] > hi-lo+1; t++);

    if (isObject(key) && !isNull(rho)) {
/* only reached from do_rank */
#define less(a, b) greater(a, b, key, CXXRCONSTRUCT(Rboolean, nalast^decreasing), decreasing, rho)
	    sort2_with_index
#undef less
    } else {
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
#define less(a, b) (ccmp(cx[a], cx[b], CXXRFALSE) < 0 || (cx[a].r == cx[b].r && cx[a].i == cx[b].i && a > b))
		sort2_with_index
#undef less
	    } else {
#define less(a, b) (ccmp(cx[a], cx[b], CXXRFALSE) > 0 || (cx[a].r == cx[b].r && cx[a].i == cx[b].i && a > b))
		sort2_with_index
#undef less
	    }
	    break;
	case STRSXP:
	    if (decreasing)
#define less(a, b) (c = Scollate((*sv)[a], (*sv)[b]), c < 0 || (c == 0 && a > b))
		sort2_with_index
#undef less
	    else
#define less(a, b) (c = Scollate((*sv)[a], (*sv)[b]), c > 0 || (c == 0 && a > b))
		sort2_with_index
#undef less
	    break;
	default:  /* only reached from do_rank */
#define less(a, b) greater(a, b, key, CXXRCONSTRUCT(Rboolean, nalast^decreasing), decreasing, rho)
	    sort2_with_index
#undef less
	}
    }
    if(isna) Free(isna);
}

/* version for long vectors */
#ifdef LONG_VECTOR_SUPPORT
static void
orderVector1l(R_xlen_t *indx, R_xlen_t n, SEXP key, Rboolean nalast,
	      Rboolean decreasing, SEXP rho)
{
    R_xlen_t c, i, j, h, t, lo = 0, hi = n-1;
    int *isna = nullptr, numna = 0;
    int *ix = nullptr /* -Wall */;
    double *x = nullptr /* -Wall */;
    Rcomplex *cx = nullptr /* -Wall */;
    StringVector* sv = nullptr /* -Wall */;
    R_xlen_t itmp;

    if (n < 2) return;
    switch (TYPEOF(key)) {
    case LGLSXP:
    case INTSXP:
	ix = INTEGER(key);
	break;
    case REALSXP:
	x = REAL(key);
	break;
    case STRSXP:
	sv = static_cast<StringVector*>(key);
	break;
    case CPLXSXP:
	cx = COMPLEX(key);
	break;
    default:  // -Wswitch
	break;
    }

    if(isNull(rho)) {
	/* First sort NAs to one end */
	isna = Calloc(n, int);
	switch (TYPEOF(key)) {
	case LGLSXP:
	case INTSXP:
	    for (i = 0; i < n; i++) isna[i] = (ix[i] == NA_INTEGER);
	    break;
	case REALSXP:
	    for (i = 0; i < n; i++) isna[i] = ISNAN(x[i]);
	    break;
	case STRSXP:
	    for (i = 0; i < n; i++) isna[i] = ((*sv)[i] == NA_STRING);
	    break;
	case CPLXSXP:
	    for (i = 0; i < n; i++) isna[i] = ISNAN(cx[i].r) || ISNAN(cx[i].i);
	    break;
	default:
	    UNIMPLEMENTED_TYPE("orderVector1", key);
	}
	for (i = 0; i < n; i++) numna += isna[i];

	if(numna)
	    switch (TYPEOF(key)) {
	    case LGLSXP:
	    case INTSXP:
	    case REALSXP:
	    case STRSXP:
	    case CPLXSXP:
		if (!nalast) for (i = 0; i < n; i++) isna[i] = !isna[i];
		for (t = 0; sincs[t] > n; t++);
#define less(a, b) (isna[a] > isna[b] || (isna[a] == isna[b] && a > b))
		sort2_with_index
#undef less
		    if(nalast) hi -= numna; else lo += numna;
	    default:  // -Wswitch
		break;
	    }
    }

    /* Shell sort isn't stable, so add test on index */

    /* FIXME: check hi-lo + 1 > 1 ? */
    for (t = 0; sincs[t] > hi-lo+1; t++);

    if (isObject(key) && !isNull(rho)) {
/* only reached from do_rank */
#define less(a, b) greater(a, b, key, CXXRCONSTRUCT(Rboolean, nalast^decreasing), decreasing, rho)
	    sort2_with_index
#undef less
    } else {
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
#define less(a, b) (ccmp(cx[a], cx[b], CXXRFALSE) < 0 || (cx[a].r == cx[b].r && cx[a].i == cx[b].i && a > b))
		sort2_with_index
#undef less
	    } else {
#define less(a, b) (ccmp(cx[a], cx[b], CXXRFALSE) > 0 || (cx[a].r == cx[b].r && cx[a].i == cx[b].i && a > b))
		sort2_with_index
#undef less
	    }
	    break;
	case STRSXP:
	    if (decreasing)
#define less(a, b) (c=Scollate((*sv)[a], (*sv)[b]), c < 0 || (c == 0 && a > b))
		sort2_with_index
#undef less
	    else
#define less(a, b) (c=Scollate((*sv)[a], (*sv)[b]), c > 0 || (c == 0 && a > b))
		sort2_with_index
#undef less
	    break;
	default:  /* only reached from do_rank */
#define less(a, b) greater(a, b, key, CXXRCONSTRUCT(Rboolean, nalast^decreasing), decreasing, rho)
	    sort2_with_index
#undef less
	}
    }
    if(isna) Free(isna);
}
#endif

/* FUNCTION order(...) */
SEXP attribute_hidden do_order(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP ap, ans = R_NilValue /* -Wall */;
    int narg = 0;
    R_xlen_t n = -1;
    Rboolean nalast, decreasing;

    nalast = CXXRCONSTRUCT(Rboolean, asLogical(CAR(args)));
    if(nalast == NA_LOGICAL)
	error(_("invalid '%s' value"), "na.last");
    args = CDR(args);
    decreasing = CXXRCONSTRUCT(Rboolean, asLogical(CAR(args)));
    if(decreasing == NA_LOGICAL)
	error(_("'decreasing' must be TRUE or FALSE"));
    args = CDR(args);
    if (args == R_NilValue)
	return R_NilValue;

    if (isVector(CAR(args)))
	n = XLENGTH(CAR(args));
    for (ap = args; ap != R_NilValue; ap = CDR(ap), narg++) {
	if (!isVector(CAR(ap)))
	    error(_("argument %d is not a vector"), narg + 1);
	if (XLENGTH(CAR(ap)) != n)
	    error(_("argument lengths differ"));
    }
    /* NB: collation functions such as Scollate might allocate */
    if (n != 0) {
	if(narg == 1) {
#ifdef LONG_VECTOR_SUPPORT
	    if (n > INT_MAX)  {
		PROTECT(ans = allocVector(REALSXP, n));
		R_xlen_t *in = static_cast<R_xlen_t *>( CXXR_alloc(n, sizeof(R_xlen_t)));
		for (R_xlen_t i = 0; i < n; i++) in[i] = i;
		orderVector1l(in, n, CAR(args), nalast, decreasing,
			      R_NilValue);
		for (R_xlen_t i = 0; i < n; i++) REAL(ans)[i] = in[i] + 1;
	    } else
#endif
	    {
		PROTECT(ans = allocVector(INTSXP, n));
		for (R_xlen_t i = 0; i < n; i++) INTEGER(ans)[i] = int( i);
		orderVector1(INTEGER(ans), int(n), CAR(args), nalast,
			     decreasing, R_NilValue);
		for (R_xlen_t i = 0; i < n; i++) INTEGER(ans)[i]++;
	    }
	} else {
#ifdef LONG_VECTOR_SUPPORT
	    if (n > INT_MAX)  {
		PROTECT(ans = allocVector(REALSXP, n));
		R_xlen_t *in = static_cast<R_xlen_t *>( CXXR_alloc(n, sizeof(R_xlen_t)));
		for (R_xlen_t i = 0; i < n; i++) in[i] = i;
		orderVectorl(in, n, CAR(args), nalast, decreasing,
			     listgreaterl);
		for (R_xlen_t i = 0; i < n; i++) REAL(ans)[i] = in[i] + 1;
	    } else
#endif
	    {
		PROTECT(ans = allocVector(INTSXP, n));
		for (R_xlen_t i = 0; i < n; i++) INTEGER(ans)[i] = int( i);
		orderVector(INTEGER(ans), int( n), args, nalast,
			    decreasing, listgreater);
		for (R_xlen_t i = 0; i < n; i++) INTEGER(ans)[i]++;
	    }
	}
	UNPROTECT(1);
	return ans;
    } else return allocVector(INTSXP, 0);
}

/* FUNCTION: rank(x, length, ties.method) */
SEXP attribute_hidden do_rank(/*const*/ CXXR::Expression* call, const CXXR::BuiltInFunction* op, CXXR::Environment* rho, CXXR::RObject* const* args, int num_args, const CXXR::PairList* tags)
{
    SEXP rank, x;
    int *ik = nullptr /* -Wall */;
    double *rk = nullptr /* -Wall */;
    enum {AVERAGE, MAX, MIN} ties_kind = AVERAGE;
    Rboolean isLong = FALSE;

    op->checkNumArgs(num_args, call);
    x = args[0];
    if(TYPEOF(x) == RAWSXP)
	error(_("raw vectors cannot be sorted"));
#ifdef LONG_VECTOR_SUPPORT
    SEXP sn = args[1];
    R_xlen_t n;
    if (TYPEOF(sn) == REALSXP)  {
	double d = REAL(x)[0];
	if(ISNAN(d)) error(_("vector size cannot be NA/NaN"));
	if(!R_FINITE(d)) error(_("vector size cannot be infinite"));
	if(d > R_XLEN_T_MAX) error(_("vector size specified is too large"));
	n = R_xlen_t( d);
	if (n < 0) error(_("invalid '%s' value"), "length(xx)");
    } else {
	int nn = asInteger(sn);
	if (nn == NA_INTEGER || nn < 0)
	    error(_("invalid '%s' value"), "length(xx)");
	n = nn;
    }
    isLong = CXXRCONSTRUCT(Rboolean, n > INT_MAX);
#else
    int n = asInteger(CADR(args));
    if (n == NA_INTEGER || n < 0)
	error(_("invalid '%s' value"), "length(xx)");
#endif
    const char *ties_str = CHAR(asChar(args[2]));
    if(!strcmp(ties_str, "average"))	ties_kind = AVERAGE;
    else if(!strcmp(ties_str, "max"))	ties_kind = MAX;
    else if(!strcmp(ties_str, "min"))	ties_kind = MIN;
    else error(_("invalid ties.method for rank() [should never happen]"));
    if (ties_kind == AVERAGE || isLong) {
	PROTECT(rank = allocVector(REALSXP, n));
	rk = REAL(rank);
    } else {
	PROTECT(rank = allocVector(INTSXP, n));
	ik = INTEGER(rank);
    }
    if (n > 0) {
#ifdef LONG_VECTOR_SUPPORT
	if(isLong) {
	    R_xlen_t i, j, k;
	    R_xlen_t *in = static_cast<R_xlen_t *>( CXXR_alloc(n, sizeof(R_xlen_t)));
	    for (i = 0; i < n; i++) in[i] = i;
	    orderVector1l(in, n, x, TRUE, FALSE, rho);
	    for (i = 0; i < n; i = j+1) {
		j = i;
		while ((j < n - 1) && equal(in[j], in[j + 1], x, TRUE, rho)) j++;
		switch(ties_kind) {
		case AVERAGE:
		    for (k = i; k <= j; k++)
			rk[in[k]] = (i + j + 2) / 2.; break;
		case MAX:
		    for (k = i; k <= j; k++) rk[in[k]] = j+1; break;
		case MIN:
		    for (k = i; k <= j; k++) rk[in[k]] = i+1; break;
		}
	    }
	} else
#endif
	{
	    int i, j, k;
	    int *in = static_cast<int *>( CXXR_alloc(n, sizeof(int)));
	    for (i = 0; i < n; i++) in[i] = i;
	    orderVector1(in, int( n), x, TRUE, FALSE, rho);
	    for (i = 0; i < n; i = j+1) {
		j = i;
		while ((j < n - 1) && equal(in[j], in[j + 1], x, TRUE, rho)) j++;
		switch(ties_kind) {
		case AVERAGE:
		    for (k = i; k <= j; k++)
			rk[in[k]] = (i + j + 2) / 2.; break;
		case MAX:
		    for (k = i; k <= j; k++) ik[in[k]] = j+1; break;
		case MIN:
		    for (k = i; k <= j; k++) ik[in[k]] = i+1; break;
		}
	    }
	}
    }
    UNPROTECT(1);
    return rank;
}

#include <R_ext/RS.h>

/* also returns integers/doubles (a method for sort.list) */
SEXP attribute_hidden do_radixsort(/*const*/ CXXR::Expression* call, const CXXR::BuiltInFunction* op, CXXR::Environment* rho, CXXR::RObject* const* args, int num_args, const CXXR::PairList* tags)
{
    SEXP x, ans;
    Rboolean nalast, decreasing;
    R_xlen_t i, n;
    int tmp, xmax = NA_INTEGER, xmin = NA_INTEGER, off, napos;

    op->checkNumArgs(num_args, call);

    x = args[0];
    nalast = CXXRCONSTRUCT(Rboolean, asLogical(args[1]));
    if(nalast == NA_LOGICAL)
	error(_("invalid '%s' value"), "na.last");
    decreasing = CXXRCONSTRUCT(Rboolean, asLogical(args[2]));
    if(decreasing == NA_LOGICAL)
	error(_("'decreasing' must be TRUE or FALSE"));
    off = nalast^decreasing ? 0 : 1;
    n = XLENGTH(x);
#ifdef LONG_VECTOR_SUPPORT
    Rboolean isLong = CXXRCONSTRUCT(Rboolean, n > INT_MAX);
    if(isLong)
	ans = allocVector(REALSXP, n);
    else
	ans = allocVector(INTSXP, n);
#else
    ans = allocVector(INTSXP, n);
#endif
    PROTECT(ans); // not currently needed
    for(i = 0; i < n; i++) {
	tmp = INTEGER(x)[i];
	if(tmp == NA_INTEGER) continue;
	if(xmax == NA_INTEGER || tmp > xmax) xmax = tmp;
	if(xmin == NA_INTEGER || tmp < xmin) xmin = tmp;
    }
    if(xmin == NA_INTEGER) {  /* all NAs, so nothing to do */
#ifdef LONG_VECTOR_SUPPORT
	if (isLong) {
	    for(i = 0; i < n; i++) REAL(ans)[i] = double(i+1);
	} else
#endif
	{
	    for(i = 0; i < n; i++) INTEGER(ans)[i] = int(i+1);
	}
	UNPROTECT(1);
	return ans;
    }

    xmax -= xmin;
    if(xmax > 100000) error(_("too large a range of values in 'x'"));
    napos = off ? 0 : xmax + 1;
    off -= xmin;
    std::vector<unsigned int> cntsv(xmax+2);
    unsigned int* cnts = &cntsv[0];

    for(i = 0; i <= xmax+1; i++) cnts[i] = 0;
    for(i = 0; i < n; i++) {
	if(INTEGER(x)[i] == NA_INTEGER) cnts[napos]++;
	else cnts[off+INTEGER(x)[i]]++;
    }

    for(i = 1; i <= xmax+1; i++) cnts[i] += cnts[i-1];
#ifdef LONG_VECTOR_SUPPORT
    if (isLong) {
	if(decreasing)
	    for(i = 0; i < n; i++) {
		tmp = INTEGER(x)[i];
		REAL(ans)[n-(cnts[(tmp == NA_INTEGER) ? napos : off+tmp]--)] =
		    double(i+1);
	    }
	else
	    for(i = n-1; i >= 0; i--) {
		tmp = INTEGER(x)[i];
		REAL(ans)[--cnts[(tmp == NA_INTEGER) ? napos : off+tmp]] =
		    double(i+1);
	    }
    } else
#endif
    {
	if(decreasing)
	    for(i = 0; i < n; i++) {
		tmp = INTEGER(x)[i];
		INTEGER(ans)[n-(cnts[(tmp == NA_INTEGER) ? napos : off+tmp]--)] =
		    int(i+1);
	    }
	else
	    for(i = n-1; i >= 0; i--) {
		tmp = INTEGER(x)[i];
		INTEGER(ans)[--cnts[(tmp == NA_INTEGER) ? napos : off+tmp]] =
		    int(i+1);
	    }
    }

    UNPROTECT(1);
    return ans;
}

SEXP attribute_hidden do_xtfrm(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP fn, prargs, ans;

    checkArity(op, args);
    check1arg(args, call, "x");

    if(DispatchOrEval(call, op, "xtfrm", args, rho, &ans,
		      MissingArgHandling::Keep, 1)) return ans;
    /* otherwise dispatch the default method */
    PROTECT(fn = findFun(install("xtfrm.default"), rho));
    PROTECT(prargs = promiseArgs(args, R_GlobalEnv));
    SET_PRVALUE(CAR(prargs), CAR(args));
    Closure* closure = SEXP_downcast<Closure*>(fn);
    Expression* callx = SEXP_downcast<Expression*>(call);
    ArgList arglist(SEXP_downcast<PairList*>(prargs), ArgList::PROMISED);
    Environment* callenv = SEXP_downcast<Environment*>(rho);
    ans = callx->invokeClosure(closure, callenv, &arglist);
    UNPROTECT(2);
    return ans;

}
