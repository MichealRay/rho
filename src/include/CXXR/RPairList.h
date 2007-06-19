/*
 *  R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996  Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1999-2006   The R Development Core Team.
 *  Andrew Runnalls (C) 2007
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2.1 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

/** @file RPairList.h
 * The future RPairList class.
 */

#ifndef RPAIRLIST_H
#define RPAIRLIST_H

#include "CXXR/RObject.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus

#ifdef USE_RINTERNALS

/* List Access Macros */
/* These also work for ... objects */
#define LISTVAL(x)	((x)->u.listsxp)
#define TAG(e)		((e)->u.listsxp.tagval)
#define CAR(e)		((e)->u.listsxp.carval)
#define CDR(e)		((e)->u.listsxp.cdrval)
#define CAAR(e)		CAR(CAR(e))
#define CDAR(e)		CDR(CAR(e))
#define CADR(e)		CAR(CDR(e))
#define CDDR(e)		CDR(CDR(e))
#define CADDR(e)	CAR(CDR(CDR(e)))
#define CADDDR(e)	CAR(CDR(CDR(CDR(e))))
#define CAD4R(e)	CAR(CDR(CDR(CDR(CDR(e)))))
#define MISSING_MASK	15 /* reserve 4 bits--only 2 uses now */
#define MISSING(x)	((x)->sxpinfo.gp & MISSING_MASK)/* for closure calls */
#define SET_MISSING(x,v) do { \
  SEXP __x__ = (x); \
  int __v__ = (v); \
  int __other_flags__ = __x__->sxpinfo.gp & ~MISSING_MASK; \
  __x__->sxpinfo.gp = __other_flags__ | __v__; \
} while (0)

#endif // USE_RINTERNALS

#endif /* __cplusplus */

/* Accessor functions.  Many are declared using () to avoid the macro
   definitions in the USE_RINTERNALS section.
   The function STRING_ELT is used as an argument to arrayAssign even 
   if the macro version is in use.
*/

/* List Access Functions */
/* These also work for ... objects */
#define CONS(a, b)	cons((a), (b))		/* data lists */
#define LCONS(a, b)	lcons((a), (b))		/* language lists */

/**
 * @param e Pointer to a list.
 * @return Pointer to the tag (key) of the list head.
 */
SEXP (TAG)(SEXP e);

/**
 * @param e Pointer to a list.
 * @return Pointer to the value of the list head.
 */
SEXP (CAR)(SEXP e);

/**
 * @param e Pointer to a list.
 * @return Pointer to the tail of the list.
 */
SEXP (CDR)(SEXP e);

/**
 * Equivalent to CAR(CAR(e)).
 */
SEXP (CAAR)(SEXP e);

/**
 * Equivalent to CDR(CAR(e)).
 */
SEXP (CDAR)(SEXP e);

/**
 * Equivalent to CAR(CDR(e)).
 */
SEXP (CADR)(SEXP e);

/**
 * Equivalent to CDR(CDR(e)).
 */
SEXP (CDDR)(SEXP e);

/**
 * Equivalent to CAR(CDR(CDR(e))).
 */
SEXP (CADDR)(SEXP e);

/**
 * Equivalent to CAR(CDR(CDR(CDR(e)))).
 */
SEXP (CADDDR)(SEXP e);

/**
 * Equivalent to CAR(CDR(CDR(CDR(CDR(e))))).
 */
SEXP (CAD4R)(SEXP e);

int  (MISSING)(SEXP x);

void (SET_MISSING)(SEXP x, int v);

/**
 * Set the tag of a list element.
 * @param x Pointer to a list.
 * @param y Pointer an \c RObject representing the new tag of the list head..
 */
void SET_TAG(SEXP x, SEXP y);

/**
 * Set the value of the first element of list.
 * @param x Pointer to a list.
 * @param y Pointer an \c RObject representing the new value of the
 *          list head.
 */
SEXP SETCAR(SEXP x, SEXP y);

/**
 * Replace the tail of a list.
 * @param x Pointer to a list.
 * @param y Pointer an \c RObject representing the new tail of the list.
 */
SEXP SETCDR(SEXP x, SEXP y);

/**
 * Set the value of the second element of list.
 * @param x Pointer to a list.
 * @param y Pointer an \c RObject representing the new value of the
 *          second element of the list.
 */
SEXP SETCADR(SEXP x, SEXP y);

/**
 * Set the value of the third element of list.
 * @param x Pointer to a list.
 * @param y Pointer an \c RObject representing the new value of the
 *          third element of the list.
 */
SEXP SETCADDR(SEXP x, SEXP y);

/**
 * Set the value of the fourth element of list.
 * @param x Pointer to a list.
 * @param y Pointer an \c RObject representing the new value of the
 *          fourth element of the list.
 */
SEXP SETCADDDR(SEXP x, SEXP y);

/**
 * Set the value of the fifth element of list.
 * @param x Pointer to a list.
 * @param y Pointer an \c RObject representing the new value of the
 *          fifth element of the list.
 */
SEXP SETCAD4R(SEXP e, SEXP y);

/* External pointer access macros */
#define EXTPTR_PTR(x)	CAR(x)
#define EXTPTR_PROT(x)	CDR(x)
#define EXTPTR_TAG(x)	TAG(x)

#ifdef BYTECODE
/* Bytecode access macros */
#define BCODE_CODE(x)	CAR(x)
#define BCODE_CONSTS(x) CDR(x)
#define BCODE_EXPR(x)	TAG(x)
#define isByteCode(x)	(TYPEOF(x)==BCODESXP)
#else
#define isByteCode(x)	FALSE
#endif

#ifdef __cplusplus
}
#endif

#endif /* RPAIRLIST_H */
