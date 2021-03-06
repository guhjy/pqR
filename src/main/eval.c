/*
 *  pqR : A pretty quick version of R
 *  Copyright (C) 2013, 2014, 2015, 2016, 2017 by Radford M. Neal
 *
 *  Based on R : A Computer Language for Statistical Data Analysis
 *  Copyright (C) 1995, 1996	Robert Gentleman and Ross Ihaka
 *  Copyright (C) 1998--2011	The R Core Team.
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


#undef HASHING

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef HAVE_ALLOCA_H
# include <alloca.h>
#endif

#define USE_FAST_PROTECT_MACROS
#define R_USE_SIGNALS 1
#include <Defn.h>
#include <Rinterface.h>
#include <Fileio.h>

#include "arithmetic.h"

#include <helpers/helpers-app.h>


/* Inline version of findFun, for speed when a special symbol is found in
   the base environmet. */

static inline SEXP FINDFUN (SEXP symbol, SEXP rho)
{
    if (SPEC_SYM(symbol)) {
        while (NO_SPEC_SYM(rho)) /* note that NO_SPEC_SYM(R_EmptyEnv) is 0 */
            rho = ENCLOS(rho);
        if (rho == R_GlobalEnv && BASE_CACHE(symbol)) {
            SEXP res = SYMVALUE(symbol);
            if (TYPEOF(res) == PROMSXP)
                res = PRVALUE_PENDING_OK(res);
            return res;
        }
    }

    return findFun_nospecsym(symbol,rho);
}


#define ARGUSED(x) LEVELS(x)

static SEXP Rf_builtin_op_no_cntxt (SEXP op, SEXP e, SEXP rho, int variant);
static SEXP bcEval(SEXP, SEXP, Rboolean);

/*#define BC_PROFILING*/
#ifdef BC_PROFILING
static Rboolean bc_profiling = FALSE;
#endif

#define R_Profiling R_high_frequency_globals.Profiling

#ifdef R_PROFILING

/* BDR 2000-07-15
   Profiling is now controlled by the R function Rprof(), and should
   have negligible cost when not enabled.
*/

/* A simple mechanism for profiling R code.  When R_PROFILING is
   enabled, eval will write out the call stack every PROFSAMPLE
   microseconds using the SIGPROF handler triggered by timer signals
   from the ITIMER_PROF timer.  Since this is the same timer used by C
   profiling, the two cannot be used together.  Output is written to
   the file PROFOUTNAME.  This is a plain text file.  The first line
   of the file contains the value of PROFSAMPLE.  The remaining lines
   each give the call stack found at a sampling point with the inner
   most function first.

   To enable profiling, recompile eval.c with R_PROFILING defined.  It
   would be possible to selectively turn profiling on and off from R
   and to specify the file name from R as well, but for now I won't
   bother.

   The stack is traced by walking back along the context stack, just
   like the traceback creation in jump_to_toplevel.  One drawback of
   this approach is that it does not show BUILTIN's since they don't
   get a context.  With recent changes to pos.to.env it seems possible
   to insert a context around BUILTIN calls to that they show up in
   the trace.  Since there is a cost in establishing these contexts,
   they are only inserted when profiling is enabled. [BDR: we have since
   also added contexts for the BUILTIN calls to foreign code.]

   One possible advantage of not tracing BUILTIN's is that then
   profiling adds no cost when the timer is turned off.  This would be
   useful if we want to allow profiling to be turned on and off from
   within R.

   One thing that makes interpreting profiling output tricky is lazy
   evaluation.  When an expression f(g(x)) is profiled, lazy
   evaluation will cause g to be called inside the call to f, so it
   will appear as if g is called by f.

   L. T.  */

#ifdef Win32
# define WIN32_LEAN_AND_MEAN 1
# include <windows.h>		/* for CreateEvent, SetEvent */
# include <process.h>		/* for _beginthread, _endthread */
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# endif
# include <signal.h>
#endif /* not Win32 */

static FILE *R_ProfileOutfile = NULL;
static int R_Mem_Profiling=0;
extern void get_current_mem(unsigned long *,unsigned long *,unsigned long *); /* in memory.c */
extern unsigned long get_duplicate_counter(void);  /* in duplicate.c */
extern void reset_duplicate_counter(void);         /* in duplicate.c */

#ifdef Win32
HANDLE MainThread;
HANDLE ProfileEvent;

static void doprof(void)
{
    RCNTXT *cptr;
    char buf[1100];
    unsigned long bigv, smallv, nodes;
    int len;

    buf[0] = '\0';
    SuspendThread(MainThread);
    if (R_Mem_Profiling){
	    get_current_mem(&smallv, &bigv, &nodes);
	    if((len = strlen(buf)) < 1000) {
		sprintf(buf+len, ":%ld:%ld:%ld:%ld:", smallv, bigv,
		     nodes, get_duplicate_counter());
	    }
	    reset_duplicate_counter();
    }
    for (cptr = R_GlobalContext; cptr; cptr = cptr->nextcontext) {
	if ((cptr->callflag & (CTXT_FUNCTION | CTXT_BUILTIN))
	    && TYPEOF(cptr->call) == LANGSXP) {
	    SEXP fun = CAR(cptr->call);
	    if(strlen(buf) < 1000) {
		strcat(buf, TYPEOF(fun) == SYMSXP ? CHAR(PRINTNAME(fun)) :
		       "<Anonymous>");
		strcat(buf, " ");
	    }
	}
    }
    ResumeThread(MainThread);
    if(strlen(buf))
	fprintf(R_ProfileOutfile, "%s\n", buf);
}

/* Profiling thread main function */
static void __cdecl ProfileThread(void *pwait)
{
    int wait = *((int *)pwait);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while(WaitForSingleObject(ProfileEvent, wait) != WAIT_OBJECT_0) {
	doprof();
    }
}
#else /* not Win32 */
static void doprof(int sig)
{
    RCNTXT *cptr;
    int newline = 0;
    unsigned long bigv, smallv, nodes;
    if (R_Mem_Profiling){
	    get_current_mem(&smallv, &bigv, &nodes);
	    if (!newline) newline = 1;
	    fprintf(R_ProfileOutfile, ":%ld:%ld:%ld:%ld:", smallv, bigv,
		     nodes, get_duplicate_counter());
	    reset_duplicate_counter();
    }
    for (cptr = R_GlobalContext; cptr; cptr = cptr->nextcontext) {
	if ((cptr->callflag & (CTXT_FUNCTION | CTXT_BUILTIN))
	    && TYPEOF(cptr->call) == LANGSXP) {
	    SEXP fun = CAR(cptr->call);
	    if (!newline) newline = 1;
	    fprintf(R_ProfileOutfile, "\"%s\" ",
		    TYPEOF(fun) == SYMSXP ? CHAR(PRINTNAME(fun)) :
		    "<Anonymous>");
	}
    }
    if (newline) fprintf(R_ProfileOutfile, "\n");
    signal(SIGPROF, doprof);
}

static void doprof_null(int sig)
{
    signal(SIGPROF, doprof_null);
}
#endif /* not Win32 */


static void R_EndProfiling(void)
{
#ifdef Win32
    SetEvent(ProfileEvent);
    CloseHandle(MainThread);
#else /* not Win32 */
    struct itimerval itv;

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 0;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_PROF, &itv, NULL);
    signal(SIGPROF, doprof_null);
#endif /* not Win32 */
    if(R_ProfileOutfile) fclose(R_ProfileOutfile);
    R_ProfileOutfile = NULL;
    R_Profiling = 0;
}

static void R_InitProfiling(SEXP filename, int append, double dinterval, int mem_profiling)
{
#ifndef Win32
    struct itimerval itv;
#else
    int wait;
    HANDLE Proc = GetCurrentProcess();
#endif
    int interval;

    interval = 1e6 * dinterval + 0.5;
    if(R_ProfileOutfile != NULL) R_EndProfiling();
    R_ProfileOutfile = RC_fopen(filename, append ? "a" : "w", TRUE);
    if (R_ProfileOutfile == NULL)
	error(_("Rprof: cannot open profile file '%s'"),
	      translateChar(filename));
    if(mem_profiling)
	fprintf(R_ProfileOutfile, "memory profiling: sample.interval=%d\n", interval);
    else
	fprintf(R_ProfileOutfile, "sample.interval=%d\n", interval);

    R_Mem_Profiling=mem_profiling;
    if (mem_profiling)
	reset_duplicate_counter();

#ifdef Win32
    /* need to duplicate to make a real handle */
    DuplicateHandle(Proc, GetCurrentThread(), Proc, &MainThread,
		    0, FALSE, DUPLICATE_SAME_ACCESS);
    wait = interval/1000;
    if(!(ProfileEvent = CreateEvent(NULL, FALSE, FALSE, NULL)) ||
       (_beginthread(ProfileThread, 0, &wait) == -1))
	R_Suicide("unable to create profiling thread");
    Sleep(wait/2); /* suspend this thread to ensure that the other one starts */
#else /* not Win32 */
    signal(SIGPROF, doprof);

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = interval;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = interval;
    if (setitimer(ITIMER_PROF, &itv, NULL) == -1)
	R_Suicide("setting profile timer failed");
#endif /* not Win32 */
    R_Profiling = 1;
}

static SEXP do_Rprof(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP filename;
    int append_mode, mem_profiling;
    double dinterval;

#ifdef BC_PROFILING
    if (bc_profiling) {
	warning(_("can't use R profiling while byte code profiling"));
	return R_NilValue;
    }
#endif
    checkArity(op, args);
    if (!isString(CAR(args)) || (LENGTH(CAR(args))) != 1)
	error(_("invalid '%s' argument"), "filename");
    append_mode = asLogical(CADR(args));
    dinterval = asReal(CADDR(args));
    mem_profiling = asLogical(CADDDR(args));
    filename = STRING_ELT(CAR(args), 0);
    if (LENGTH(filename))
	R_InitProfiling(filename, append_mode, dinterval, mem_profiling);
    else
	R_EndProfiling();
    return R_NilValue;
}
#else /* not R_PROFILING */
static SEXP do_Rprof(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    error(_("R profiling is not available on this system"));
}
#endif /* not R_PROFILING */

/* NEEDED: A fixup is needed in browser, because it can trap errors,
 *	and currently does not reset the limit to the right value. */

#define CHECK_STACK_BALANCE(o,s) do { \
  if (s != R_PPStackTop) check_stack_balance(o,s); \
} while (0)

void attribute_hidden check_stack_balance(SEXP op, int save)
{
    if(save == R_PPStackTop) return;
    REprintf("Warning: stack imbalance in '%s', %d then %d\n",
	     PRIMNAME(op), save, R_PPStackTop);
}


/* Wait until no value in an argument list is still being computed by a task.
   Macro version does preliminary check in-line for speed. */

#define WAIT_UNTIL_ARGUMENTS_COMPUTED(_args_) \
    do { \
        if (helpers_tasks > 0) { \
            SEXP _a_ = (_args_); \
            while (_a_ != R_NilValue) { \
                if (helpers_is_being_computed(CAR(_a_))) { \
                    wait_until_arguments_computed (_a_); \
                    break; \
                } \
                _a_ = CDR(_a_); \
            } \
        } \
    } while (0)

void attribute_hidden wait_until_arguments_computed (SEXP args)
{
    SEXP wait_for, a;

    if (helpers_tasks == 0) return;

    wait_for = R_NoObject;

    for (a = args; a != R_NilValue; a = CDR(a)) {
        SEXP this_arg = CAR(a);
        if (helpers_is_being_computed(this_arg)) {
            if (wait_for == R_NoObject)
                wait_for = this_arg;
            else {
                helpers_wait_until_not_being_computed2 (wait_for, this_arg);
                wait_for = R_NoObject;
            }
        }
    }

    if (wait_for != R_NoObject)
        helpers_wait_until_not_being_computed (wait_for);
}

/* e is protected here */
SEXP attribute_hidden forcePromiseUnbound (SEXP e, int variant)
{
    RPRSTACK prstack;
    SEXP val;

    PROTECT(e);

    if (PRSEEN(e)) PRSEEN_error_or_warning(e);

    /* Mark the promise as under evaluation and push it on a stack
       that can be used to unmark pending promises if a jump out
       of the evaluation occurs. */

    prstack.promise = e;
    prstack.next = R_PendingPromises;
    R_PendingPromises = &prstack;

    SET_PRSEEN(e, 1);

    val = EVALV (PRCODE(e), PRENV(e), 
                 (variant & VARIANT_PENDING_OK) | VARIANT_MISSING_OK);

    /* Pop the stack, unmark the promise and set its value field. */

    R_PendingPromises = prstack.next;
    SET_PRSEEN(e, 0);
    SET_PRVALUE(e, val);
    INC_NAMEDCNT(val);

    /* Attempt to mimic past behaviour... */
    if (val == R_MissingArg) {
        if ( ! (variant & VARIANT_MISSING_OK) && TYPEOF(PRCODE(e)) == SYMSXP 
                  && R_isMissing (PRCODE(e), PRENV(e)))
            arg_missing_error(PRCODE(e));
    }
    else {
        /* Set the environment to R_NilValue to allow GC to
           reclaim the promise environment (unless value is R_MissingArg);
           this is also useful for fancy games with delayedAssign() */
        SET_PRENV(e, R_NilValue);
    }

    UNPROTECT(1);

    return val;
}

SEXP forcePromise (SEXP e) /* e protected here if necessary */
{
    if (PRVALUE(e) == R_UnboundValue) {
        return forcePromiseUnbound(e,0);
    }
    else
        return PRVALUE(e);
}


/* The "evalv" function returns the value of "e" evaluated in "rho",
   with given variant.  The caller must ensure that both SEXP
   arguments are protected.  The "eval" function is just like "evalv"
   with 0 for the variant return argument.

   The "Rf_evalv2" function, if it exists, is the main part of
   "evalv", split off so that constants may be evaluated with less
   overhead within "eval" or "evalv".  It may also be used in the
   EVALV macro in Defn.h. 

   Some optional tweaks can be done here, controlled by R_EVAL_TWEAKS,
   set to decimal integer XYZ.  If XYZ is zero, no tweaks are done.
   Otherwise, the meanings are

       Z = 1      Enable and use Rf_evalv2 (also done if X or Y is non-zero)
       Y = 1      Have eval do its own prelude, rather than just calling evalv
       X = 0      Have EVALV in Defn.h just call evalv here
           1      Have EVALV do its own prelude, then call evalv2
           2      Have EVALV do its own prelude and easy symbol stuff, then
                  call evalv2
 */

SEXP Rf_evalv2(SEXP,SEXP,int);
SEXP Rf_builtin_op (SEXP op, SEXP e, SEXP rho, int variant);

#define evalcount R_high_frequency_globals.evalcount

#define EVAL_PRELUDE do { \
\
    R_variant_result = 0; \
\
    /* Evaluate constants quickly, without the overhead that's necessary when \
       the computation might be complex.  This code is repeated in evalv2 \
       for when evalcount < 0.  That way we avoid calling any procedure \
       other than evalv2 in this procedure, possibly reducing overhead \
       for constant evaluation. */ \
\
    if (SELF_EVAL(TYPEOF(e)) && --evalcount >= 0) { \
	/* Make sure constants in expressions have maximum NAMEDCNT when \
	   used as values, so they won't be modified. */ \
        SET_NAMEDCNT_MAX(e); \
        R_Visible = TRUE; \
        return e; \
    } \
} while (0)

SEXP eval(SEXP e, SEXP rho)
{
#   if (R_EVAL_TWEAKS/10)%10 == 0
        return Rf_evalv(e,rho,0);
#   else
        EVAL_PRELUDE;
        return Rf_evalv2(e,rho,0);
#   endif
}

SEXP evalv(SEXP e, SEXP rho, int variant)
{
    if (0) {
        /* THE "IF" CONDITION ABOVE IS NORMALLY 0; CAN SET TO 1 FOR DEBUGGING.
           Enabling this zeroing of variant will test that callers who normally
           get a variant result can actually handle an ordinary result. */
        variant = 0;
    }

    EVAL_PRELUDE;

#if R_EVAL_TWEAKS > 0

    return Rf_evalv2(e,rho,variant);
}

SEXP attribute_hidden Rf_evalv2(SEXP e, SEXP rho, int variant)
{

#endif

    /* Handle check for user interrupt.  When negative, repeats check for 
       SELF_EVAL which may have already been done, but not acted on since
       evalcount went negative. */

    if (--evalcount < 0) {
        R_CheckUserInterrupt();
        evalcount = 1000;
        /* Evaluate constants quickly. */
        if (SELF_EVAL(TYPEOF(e))) {
            /* Make sure constants in expressions have maximum NAMEDCNT when
	       used as values, so they won't be modified. */
            SET_NAMEDCNT_MAX(e);
            R_Visible = TRUE;
            return e;
        }
    }

    SEXP op, res;

    R_EvalDepth += 1;

    if (R_EvalDepth > R_Expressions) {
	R_Expressions = R_Expressions_keep + 500;
	errorcall (R_NilValue /* avoids deparsing call in the error handler */,
         _("evaluation nested too deeply: infinite recursion / options(expressions=)?"));
    }

    R_CHECKSTACK();

#ifdef Win32
    /* This resets the precision, rounding and exception modes of a ix86 fpu. */
    __asm__ ( "fninit" );
#endif

    SEXPTYPE typeof_e = TYPEOF(e);

    if (typeof_e == SYMSXP) {

	if (e == R_DotsSymbol)
	    dotdotdot_error();

        R_Visible = TRUE;  /* May be set FALSE by active binding / lazy eval */

	res = DDVAL(e) ? ddfindVar(e,rho) : FIND_VAR_PENDING_OK (e, rho);

	if (res == R_UnboundValue)
            unbound_var_error(e);

        if (res == R_MissingArg) {
            if ( ! (variant & VARIANT_MISSING_OK))
                if (!DDVAL(e))  /* revert bug fix for the moment */
                    arg_missing_error(e);
        }
        else if (TYPEOF(res) == PROMSXP) {
            if (PRVALUE_PENDING_OK(res) == R_UnboundValue)
                res = forcePromiseUnbound(res,variant);
            else
                res = PRVALUE_PENDING_OK(res);
        }

        /* A NAMEDCNT of 0 might arise from an inadverently missing increment
           somewhere, or from a save/load sequence (since loaded values in
           promises have NAMEDCNT of 0), so fix up here... */

        if (NAMEDCNT_EQ_0(res))
            SET_NAMEDCNT_1(res);

        if ( ! (variant & VARIANT_PENDING_OK))
            WAIT_UNTIL_COMPUTED(res);
    }

    else if (typeof_e == LANGSXP) {

        SEXP fn = CAR(e), args = CDR(e);

        if (TYPEOF(fn) == SYMSXP)
            op = FINDFUN(fn,rho);
        else
            op = eval(fn,rho);

	if (RTRACE(op)) R_trace_call(e,op);

	if (TYPEOF(op) == CLOSXP) {
            PROTECT(op);
	    res = applyClosure_v (e, op, promiseArgs(args,rho), rho, 
                                  R_NoObject, variant);
            UNPROTECT(1);
        }
	else {
            int save = R_PPStackTop;
            const void *vmax = VMAXGET();

            R_Visible = TRUE;

            /* op is protected by PrimCache (see mkPRIMSXP). */
            if (TYPEOF(op) == SPECIALSXP)
                res = CALL_PRIMFUN (e, op, args, rho, variant);
            else if (TYPEOF(op) == BUILTINSXP)
                res = R_Profiling ? Rf_builtin_op(op, e, rho, variant)
                                  : Rf_builtin_op_no_cntxt(op, e, rho, variant);
            else
                apply_non_function_error();

            if (!R_Visible && PRIMPRINT(op) == 0)
                R_Visible = TRUE;

            CHECK_STACK_BALANCE(op, save);
            VMAXSET(vmax);
        }
    }

    else if (typeof_e == PROMSXP) {

	if (PRVALUE_PENDING_OK(e) == R_UnboundValue)
            res = forcePromiseUnbound(e,variant);
        else
            res = PRVALUE_PENDING_OK(e);

        if ( ! (variant & VARIANT_PENDING_OK))
            WAIT_UNTIL_COMPUTED(res);

        R_Visible = TRUE;
    }

    else if (typeof_e == BCODESXP) {

	res = bcEval(e, rho, TRUE);
    }

    else if (typeof_e == DOTSXP)
        dotdotdot_error();

    else
        UNIMPLEMENTED_TYPE("eval", e);

    R_EvalDepth -= 1;

#   if 0  /* Enable for debug output after typing STATIC.BOX.DEBUG */

    if (IS_STATIC_BOX(res) 
         && installed_already("STATIC.BOX.DEBUG") != R_No_object)
    { REprintf("STATIC BOX RETURNED: %s %f\n",
        TYPEOF(res)==INTSXP ? "int" : "real",
        TYPEOF(res)==INTSXP ? (double)*INTEGER(res) : *REAL(res));
    }

#   endif

    return res;
}


/* Like Rf_builtin_op (defined in builtin.c) except that no context is
   created.  Making this separate from Rf_builtin_op saves on stack
   space for the local context variable.  Since the somewhat
   time-consuming context creation is not done, there is no advantage
   to evaluating a single argument with pending OK. */

static SEXP Rf_builtin_op_no_cntxt (SEXP op, SEXP e, SEXP rho, int variant)
{
    SEXP args = CDR(e);
    SEXP arg1;
    SEXP res;

    /* See if this may be a fast primitive.  All fast primitives
       should be BUILTIN.  We do a fast call only if there is exactly
       one argument, with no tag, not missing or a ... argument.  
       The argument is stored in arg1. */

    if (args!=R_NilValue) {
        if (PRIMFUN_FAST(op) 
              && TAG(args)==R_NilValue && CDR(args)==R_NilValue
              && (arg1 = CAR(args))!=R_DotsSymbol 
              && arg1!=R_MissingArg && arg1!=R_MissingUnder) {

            PROTECT(arg1 = EVALV (arg1, rho, PRIMFUN_ARG1VAR(op)));

            if (isObject(arg1) && PRIMFUN_DSPTCH1(op)) {
                if ((PRIMFUN_ARG1VAR (op) & VARIANT_UNCLASS)
                       && (R_variant_result & VARIANT_UNCLASS_FLAG)) {
                    R_variant_result &= ~VARIANT_UNCLASS_FLAG;
                }
                else {
                    UNPROTECT(1);
                    PROTECT(args = CONS(arg1,R_NilValue));
                    goto not_fast;
                }
            }

            res = ((SEXP(*)(SEXP,SEXP,SEXP,SEXP,int)) PRIMFUN_FAST(op)) 
                     (e, op, arg1, rho, variant);

            UNPROTECT(1);  /* arg1 */
            return res;
        }

        args = evalList (args, rho);
    }

    PROTECT(args);

    /* Handle a non-fast op.  We may get here after starting to handle a
       fast op, but if so, args has been set to the evaluated argument list. */

  not_fast: 

    res = CALL_PRIMFUN(e, op, args, rho, variant);

    UNPROTECT(1); /* args */
    return res;
}


attribute_hidden
void SrcrefPrompt(const char * prefix, SEXP srcref)
{
    /* If we have a valid srcref, use it */
    if (srcref && srcref != R_NilValue) {
        if (TYPEOF(srcref) == VECSXP) srcref = VECTOR_ELT(srcref, 0);
	SEXP srcfile = getAttrib00(srcref, R_SrcfileSymbol);
	if (TYPEOF(srcfile) == ENVSXP) {
	    SEXP filename = findVar(install("filename"), srcfile);
	    if (isString(filename) && length(filename)) {
	    	Rprintf(_("%s at %s#%d: "), prefix, CHAR(STRING_ELT(filename, 0)), 
	                                    asInteger(srcref));
	        return;
	    }
	}
    }
    /* default: */
    Rprintf("%s: ", prefix);
}

/* Apply SEXP op of type CLOSXP to actuals */

static void loadCompilerNamespace(void)
{
    SEXP fun, arg, expr;

    PROTECT(fun = install("getNamespace"));
    PROTECT(arg = mkString("compiler"));
    PROTECT(expr = lang2(fun, arg));
    eval(expr, R_GlobalEnv);
    UNPROTECT(3);
}

static int R_disable_bytecode = 0;

void attribute_hidden R_init_jit_enabled(void)
{
    if (R_jit_enabled <= 0) {
	char *enable = getenv("R_ENABLE_JIT");
	if (enable != NULL) {
	    int val = atoi(enable);
	    if (val > 0)
		loadCompilerNamespace();
	    R_jit_enabled = val;
	}
    }

    if (R_compile_pkgs <= 0) {
	char *compile = getenv("R_COMPILE_PKGS");
	if (compile != NULL) {
	    int val = atoi(compile);
	    if (val > 0)
		R_compile_pkgs = TRUE;
	    else
		R_compile_pkgs = FALSE;
	}
    }

    if (R_disable_bytecode <= 0) {
	char *disable = getenv("R_DISABLE_BYTECODE");
	if (disable != NULL) {
	    int val = atoi(disable);
	    if (val > 0)
		R_disable_bytecode = TRUE;
	    else
		R_disable_bytecode = FALSE;
	}
    }
}
    
SEXP attribute_hidden R_cmpfun(SEXP fun)
{
    SEXP packsym, funsym, call, fcall, val;

    packsym = install("compiler");
    funsym = install("tryCmpfun");

    PROTECT(fcall = lang3(R_TripleColonSymbol, packsym, funsym));
    PROTECT(call = lang2(fcall, fun));
    val = eval(call, R_GlobalEnv);
    UNPROTECT(2);
    return val;
}

static SEXP R_compileExpr(SEXP expr, SEXP rho)
{
    SEXP packsym, funsym, quotesym;
    SEXP qexpr, call, fcall, val;

    packsym = install("compiler");
    funsym = install("compile");
    quotesym = install("quote");

    PROTECT(fcall = lang3(R_DoubleColonSymbol, packsym, funsym));
    PROTECT(qexpr = lang2(quotesym, expr));
    PROTECT(call = lang3(fcall, qexpr, rho));
    val = eval(call, R_GlobalEnv);
    UNPROTECT(3);
    return val;
}

static SEXP R_compileAndExecute(SEXP call, SEXP rho)
{
    int old_enabled = R_jit_enabled;
    SEXP code, val;

    R_jit_enabled = 0;
    PROTECT(call);
    PROTECT(rho);
    PROTECT(code = R_compileExpr(call, rho));
    R_jit_enabled = old_enabled;

    val = bcEval(code, rho, TRUE);
    UNPROTECT(3);
    return val;
}

static SEXP do_enablejit(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int old = R_jit_enabled, new;
    checkArity(op, args);
    new = asInteger(CAR(args));
    if (new > 0)
	loadCompilerNamespace();
    R_jit_enabled = new;
    return ScalarIntegerMaybeConst(old);
}

static SEXP do_compilepkgs(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int old = R_compile_pkgs, new;
    checkArity(op, args);
    new = asLogical(CAR(args));
    if (new != NA_LOGICAL && new)
	loadCompilerNamespace();
    R_compile_pkgs = new;
    return ScalarLogicalMaybeConst(old);
}

/* forward declaration */
static SEXP bytecodeExpr(SEXP);

/* This function gets the srcref attribute from a statement block, 
   and confirms it's in the expected format */
   
static R_INLINE void getBlockSrcrefs(SEXP call, SEXP **refs, int *len)
{
    SEXP srcrefs = getAttrib00(call, R_SrcrefSymbol);
    if (TYPEOF(srcrefs) == VECSXP) {
        *refs = (SEXP *) DATAPTR(srcrefs);
        *len = LENGTH(srcrefs);
    }
    else
    {   *len = 0;
    }
}

/* This function extracts one srcref, and confirms the format.  It is 
   passed an index and the array and length from getBlockSrcrefs. */

static R_INLINE SEXP getSrcref(SEXP *refs, int len, int ind)
{
    if (ind < len) {
        SEXP result = refs[ind];
        if (TYPEOF(result) == INTSXP && LENGTH(result) >= 6)
            return result;
    }

    return R_NilValue;
}

static void printcall (SEXP call, SEXP rho)
{
    int old_bl = R_BrowseLines;
    int blines = asInteger(GetOption1(install("deparse.max.lines")));
    if (blines != NA_INTEGER && blines > 0) R_BrowseLines = blines;
    PrintValueRec(call,rho);
    R_BrowseLines = old_bl;
}

static void start_browser (SEXP call, SEXP op, SEXP stmt, SEXP env)
{
    SrcrefPrompt("debug", R_Srcref);
    PrintValue(stmt);
    do_browser(call, op, R_NilValue, env);
}

SEXP attribute_hidden applyClosure_v(SEXP call, SEXP op, SEXP arglist, SEXP rho,
                                     SEXP suppliedenv, int variant)
{
    int vrnt = VARIANT_PENDING_OK | VARIANT_DIRECT_RETURN 
                 | VARIANT_PASS_ON(variant);

    SEXP formals, actuals, savedrho, savedsrcref;
    volatile SEXP body, newrho;
    SEXP f, a, res;
    RCNTXT cntxt;

    PROTECT2(op,arglist);

    formals = FORMALS(op);
    body = BODY(op);
    savedrho = CLOENV(op);

    if (R_jit_enabled > 0 && TYPEOF(body) != BCODESXP) {
	int old_enabled = R_jit_enabled;
	SEXP newop;
	R_jit_enabled = 0;
	newop = R_cmpfun(op);
	body = BODY(newop);
	SET_BODY(op, body);
	R_jit_enabled = old_enabled;
    }

    /*  Set up a context with the call in it for use if an error occurs below
        in matchArgs or from running out of memory (eg, in NewEnvironment). */

    begincontext(&cntxt, CTXT_RETURN, call, savedrho, rho, arglist, op);
    savedsrcref = R_Srcref;  /* saved in context for longjmp, and protection */

    /*  Build a list which matches the actual (unevaluated) arguments
	to the formal paramters.  Build a new environment which
	contains the matched pairs.  Note that actuals is protected via
        newrho. */

    actuals = matchArgs(formals, NULL, 0, arglist, call);
    PROTECT(newrho = NewEnvironment(R_NilValue, actuals, savedrho));
        /* no longer passes formals, since matchArg now puts tags in actuals */

    /* This piece of code is destructively modifying the actuals list,
       which is now also the list of bindings in the frame of newrho.
       This is one place where internal structure of environment
       bindings leaks out of envir.c.  It should be rewritten
       eventually so as not to break encapsulation of the internal
       environment layout.  We can live with it for now since it only
       happens immediately after the environment creation.  LT */

    f = formals;
    a = actuals;
    while (f != R_NilValue) {
	if (MISSING(a) && CAR(f) != R_MissingArg) {
	    SETCAR(a, mkPROMISE(CAR(f), newrho));
	    SET_MISSING(a, 2);
	}
	f = CDR(f);
	a = CDR(a);
    }

    setNoSpecSymFlag (newrho);

    /*  Fix up any extras that were supplied by usemethod. */

    if (suppliedenv != R_NoObject) {
	for (SEXP t = FRAME(suppliedenv); t != R_NilValue; t = CDR(t)) {
	    for (a = actuals; a != R_NilValue; a = CDR(a))
		if (TAG(a) == TAG(t))
		    break;
	    if (a == R_NilValue)
		set_var_in_frame (TAG(t), CAR(t), newrho, TRUE, 3);
	}
    }

    UNPROTECT(1); /* newrho, which will be protected below via revised context*/

    /*  Change the previously-set-up context to have the correct environment.

        If we have a generic function we need to use the sysparent of
	the generic as the sysparent of the method because the method
	is a straight substitution of the generic. */

    if (R_GlobalContext->nextcontext->callflag == CTXT_GENERIC)
	revisecontext (newrho, R_GlobalContext->nextcontext->sysparent);
    else
	revisecontext (newrho, rho);

    /* Get the srcref record from the closure object */
    
    R_Srcref = getAttrib00(op, R_SrcrefSymbol);

    /* Debugging */

    if (RDEBUG(op) | RSTEP(op)) {
        SET_RDEBUG(newrho, 1);
        if (RSTEP(op)) SET_RSTEP(op, 0);
	SEXP savesrcref; SEXP *srcrefs; int len;
	/* switch to interpreted version when debugging compiled code */
	if (TYPEOF(body) == BCODESXP)
	    body = bytecodeExpr(body);
	Rprintf("debugging in: ");
        printcall(call,rho);
	savesrcref = R_Srcref;
	getBlockSrcrefs(body,&srcrefs,&len);
	PROTECT(R_Srcref = getSrcref(srcrefs,len,0));
        start_browser (call, op, body, newrho);
	R_Srcref = savesrcref;
	UNPROTECT(1);
    }

    /*  It isn't completely clear that this is the right place to do
	this, but maybe (if the matchArgs above reverses the
	arguments) it might just be perfect.

	This will not currently work as the entry points in envir.c
	are static.
    */

#ifdef  HASHING
    {
	SEXP R_NewHashTable(int);
	SEXP R_HashFrame(SEXP);
	int nargs = length(arglist);
	HASHTAB(newrho) = R_NewHashTable(nargs);
	newrho = R_HashFrame(newrho);
    }
#endif
#undef  HASHING

    /*  Set a longjmp target which will catch any explicit returns
	from the function body.  */

    if ((SETJMP(cntxt.cjmpbuf))) {
	if (R_ReturnedValue == R_RestartToken) {
	    cntxt.callflag = CTXT_RETURN;  /* turn restart off */
	    R_ReturnedValue = R_NilValue;  /* remove restart token */
	    PROTECT(res = evalv (body, newrho, vrnt));
	}
	else {
	    PROTECT(res = R_ReturnedValue);
        }
    }
    else {
	PROTECT(res = evalv (body, newrho, vrnt));
    }

    R_variant_result &= ~VARIANT_RTN_FLAG;

    R_Srcref = savedsrcref;
    endcontext(&cntxt);

    if ( ! (variant & VARIANT_PENDING_OK))
        WAIT_UNTIL_COMPUTED(res);

    if (RDEBUG(op)) {
	Rprintf("exiting from: ");
        printcall(call,rho);
    }

    UNPROTECT(3); /* op, arglist, res */
    return res;
}

SEXP applyClosure (SEXP call, SEXP op, SEXP arglist, SEXP rho, 
                   SEXP suppliedenv)
{
  return applyClosure_v (call, op, arglist, rho, suppliedenv, 0);
}

/* **** FIXME: This code is factored out of applyClosure.  If we keep
   **** it we should change applyClosure to run through this routine
   **** to avoid code drift. */
static SEXP R_execClosure(SEXP call, SEXP op, SEXP arglist, SEXP rho,
			  SEXP newrho)
{
    volatile SEXP body;
    SEXP res, savedsrcref;
    RCNTXT cntxt;

    PROTECT2(op,arglist);

    body = BODY(op);

    if (R_jit_enabled > 0 && TYPEOF(body) != BCODESXP) {
	int old_enabled = R_jit_enabled;
	SEXP newop;
	R_jit_enabled = 0;
	newop = R_cmpfun(op);
	body = BODY(newop);
	SET_BODY(op, body);
	R_jit_enabled = old_enabled;
    }

    begincontext(&cntxt, CTXT_RETURN, call, newrho, rho, arglist, op);
    savedsrcref = R_Srcref;  /* saved in context for longjmp, and protection */

    /* Get the srcref record from the closure object.  Disable for now
       at least, since it's not clear that it's needed. */
    
    R_Srcref = R_NilValue;  /* was: getAttrib(op, R_SrcrefSymbol); */

    /* Debugging */

    if (RDEBUG(op) | RSTEP(op)) {
        SET_RDEBUG(newrho, 1);
        if (RSTEP(op)) SET_RSTEP(op, 0);
        SEXP savesrcref; SEXP *srcrefs; int len;
	/* switch to interpreted version when debugging compiled code */
	if (TYPEOF(body) == BCODESXP)
	    body = bytecodeExpr(body);
	Rprintf("debugging in: ");
	printcall (call, rho);
	savesrcref = R_Srcref;
	getBlockSrcrefs(body,&srcrefs,&len);
	PROTECT(R_Srcref = getSrcref(srcrefs,len,0));
        start_browser (call, op, body, newrho);
	R_Srcref = savesrcref;
	UNPROTECT(1);
    }

    /*  It isn't completely clear that this is the right place to do
	this, but maybe (if the matchArgs above reverses the
	arguments) it might just be perfect.  */

#ifdef  HASHING
#define HASHTABLEGROWTHRATE  1.2
    {
	SEXP R_NewHashTable(int, double);
	SEXP R_HashFrame(SEXP);
	int nargs = length(arglist);
	HASHTAB(newrho) = R_NewHashTable(nargs, HASHTABLEGROWTHRATE);
	newrho = R_HashFrame(newrho);
    }
#endif
#undef  HASHING

    /*  Set a longjmp target which will catch any explicit returns
	from the function body.  */

    if ((SETJMP(cntxt.cjmpbuf))) {
	if (R_ReturnedValue == R_RestartToken) {
	    cntxt.callflag = CTXT_RETURN;  /* turn restart off */
	    R_ReturnedValue = R_NilValue;  /* remove restart token */
	    PROTECT(res = eval(body, newrho));
	}
	else {
	    PROTECT(res = R_ReturnedValue);
            WAIT_UNTIL_COMPUTED(res);
        }
    }
    else {
	PROTECT(res = eval(body, newrho));
    }

    R_Srcref = savedsrcref;
    endcontext(&cntxt);

    if (RDEBUG(op)) {
	Rprintf("exiting from: ");
	printcall (call, rho);
    }

    UNPROTECT(3);  /* op, arglist, res */
    return res;
}

/* **** FIXME: Temporary code to execute S4 methods in a way that
   **** preserves lexical scope. */

/* called from methods_list_dispatch.c */
SEXP R_execMethod(SEXP op, SEXP rho)
{
    SEXP call, arglist, callerenv, newrho, next, val;
    RCNTXT *cptr;

    /* create a new environment frame enclosed by the lexical
       environment of the method */
    PROTECT(newrho = Rf_NewEnvironment(R_NilValue, R_NilValue, CLOENV(op)));

    /* copy the bindings for the formal environment from the top frame
       of the internal environment of the generic call to the new
       frame.  need to make sure missingness information is preserved
       and the environments for any default expression promises are
       set to the new environment.  should move this to envir.c where
       it can be done more efficiently. */
    for (next = FORMALS(op); next != R_NilValue; next = CDR(next)) {
	SEXP symbol =  TAG(next);
	R_varloc_t loc;
	int missing;
	loc = R_findVarLocInFrame(rho,symbol);
	if (loc == R_NoObject)
	    error(_("could not find symbol \"%s\" in environment of the generic function"),
		  CHAR(PRINTNAME(symbol)));
	missing = R_GetVarLocMISSING(loc);
	val = R_GetVarLocValue(loc);
	SET_FRAME(newrho, CONS(val, FRAME(newrho)));
	SET_TAG(FRAME(newrho), symbol);
	if (missing) {
	    SET_MISSING(FRAME(newrho), missing);
	    if (TYPEOF(val) == PROMSXP && PRENV(val) == rho) {
		SEXP deflt;
		SET_PRENV(val, newrho);
		/* find the symbol in the method, copy its expression
		 * to the promise */
		for(deflt = CAR(op); deflt != R_NilValue; deflt = CDR(deflt)) {
		    if(TAG(deflt) == symbol)
			break;
		}
		if(deflt == R_NilValue)
		    error(_("symbol \"%s\" not in environment of method"),
			  CHAR(PRINTNAME(symbol)));
		SET_PRCODE(val, CAR(deflt));
	    }
	}
    }

    /* copy the bindings of the spacial dispatch variables in the top
       frame of the generic call to the new frame */
    defineVar(R_dot_defined, findVarInFrame(rho, R_dot_defined), newrho);
    defineVar(R_dot_Method, findVarInFrame(rho, R_dot_Method), newrho);
    defineVar(R_dot_target, findVarInFrame(rho, R_dot_target), newrho);

    /* copy the bindings for .Generic and .Methods.  We know (I think)
       that they are in the second frame, so we could use that. */
    defineVar(R_dot_Generic, findVar(R_dot_Generic, rho), newrho);
    defineVar(R_dot_Methods, findVar(R_dot_Methods, rho), newrho);

    /* Find the calling context.  Should be R_GlobalContext unless
       profiling has inserted a CTXT_BUILTIN frame. */
    cptr = R_GlobalContext;
    if (cptr->callflag & CTXT_BUILTIN)
	cptr = cptr->nextcontext;

    /* The calling environment should either be the environment of the
       generic, rho, or the environment of the caller of the generic,
       the current sysparent. */
    callerenv = cptr->sysparent; /* or rho? */

    /* get the rest of the stuff we need from the current context,
       execute the method, and return the result */
    call = cptr->call;
    arglist = cptr->promargs;
    val = R_execClosure(call, op, arglist, callerenv, newrho);
    UNPROTECT(1);
    return val;
}

                                  /* Caller needn't protect the s arg below */
static R_INLINE Rboolean asLogicalNoNA(SEXP s, SEXP call)
{
    int len, cond;

    switch(TYPEOF(s)) { /* common cases done here for efficiency */
    case INTSXP:  /* assume logical and integer are the same */
    case LGLSXP:
        len = LENGTH(s);
        if (len == 0 || LOGICAL(s)[0] == NA_LOGICAL) goto error;
        cond = LOGICAL(s)[0];
        break;
    default:
        len = length(s);
        if (len == 0) goto error;
        cond = asLogical(s);
        break;
    }

    if (cond == NA_LOGICAL) goto error;

    if (len > 1) asLogicalNoNA_warning (s, call);

    return cond;

  error:
    asLogicalNoNA_error (s, call);
}


#define BodyHasBraces(body) \
    (isLanguage(body) && CAR(body) == R_BraceSymbol)


static SEXP do_if (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP Cond, Stmt;
    int absent_else = 0;

    Cond = CAR(args); args = CDR(args);
    Stmt = CAR(args); args = CDR(args);

    if (!asLogicalNoNA (evalv(Cond,rho,VARIANT_STATIC_BOX_OK), call)) {
        /* go to else part */
        if (args != R_NilValue)
            Stmt = CAR(args);
        else {
            absent_else = 1;
            Stmt = R_NilValue;
        }
    }

    if (RDEBUG(rho) && Stmt!=R_NilValue && !BodyHasBraces(Stmt))
        start_browser (call, op, Stmt, rho);

    if (absent_else) {
        R_Visible = FALSE; /* case of no 'else' so return invisible NULL */
        return R_NilValue;
    }

    return evalv (Stmt, rho, VARIANT_PASS_ON(variant));
}


/* For statement.  Unevaluated arguments for different formats are as follows:

       for (i in v) body          i, v, body
       for (i down v) body        i, down=v, body
       for (i across v) body      i, across=v, body
       for (i along v) body       i, along=v, body     (ok for vec or for array)
       for (i, j along M) body    i, j, along=M, body     (requires correct dim)
       etc.

   Extra variables after i are ignored for 'in', 'down', and 'across'.

   Evaluates body with VARIANT_NULL | VARIANT_PENDING_OK.
 */

#define DO_LOOP_RDEBUG(call, op, body, rho, bgn) do { \
        if (!bgn && RDEBUG(rho)) start_browser (call, op, body, rho); \
    } while (0)

static SEXP do_for(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    /* Need to declare volatile variables whose values are relied on
       after for_next or for_break longjmps and that might change between
       the setjmp and longjmp calls.  Theoretically this does not include
       n, bgn, and some others, but gcc -O2 -Wclobbered warns about some, 
       so to be safe we declare them volatile as well. */

    volatile int i, n, bgn;
    volatile SEXP val, nval;
    volatile SEXP v, bcell;                /* for use with one 'for' variable */
    volatile SEXP indexes, ixvals, bcells; /* for use with >1 'for' variables */
    int dbg, val_type;
    SEXP a, syms, sym, body, dims;
    RCNTXT cntxt;
    PROTECT_INDEX valpi, vpi, bix;
    int is_seq, seq_start;
    int along = 0, across = 0, down = 0, in = 0;
    int nsyms;

    R_Visible = FALSE;

    PROTECT(args);

    /* Count how many variables there are before the argument after the "in",
       "across", "down", or "along" keyword.  Set 'a' to the cell for the 
       argument after these variables. */

    syms = args;
    nsyms = 0;
    a = args;
    do {
        if (!isSymbol(CAR(a))) errorcall(call, _("non-symbol loop variable"));
        a = CDR(a);
        nsyms += 1;
    } while (CDDR(a) != R_NilValue);

    if (TAG(a) == R_AlongSymbol)
        along = 1;
    else if (TAG(a) == R_AcrossSymbol)
        across = 1;
    else if (TAG(a) == R_DownSymbol)
        down = 1;
    else
        in = 1;  /* we treat any other (or no) tag as "in" */

    /* Sometimes handled by bytecode... */

    if (in && nsyms == 1 && R_jit_enabled > 2 && ! R_PendingPromises) {
	R_compileAndExecute(call, rho); 
	return R_NilValue;
    }

    if (!along) nsyms = 1;  /* ignore extras when not 'along' */

    val = CAR(a);
    body = CADR(a);
    sym = CAR(syms);

    PROTECT(rho);

    PROTECT(val = evalv(val, rho, in    ? VARIANT_SEQ | VARIANT_ANY_ATTR :
                                  along ? VARIANT_UNCLASS | VARIANT_ANY_ATTR :
                                    VARIANT_UNCLASS | VARIANT_ANY_ATTR_EX_DIM));
    dims = R_NilValue;

    is_seq = 0;

    if (along) { /* "along" and therefore not seq variant */
        R_variant_result = 0;
        if (nsyms == 1) { /* go along vector/pairlist (may also be an array) */
            is_seq = 1;
            seq_start = 1;
            val_type = INTSXP;
        }
        else { /* "along" for array, with several dimensions */
            dims = getAttrib (val, R_DimSymbol);
            if (length(dims) != nsyms)
                errorcall (call, _("incorrect number of dimensions"));
            PROTECT(dims);
            INC_NAMEDCNT(dims);
            PROTECT(indexes = allocVector(INTSXP,nsyms));
            INTEGER(indexes)[0] = 0; /* so will be 1 after first increment */
            for (int j = 1; j < nsyms; j++) 
                INTEGER(indexes)[j] = 1;
            PROTECT(ixvals = allocVector(VECSXP,nsyms));
            PROTECT(bcells = allocVector(VECSXP,nsyms));
        }
        n = length(val);
    }
    else if (across || down) { /* "across" or "down", hence not seq variant*/
        R_variant_result = 0;
        is_seq = 1;
        seq_start = 1;
        val_type = INTSXP;
        dims = getAttrib (val, R_DimSymbol);
        if (TYPEOF(dims)!=INTSXP || LENGTH(dims)==0) /* no valid dim attribute*/
            n = length(val);
        else if (down)
            n = INTEGER(dims)[0];
        else /* across */
            n = LENGTH(dims) > 1 ? INTEGER(dims)[1] : INTEGER(dims)[0];
    }
    else if (R_variant_result) {  /* variant "in" value */
        R_variant_result = 0;
        is_seq = 1;
        if (TYPEOF(val)!=INTSXP || LENGTH(val)!=2) /* shouldn't happen*/
            errorcall(call, "internal inconsistency with variant op in for!");
        seq_start = INTEGER(val)[0];
        n = INTEGER(val)[1] - INTEGER(val)[0] + 1;
        val_type = INTSXP;
    }
    else { /* non-variant "in" value */

        PROTECT_WITH_INDEX (val, &valpi);
        INC_NAMEDCNT(val);  /* increment NAMEDCNT to avoid mods by loop code */
        nval = val;  /* for scanning pairlist */

        /* Deal with the case where we are iterating over a factor.
           We need to coerce to character, then iterate */

        if (inherits (val, "factor"))
            REPROTECT(val = asCharacterFactor(val), valpi);

        n = length(val);
        val_type = TYPEOF(val);
    }

    dbg = RDEBUG(rho);
    bgn = BodyHasBraces(body);

    if (nsyms == 1) {
        PROTECT_WITH_INDEX(v = R_NilValue, &vpi);
        PROTECT_WITH_INDEX(bcell = R_NilValue, &bix);
    }

    begincontext(&cntxt, CTXT_LOOP, R_NilValue, rho, R_BaseEnv, R_NilValue,
		 R_NilValue);

    switch (SETJMP(cntxt.cjmpbuf)) {
    case CTXT_BREAK: goto for_break;
    case CTXT_NEXT: goto for_next;
    }

    /* Loop variables are initialized to R NULL. */

    int j;
    SEXP s;
    for (j = 0, s = syms; j < nsyms; j++, s = CDR(s)) { 
        set_var_in_frame (CAR(s), R_NilValue, rho, TRUE, 3);
    }

    /* MAIN LOOP. */

    for (i = 0; i < n; i++) {

        /* Handle multi-dimensional "along". */

        if (nsyms > 1) {

            /* Increment to next combination of indexes. */

            for (j = 0; INTEGER(indexes)[j] == INTEGER(dims)[j]; j++) {
                if (j == nsyms-1) abort();
                INTEGER(indexes)[j] = 1;
            }
            INTEGER(indexes)[j] += 1;

            /* Make sure all 'for' variables are set to the right index,
               using records of the binding cells used for speed. */

            for (j = 0, s = syms; j < nsyms; j++, s = CDR(s)) {
                SEXP v = VECTOR_ELT(ixvals,j);
                if (v==R_NilValue || NAMEDCNT_GT_1(v) || HAS_ATTRIB(v)){
                    v = allocVector(INTSXP,1);
                    SET_VECTOR_ELT(ixvals,j,v);
                }
                INTEGER(v)[0] = INTEGER(indexes)[j];
                SEXP bcell = VECTOR_ELT(bcells,j);
                if (bcell == R_NilValue || CAR(bcell) != v) {
                    set_var_in_frame (CAR(s), v, rho, TRUE, 3);
                    SET_VECTOR_ELT(bcells,j,R_binding_cell);
                }
            }
            
            goto do_iter;
        }

        /* Handle "in", "across", "down", and univariate "along". */

	switch (val_type) {

	case EXPRSXP:
	case VECSXP:
	    v = VECTOR_ELT(val, i);
	    SET_NAMEDCNT_MAX(v); /* maybe unnecessary? */
	    break;

	case LISTSXP:
	    v = CAR(nval);
	    nval = CDR(nval);
	    SET_NAMEDCNT_MAX(v);
	    break;

	default:

            /* Allocate new space for the loop variable value when the value has
               been assigned to another variable (NAMEDCNT(v) > 1), and when an
               attribute has been attached to it. */

            if (v == R_NilValue || NAMEDCNT_GT_1(v) || HAS_ATTRIB(v))
                REPROTECT(v = allocVector(val_type, 1), vpi);

            switch (val_type) {
            case LGLSXP:
                LOGICAL(v)[0] = LOGICAL(val)[i];
                break;
            case INTSXP:
                INTEGER(v)[0] = is_seq ? seq_start + i : INTEGER(val)[i];
                break;
            case REALSXP:
                REAL(v)[0] = REAL(val)[i];
                break;
            case CPLXSXP:
                COMPLEX(v)[0] = COMPLEX(val)[i];
                break;
            case STRSXP:
                SET_STRING_ELT(v, 0, STRING_ELT(val, i));
                break;
            case RAWSXP:
                RAW(v)[0] = RAW(val)[i];
                break;
            default:
                errorcall(call, _("invalid for() loop sequence"));
            }

            break;
        }

        if (bcell == R_NilValue || CAR(bcell) != v) {
            set_var_in_frame (sym, v, rho, TRUE, 3);
            REPROTECT(bcell = R_binding_cell, bix);
        }

    do_iter: ;

        DO_LOOP_RDEBUG(call, op, body, rho, bgn);

        evalv (body, rho, VARIANT_NULL | VARIANT_PENDING_OK);

    for_next: ;  /* semi-colon needed for attaching label */
    }

 for_break:
    endcontext(&cntxt);
    if (in && !is_seq) {
        DEC_NAMEDCNT(val);
        UNPROTECT(1);  /* val */
    }
    if (nsyms == 1)
        UNPROTECT(2);  /* v, bcell */
    else 
        UNPROTECT(4);  /* dims, indexes, ixvals, bcells */
    UNPROTECT(3);      /* val, rho, args */
    SET_RDEBUG(rho, dbg);

    R_Visible = FALSE;
    return R_NilValue;
}


/* While statement.  Evaluates body with VARIANT_NULL | VARIANT_PENDING_OK. */

static SEXP do_while(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int dbg;
    volatile int bgn;
    volatile SEXP body;
    RCNTXT cntxt;

    R_Visible = FALSE;

    if (R_jit_enabled > 2 && ! R_PendingPromises) {
	R_compileAndExecute(call, rho);
	return R_NilValue;
    }

    dbg = RDEBUG(rho);
    body = CADR(args);
    bgn = BodyHasBraces(body);

    begincontext(&cntxt, CTXT_LOOP, R_NilValue, rho, R_BaseEnv, R_NilValue,
		 R_NilValue);

    if (SETJMP(cntxt.cjmpbuf) != CTXT_BREAK) { /* <- back here for "next" */
	while (asLogicalNoNA 
                 (evalv (CAR(args), rho, VARIANT_STATIC_BOX_OK), call)) {
	    DO_LOOP_RDEBUG(call, op, body, rho, bgn);
	    evalv (body, rho, VARIANT_NULL | VARIANT_PENDING_OK);
	}
    }

    endcontext(&cntxt);
    SET_RDEBUG(rho, dbg);

    R_Visible = FALSE;
    return R_NilValue;
}


/* Repeat statement.  Evaluates body with VARIANT_NULL | VARIANT_PENDING_OK. */

static SEXP do_repeat(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int dbg;
    volatile int bgn;
    volatile SEXP body;
    RCNTXT cntxt;

    R_Visible = FALSE;

    if (R_jit_enabled > 2 && ! R_PendingPromises) {
	R_compileAndExecute(call, rho);
	return R_NilValue;
    }

    dbg = RDEBUG(rho);
    body = CAR(args);
    bgn = BodyHasBraces(body);

    begincontext(&cntxt, CTXT_LOOP, R_NilValue, rho, R_BaseEnv, R_NilValue,
		 R_NilValue);

    if (SETJMP(cntxt.cjmpbuf) != CTXT_BREAK) { /* <- back here for "next" */
	for (;;) {
	    DO_LOOP_RDEBUG(call, op, body, rho, bgn);
	    evalv (body, rho, VARIANT_NULL | VARIANT_PENDING_OK);
	}
    }

    endcontext(&cntxt);
    SET_RDEBUG(rho, dbg);

    R_Visible = FALSE;
    return R_NilValue;
}


static R_NORETURN SEXP do_break(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    findcontext(PRIMVAL(op), rho, R_NilValue);
}

/* Parens are now a SPECIAL, to avoid overhead of creating an arg list. 
   Also avoids overhead of calling checkArity when there is no error.  
   Care is taken to allow (...) when ... is bound to exactly one argument, 
   though it is debatable whether this should be considered valid. 

   The eval variant requested is passed on to the inner expression. */

static SEXP do_paren (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    if (args!=R_NilValue && CAR(args)==R_DotsSymbol && CDR(args)==R_NilValue) {
        args = findVar(R_DotsSymbol, rho);
        if (TYPEOF(args) != DOTSXP)
            args = R_NilValue;
    }

    if (args == R_NilValue || CDR(args) != R_NilValue)
        checkArity(op, args);

    return EVALV (CAR(args), rho, VARIANT_PASS_ON(variant));
}

/* Curly brackets.  Passes on the eval variant to the last expression.  For
   earlier expressions, passes either VARIANT_NULL | VARIANT_PENDING_OK or
   the variant passed OR'd with those, if the variant passed includes
   VARIANT_DIRECT_RETURN. */

static SEXP do_begin (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    if (args == R_NilValue)
        return R_NilValue;

    SEXP arg, s;
    SEXP savedsrcref = R_Srcref;
    int len;
    SEXP *srcrefs;

    getBlockSrcrefs(call,&srcrefs,&len);

    int vrnt = VARIANT_NULL | VARIANT_PENDING_OK;
    variant = VARIANT_PASS_ON(variant);
    if (variant & VARIANT_DIRECT_RETURN) 
        vrnt |= variant;

    for (int i = 1; ; i++) {
        arg = CAR(args);
        args = CDR(args);
        R_Srcref = getSrcref (srcrefs, len, i);
        if (RDEBUG(rho))
            start_browser (call, op, arg, rho);
        if (args == R_NilValue)
            break;
        s = evalv (arg, rho, vrnt);
        if (R_variant_result & VARIANT_RTN_FLAG) {
            R_Srcref = savedsrcref;
            return s;
        }
    }

    s = EVALV (arg, rho, variant);
    R_Srcref = savedsrcref;
    return s;
}


static SEXP do_return(SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP v;

    if (args == R_NilValue) /* zero arguments provided */
	v = R_NilValue;
    else if (CDR(args) == R_NilValue) /* one argument */
	v = EVALV (CAR(args), rho, ! (variant & VARIANT_DIRECT_RETURN) ? 0
                    : VARIANT_PASS_ON(variant) & ~ VARIANT_NULL);
    else
	errorcall(call, _("multi-argument returns are not permitted"));

    if (variant & VARIANT_DIRECT_RETURN) {
        R_variant_result |= VARIANT_RTN_FLAG;
        return v;
    }

    findcontext(CTXT_BROWSER | CTXT_FUNCTION, rho, v);
}

/* Declared with a variable number of args in names.c */
static SEXP do_function(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP rval, srcref;

    /* The following is as in 2.15.0, but it's not clear how it can happen. */
    if (TYPEOF(op) == PROMSXP) {
	op = forcePromise(op);
	SET_NAMEDCNT_MAX(op);
    }

    CheckFormals(CAR(args));
    rval = mkCLOSXP(CAR(args), CADR(args), rho);
    srcref = CADDR(args);
    if (srcref != R_NilValue) 
        setAttrib(rval, R_SrcrefSymbol, srcref);
    return rval;
}


/* START OF DEFUNCT CODE ---------------------------------------------------- */
/*                                                                            */
/* This code is no longer used, unless a #if 0 is changed to #if 1 in do_set. */

static SEXP replaceCall(SEXP fun, SEXP varval, SEXP args, SEXP rhs);
static SEXP installAssignFcnName(SEXP fun);

/*  -------------------------------------------------------------------
 *  Assignments for complex LVAL specifications. This is the stuff that
 *  nightmares are made of ...	
 *
 *  For complex superassignment  x[y==z]<<-w  we want x required to be 
 *  nonlocal, y,z, and w permitted to be local or nonlocal.
 *
 *  If val is a language object, we must prevent evaluation.  As an
 *  example consider  e <- quote(f(x=1,y=2)); names(e) <- c("","a","b") 
 */

static SEXP EnsureLocal(SEXP symbol, SEXP rho)
{
    SEXP vl;

    vl = findVarInFrame3 (rho, symbol, TRUE);
    if (vl != R_UnboundValue) {
        if (TYPEOF(vl) == PROMSXP)
            vl = forcePromise(vl);
        return vl;
    }

    if (rho != R_EmptyEnv) {
        vl = findVar (symbol, ENCLOS(rho));
        if (TYPEOF(vl) == PROMSXP)
            vl = forcePromise(vl);
    }

    if (vl == R_UnboundValue)
        unbound_var_error(symbol);

    set_var_in_frame (symbol, vl, rho, TRUE, 3);
    return vl;
}

/* arguments of assignCall must be protected by the caller. */

static SEXP assignCall(SEXP op, SEXP symbol, SEXP fun,
		       SEXP varval, SEXP args, SEXP rhs)
{
    SEXP c;

    c = LCONS (op, CONS (symbol, 
          CONS (replaceCall(fun, varval, args, rhs), R_NilValue)));

    return c;
}

/*  "evalseq" preprocesses the LHS of an assignment.  Given an expression, 
 *  it builds a list of partial values for the expression.  For example, 
 *  the assignment 
 *
 *       x$a[[3]][2] <- 10
 *
 *  yields the (improper) list:
 *
 *       (eval(x$a[[3]])  eval(x$a)  eval(x) . x)
 *
 *  Note that the full LHS expression is not included (and not passed to
 *  evalseq).  Note also the terminating symbol in the improper list.  
 *  The partial evaluations are carried out efficiently using previously 
 *  computed components.
 *
 *  Each CONS cell in the list returned will have its LEVELS field set to 1
 *  if NAMEDCNT for its CAR or the CAR of any later element in the list is
 *  greater than 1 (and otherwise to 0).  This determines whether duplication 
 *  of the corresponding part of the object is neccessary.
 *
 *  The expr and rho arguments must be protected by the caller of evalseq.
 */

static SEXP evalseq(SEXP expr, SEXP rho, int forcelocal,  R_varloc_t tmploc)
{
    SEXP val, nval, nexpr, r;

    switch (TYPEOF(expr)) {

    case NILSXP:
	error(_("invalid (NULL) left side of assignment"));

    case SYMSXP:

        nval = forcelocal ? EnsureLocal(expr, rho) : eval(expr, ENCLOS(rho));

        /* This duplication should be unnecessary, but some packages
           (eg, Matrix 1.0-6) assume (in C code) that the object in a
           replacement function is not shared. */
        if (NAMEDCNT_GT_1(nval)) 
            nval = dup_top_level(nval);

	r = CONS(nval, expr);

        /* Statement below is now unnecessary (can always leave LEVELS at 0),
           given the duplication above. */
        /* SETLEVELS (r, NAMEDCNT_GT_1(nval)); */

        break;

    case LANGSXP:

	PROTECT(val = evalseq(CADR(expr), rho, forcelocal, tmploc));
	R_SetVarLocValue(tmploc, CAR(val));
	PROTECT(nexpr = LCONS (CAR(expr), 
                               LCONS(R_GetVarLocSymbol(tmploc), CDDR(expr))));
	nval = eval(nexpr, rho);
	UNPROTECT(2);

	r = CONS(nval, val);

        if (LEVELS(val) || NAMEDCNT_GT_1(nval))
            SETLEVELS (r, 1);

        break;

    default:
        error(_("target of assignment expands to non-language object"));
    }

    return r;
}

static void tmp_cleanup(void *data)
{
    (void) RemoveVariable (R_TmpvalSymbol, (SEXP) (uintptr_t) data);
}

/* Main entry point for complex assignments; rhs has already been evaluated. */

static void applydefine (SEXP call, SEXP op, SEXP expr, SEXP rhs, SEXP rho)
{
    SEXP lhs, tmp, afun, rhsprom, v;
    R_varloc_t tmploc;
    RCNTXT cntxt;

    if (rho == R_BaseNamespace)
	errorcall(call, _("cannot do complex assignments in base namespace"));
    if (rho == R_BaseEnv)
	errorcall(call, _("cannot do complex assignments in base environment"));

    /*  We need a temporary variable to hold the intermediate values
	in the computation.  For efficiency reasons we record the
	location where this variable is stored.  We need to protect
	the location in case the biding is removed from its
	environment by user code or an assignment within the
	assignment arguments */

    /*  There are two issues with the approach here:

	    A complex assignment within a complex assignment, like
	    f(x, y[] <- 1) <- 3, can cause the value temporary
	    variable for the outer assignment to be overwritten and
	    then removed by the inner one.  This could be addressed by
	    using multiple temporaries or using a promise for this
	    variable as is done for the RHS.  Printing of the
	    replacement function call in error messages might then need
	    to be adjusted.

	    With assignments of the form f(g(x, z), y) <- w the value
	    of 'z' will be computed twice, once for a call to g(x, z)
	    and once for the call to the replacement function g<-.  It
	    might be possible to address this by using promises.
	    Using more temporaries would not work as it would mess up
	    replacement functions that use substitute and/or
	    nonstandard evaluation (and there are packages that do
	    that -- igraph is one).

	    LT */

    defineVar(R_TmpvalSymbol, R_NilValue, rho);
    PROTECT((SEXP) (tmploc = R_findVarLocInFrame(rho, R_TmpvalSymbol)));

    /* Now set up a context to remove it when we are done, even in the
     * case of an error.  This all helps error() provide a better call.
     */
    begincontext(&cntxt, CTXT_CCODE, call, R_BaseEnv, R_BaseEnv,
		 R_NilValue, R_NilValue);
    cntxt.cend = &tmp_cleanup;
    cntxt.cenddata = (void *) (uintptr_t) rho;  /* DUBIOUS ??? */

    /*  Do a partial evaluation down through the LHS. */
    lhs = evalseq(CADR(expr), rho,
		  PRIMVAL(op)==1 || PRIMVAL(op)==3, tmploc);

    PROTECT(lhs);
    PROTECT(rhsprom = mkPROMISE(CADDR(call), rho));
    SET_PRVALUE(rhsprom, rhs);
    WAIT_UNTIL_COMPUTED(rhs);

    while (isLanguage(CADR(expr))) {
        PROTECT(tmp = installAssignFcnName(CAR(expr)));
        v = CAR(lhs);
        if (LEVELS(lhs) && v != R_NilValue) {
            v = duplicate(v);
            SET_NAMEDCNT_1(v);
            SETCAR(lhs,v);
        }
        R_SetVarLocValue(tmploc, v);
	PROTECT(rhs = replaceCall(tmp, R_TmpvalSymbol, CDDR(expr), rhsprom));
	rhs = eval (rhs, rho);
	SET_PRVALUE(rhsprom, rhs);
	SET_PRCODE(rhsprom, rhs); /* not good but is what we have been doing */
	UNPROTECT(2);
	lhs = CDR(lhs);
	expr = CADR(expr);
    }

    PROTECT(afun = installAssignFcnName(CAR(expr)));
    R_SetVarLocValue(tmploc, CAR(lhs));
    expr = assignCall(R_AssignSymbols[PRIMVAL(op)], CDR(lhs),
		      afun, R_TmpvalSymbol, CDDR(expr), rhsprom);
    UNPROTECT(4);
    PROTECT(expr);
    (void) eval(expr, rho);
    UNPROTECT(1);
    endcontext(&cntxt); /* which does not run the remove */
    (void) RemoveVariable (R_TmpvalSymbol, rho);
}

/* END OF DEFUNCT CODE ------------------------------------------------------ */


#define ASSIGNBUFSIZ 32
static SEXP installAssignFcnName(SEXP fun)
{
    /* Handle "[", "[[", and "$" specially for speed. */

    if (fun == R_BracketSymbol)
        return R_SubAssignSymbol;

    if (fun == R_Bracket2Symbol)
        return R_SubSubAssignSymbol;

    if (fun == R_DollarSymbol)
        return R_DollarAssignSymbol;

    /* The general case for a symbol */

    if (TYPEOF(fun) == SYMSXP) {

        char buf[ASSIGNBUFSIZ];
        const char *fname = CHAR(PRINTNAME(fun));

        if (!copy_2_strings (buf, sizeof buf, fname, "<-"))
            error(_("overlong name in '%s'"), fname);

        return install(buf);
    }

    /* Handle foo::bar and foo:::bar. */

    if (TYPEOF(fun)==LANGSXP && length(fun)==3 && TYPEOF(CADDR(fun))==SYMSXP
      && (CAR(fun)==R_DoubleColonSymbol || CAR(fun)==R_TripleColonSymbol))
        return lang3 (CAR(fun), CADR(fun), installAssignFcnName(CADDR(fun)));

    error(_("invalid function in complex assignment"));
}

/* arguments of replaceCall must be protected by the caller. */

static SEXP replaceCall(SEXP fun, SEXP varval, SEXP args, SEXP rhs)
{
    SEXP value, first;

    first = value = cons_with_tag (rhs, R_NilValue, R_ValueSymbol);

    if (args != R_NilValue) {

        first = cons_with_tag (CAR(args), value, TAG(args));

        SEXP p = CDR(args);
        if (p != R_NilValue) {
            PROTECT(first);
            SEXP q = first;
            do { 
                SETCDR (q, cons_with_tag (CAR(p), value, TAG(p)));
                q = CDR(q);
                p = CDR(p);
            } while (p != R_NilValue);
            UNPROTECT(1);
        }
    }

    first = LCONS (fun, CONS(varval, first));

    return first;
}

/* Create two lists of promises to evaluate each argument, with promises
   shared.  When an argument is evaluated in a call using one argument list,
   its value is then known without re-evaluation in a second call using 
   the second argument list.  The argument lists are terminated with the
   initial values of *a1 and *a2. */

static void promiseArgsTwo (SEXP el, SEXP rho, SEXP *a1, SEXP *a2)
{
    BEGIN_PROTECT6 (head1, tail1, head2, tail2, ev, h);

    head1 = head2 = R_NilValue;

    while (el != R_NilValue) {

        SEXP a = CAR(el);

	/* If we have a ... symbol, we look to see what it is bound to.
	   If its binding is R_NilValue we just ignore it.  If it is bound
           to a ... list of promises, we repromise all the promises and 
           then splice the list of resulting values into the return value.
	   Anything else bound to a ... symbol is an error. */

	if (a == R_DotsSymbol) {
	    h = findVar(a, rho);
            if (h == R_NilValue) {
                /* nothing */
            }
	    else if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    a = CAR(h);
                    if (TYPEOF(a) == PROMSXP) {
                        INC_NAMEDCNT(a);
                        INC_NAMEDCNT(a);
                        SEXP p = PRVALUE_PENDING_OK(a);
                        if (p != R_UnboundValue && NAMEDCNT_GT_0(p))
                            INC_NAMEDCNT(p);
                    }
                    else if (a != R_MissingArg && a != R_MissingUnder) {
                       a = mkPROMISE (a, rho);
                       INC_NAMEDCNT(a);
                    }
                    ev = cons_with_tag (a, R_NilValue, TAG(h));
                    if (head1==R_NilValue)
                        head1 = ev;
                    else
                        SETCDR(tail1,ev);
                    tail1 = ev;
                    ev = cons_with_tag (a, R_NilValue, TAG(h));
                    if (head2==R_NilValue)
                        head2 = ev;
                    else
                        SETCDR(tail2,ev);
                    tail2 = ev;
		    h = CDR(h);
		}
	    }
	    else if (h != R_MissingArg)
		dotdotdot_error();
	}
        else {
            if (TYPEOF(a) == PROMSXP) {
                INC_NAMEDCNT(a);
                INC_NAMEDCNT(a);
                SEXP p = PRVALUE_PENDING_OK(a);
                if (p != R_UnboundValue && NAMEDCNT_GT_0(p))
                    INC_NAMEDCNT(p);
            }
            else if (a != R_MissingArg && a != R_MissingUnder) {
               a = mkPROMISE (a, rho);
               INC_NAMEDCNT(a);
            }
            ev = cons_with_tag (a, R_NilValue, TAG(el));
            if (head1 == R_NilValue)
                head1 = ev;
            else
                SETCDR(tail1, ev);
            tail1 = ev;
            ev = cons_with_tag (a, R_NilValue, TAG(el));
            if (head2 == R_NilValue)
                head2 = ev;
            else
                SETCDR(tail2, ev);
            tail2 = ev;
        }
	el = CDR(el);
    }

    if (head1 != R_NilValue) {
        if (*a1 != R_NilValue)
            SETCDR(tail1,*a1);
        *a1 = head1;
        if (*a2 != R_NilValue)
            SETCDR(tail2,*a2);
        *a2 = head2;
    }

    END_PROTECT;
}

/*  Assignment in its various forms. */

SEXP Rf_set_subassign (SEXP call, SEXP lhs, SEXP rhs, SEXP rho, 
                       int variant, int opval);

static SEXP do_set (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP a;

    if (args==R_NilValue || (a = CDR(args)) == R_NilValue || CDR(a)!=R_NilValue)
        checkArity(op,args);

    SEXP lhs = CAR(args), rhs = CAR(a);
    int opval = PRIMVAL(op);

    /* Swap operands for -> and ->>. */

    if (opval >= 10) {
        rhs = lhs;
        lhs = CAR(a);
        opval -= 10;
    }

    /* Convert lhs string to a symbol. */

    if (TYPEOF(lhs) == STRSXP) {
        lhs = install(translateChar(STRING_ELT(lhs, 0)));
    }

    if (TYPEOF(lhs) == SYMSXP) {

        /* -- ASSIGNMENT TO A SIMPLE VARIABLE -- */

        /* Handle <<- without trying the optimizations done below. */

        if (opval == 2) {
            rhs = EVALV (rhs, rho, VARIANT_PENDING_OK);
            set_var_nonlocal (lhs, rhs, ENCLOS(rho), 3);
            goto done;
        }

        /* Handle assignment into base and user database environments without
           any special optimizations. */

        if (IS_BASE(rho) || IS_USER_DATABASE(rho)) {
            rhs = evalv (rhs, rho, VARIANT_PENDING_OK);
            set_var_in_frame (lhs, rhs, rho, TRUE, 3);
            goto done;
        }

        /* We decide whether we'll ask the right hand side evalutation to do
           the assignment, for statements like v<-exp(v), v<-v+1, or v<-2*v. */

        int local_assign = 0;

        if (TYPEOF(rhs) == LANGSXP) {
            if (CADR(rhs) == lhs) 
                local_assign = VARIANT_LOCAL_ASSIGN1;
            else if (CADDR(rhs) == lhs)
                local_assign = VARIANT_LOCAL_ASSIGN2;
        }

        /* Evaluate the right hand side, asking for it in a static box. */

        rhs = EVALV (rhs, rho, 
                     local_assign | VARIANT_PENDING_OK | VARIANT_STATIC_BOX_OK);

        /* See if the assignment was done by the rhs operator. */

        if (R_variant_result) {
            R_variant_result = 0;
            goto done;
        }

        /* Try to copy the value, not assign the object, if the rhs is scalar
           and doesn't have zero NAMEDCNT (for which assignment would be free). 
           This will copy from a static box, which must be replaced by a regular
           value if the copy can't be done.  If the copy can't be done, but 
           a binding cell was found here, the assignment is done directly into
           the binding cell, avoiding overhead of calling set_var_in_frame.

           Avoid accessing NAMEDCNT in a way that will cause unnecessary waits
           for task completion. */

        if (isVectorNonpointer(rhs) && LENGTH(rhs) == 1 && NAMEDCNT_GT_0(rhs)) {
            SEXPTYPE rhs_type = TYPEOF(rhs);
            SEXP v;
            if (SEXP32_FROM_SEXP(rho) != LASTSYMENV(lhs) 
                  || BINDING_IS_LOCKED((R_binding_cell = LASTSYMBINDING(lhs)))
                  || (v = CAR(R_binding_cell)) == R_UnboundValue)
                v = findVarInFrame3_nolast (rho, lhs, 7);
            if (v != R_UnboundValue  && TYPEOF(v) == rhs_type && LENGTH(v) == 1
                 && ATTRIB(v) == ATTRIB(rhs) && TRUELENGTH(v) == TRUELENGTH(rhs)
                 && LEVELS(v) == LEVELS(rhs) && !NAMEDCNT_GT_1(v)) {
                if (NAMEDCNT_EQ_0(v))
                    SET_NAMEDCNT_1(v);
                helpers_wait_until_not_in_use(v);
                WAIT_UNTIL_COMPUTED(v);
                switch (rhs_type) {
                case LGLSXP:  *LOGICAL(v) = *LOGICAL(rhs); break;
                case INTSXP:  *INTEGER(v) = *INTEGER(rhs); break;
                case REALSXP: *REAL(v)    = *REAL(rhs);    break;
                case CPLXSXP: *COMPLEX(v) = *COMPLEX(rhs); break;
                case RAWSXP:  *RAW(v)     = *RAW(rhs);     break;
                }
                rhs = v; /* for return value */
                goto done;
            }
            if (IS_STATIC_BOX(rhs)) 
                rhs = rhs==R_ScalarIntegerBox ? ScalarInteger(*INTEGER(rhs))
                                              : ScalarReal(*REAL(rhs));
            if (R_binding_cell != R_NilValue) {
                DEC_NAMEDCNT_AND_PRVALUE(v);
                SETCAR(R_binding_cell, rhs);
                SET_MISSING(R_binding_cell,0);
                INC_NAMEDCNT(rhs);
                if (rho == R_GlobalEnv) 
                    R_DirtyImage = 1;
                goto done;
            }
        }

        /* Assign rhs object to lhs symbol the usual way. */

        set_var_in_frame (lhs, rhs, rho, TRUE, 3);
    }

    else if (TYPEOF(lhs) == LANGSXP) {

        /* -- ASSIGNMENT TO A COMPLEX TARGET -- */

        rhs = Rf_set_subassign (call, lhs, rhs, rho, variant, opval);
    }

    else {
        errorcall (call, _("invalid assignment left-hand side"));
    }

  done:

    R_Visible = FALSE;

    if (variant & VARIANT_NULL)
        return R_NilValue;

    if ( ! (variant & VARIANT_PENDING_OK)) 
        WAIT_UNTIL_COMPUTED(rhs);
    
    return rhs;
}

/* Complex assignment.  Made a separate, non-static, function in order
   to avoid possible overhead of a large function (eg, stack frame size)
   for the simple case. */

SEXP attribute_hidden Rf_set_subassign (SEXP call, SEXP lhs, SEXP rhs, SEXP rho,
                                        int variant, int opval)
{
    SEXP var, varval, newval, rhsprom, lhsprom, e;

    /* Find the variable ultimately assigned to, and its depth.
       The depth is 1 for a variable within one replacement function
       (eg, in names(a) <- ...). */

    int depth = 1;
    for (var = CADR(lhs); TYPEOF(var) != SYMSXP; var = CADR(var)) {
        if (TYPEOF(var) != LANGSXP) {
            if (TYPEOF(var) == STRSXP && LENGTH(var) == 1) {
                var = install (CHAR (STRING_ELT(var,0)));
                break;
            }
            errorcall (call, _("invalid assignment left-hand side"));
        }
        depth += 1;
    }

    /* Find the assignment function symbol for the depth 1 assignment, and
       see if we maybe (tentatively) will be using the fast interface. */

    SEXP assgnfcn = installAssignFcnName(CAR(lhs));

    int maybe_fast = depth == 1 && (assgnfcn == R_SubAssignSymbol ||
                                    assgnfcn == R_DollarAssignSymbol ||
                                    assgnfcn == R_SubSubAssignSymbol);

    /* We evaluate the right hand side now, asking for it in a static 
       box if we (tentatively) will be using the fast interface (unless
       value needed for return), and otherwise for pending computation. */

    SEXP rhs_uneval = rhs;  /* save unevaluated rhs */
    R_static_box_contents rhs_contents;

    if (maybe_fast) {
        PROTECT(rhs = EVALV (rhs, rho, 
                        (variant & VARIANT_NULL) ? VARIANT_STATIC_BOX_OK : 0));
        if (IS_STATIC_BOX(rhs)) {
            /* save in case evaluation of varval uses the same static box */
            SAVE_STATIC_BOX_CONTENTS(rhs,&rhs_contents); 
        }
    }
    else
        PROTECT(rhs = EVALV (rhs, rho, VARIANT_PENDING_OK));

    /* Debugging/comparison aid:  Can be enabled one way or the other below,
       then activated by typing `switch to old` or `switch to new` at the
       prompt. */

#   if 0
        if (1 && installed_already("switch to new") == R_NoObect
         || 0 && installed_already("switch to old") != R_NoObject) {

            if ( ! (variant & VARIANT_NULL))
                INC_NAMEDCNT(rhs);

            applydefine (call, op, lhs, rhs, rho);

            if ( ! (variant & VARIANT_NULL))
                DEC_NAMEDCNT(rhs);
  
            UNPROTECT(1);
            goto done;
        }
#   endif

    /* Increment NAMEDCNT temporarily if rhs will be needed as the value,
       to protect it from being modified by the assignment, or otherwise. */

    if ( ! (variant & VARIANT_NULL))
        INC_NAMEDCNT(rhs);

    /* Get the value of the variable assigned to, and ensure it is local
       (unless this is the <<- operator).  Save and protect the binding 
       cell used. */

    if (opval == 2) /* <<- */
        varval = findVar (var, ENCLOS(rho));
    else {
        varval = findVarInFramePendingOK (rho, var);
        if (varval == R_UnboundValue && rho != R_EmptyEnv) {
            varval = findVar (var, ENCLOS(rho));
            if (varval != R_UnboundValue) {
                if (TYPEOF(varval) == PROMSXP)
                    varval = forcePromise(varval);
                set_var_in_frame (var, varval, rho, TRUE, 3);
            }
        }
    }
    if (NAMEDCNT_EQ_0(varval)) /* may sometime happen - should mean 1 */
        SET_NAMEDCNT_1(varval);

    SEXP bcell = R_binding_cell;
    PROTECT(bcell);

    if (TYPEOF(varval) == PROMSXP)
        varval = forcePromise(varval);
    if (varval == R_UnboundValue)
        unbound_var_error(var);
    if (varval == R_MissingArg)
        arg_missing_error(var);

    /* We might be able to avoid this duplication sometimes (eg, in
       a <- b <- integer(10); a[1] <- 0.5), except that some packages 
       (eg, Matrix 1.0-6) assume (in C code) that the object in a 
       replacement function is not shared. */

    if (NAMEDCNT_GT_1(varval))
        varval = dup_top_level(varval);

    PROTECT(varval);

    /* Special code for depth of 1.  This is purely an optimization - the
       general code below should also work when depth is 1. */

    if (depth == 1) {

        SEXP fn;

        if (IS_STATIC_BOX(rhs))
            RESTORE_STATIC_BOX_CONTENTS(rhs,&rhs_contents); 

        if (maybe_fast && !isObject(varval) && CADDR(lhs) != R_DotsSymbol
              && (fn = FINDFUN(assgnfcn,rho), 
                  TYPEOF(fn) == SPECIALSXP && PRIMFASTSUB(fn) && !RTRACE(fn))) {
            /* Use the fast interface.  No need to wait for rhs, since
               not evaluated with PENDING_OK */
            R_fast_sub_into = varval;
            R_fast_sub_value = rhs;
            newval = CALL_PRIMFUN (call, fn, CDDR(lhs), rho, 
                                   VARIANT_FAST_SUBASSIGN);
            UNPROTECT(3);
        }
        else {
            PROTECT(rhsprom = mkPROMISE(rhs_uneval, rho));
            SET_PRVALUE(rhsprom, IS_STATIC_BOX(rhs) ? duplicate(rhs) : rhs);
            PROTECT (lhsprom = mkPROMISE(CADR(lhs), rho));
            SET_PRVALUE (lhsprom, varval);
            PROTECT(e = replaceCall (assgnfcn, lhsprom, CDDR(lhs), rhsprom));
            newval = eval(e,rho);
            UNPROTECT(6);
        }
    }

    else {  /* the general case, for any depth */

        /* Structure recording information on expressions at all levels of 
           the lhs.  Level 'depth' is the ultimate variable; level 0 is the
           whole lhs expression. */

        struct { 
            SEXP fetch_args;      /* Arguments lists, sharing promises */
            SEXP store_args;
            SEXP value_arg;       /* Last cell in store_args, for value */
            SEXP expr;            /* Expression at this level */
            SEXP value;           /* Value of expr, may later change */
            int in_next;          /* 1 or 2 if value is unshared part */
        } s[depth+1];             /*   of value at next level, else 0 */

        SEXP v;
        int d;

        /* For each level from 1 to depth, store the lhs expression at that
           level.  For each level except the final variable and outermost 
           level, which only does a store, save argument lists for the 
           fetch/store functions that share promises, so that they are
           evaluated only once.  The store argument list has a "value"
           cell at the end to fill in the stored value. */

        s[0].expr = lhs;
        s[0].store_args = CDDR(lhs);  /* original args, no value cell */
        for (v = CADR(lhs), d = 1; d < depth; v = CADR(v), d++) {
            s[d].fetch_args = R_NilValue;
            PROTECT (s[d].value_arg = s[d].store_args =
                cons_with_tag (R_NilValue, R_NilValue, R_ValueSymbol));
            promiseArgsTwo (CDDR(v), rho, &s[d].fetch_args, 
                                          &s[d].store_args);
            UNPROTECT(1);
            PROTECT2 (s[d].fetch_args, s[d].store_args);
            s[d].expr = v;
        }
        s[depth].expr = var;

        /* Note: In code below, promises with the value already filled in
                 are used to 'quote' values passsed as arguments, so they 
                 will not be changed when the arguments are evaluated, and 
                 so deparsed error messages will have the source expression.
                 These promises should not be recycled, since they may be 
                 saved in warning messages stored for later display.  */

        /* For each level except the outermost, evaluate and save the value
           of the expression as it is before the assignment.  Also, ask if
           it is an unshared subset of the next larger expression.  If it
           is not known to be part of the next larger expression, we do a
           top-level duplicate of it, unless it has NAMEDCNT of 0. */

        s[depth].value = varval;

        for (d = depth-1; d > 0; d--) {

            SEXP prom = mkPROMISE(s[d+1].expr,rho);
            SET_PRVALUE(prom,s[d+1].value);

            /* We'll need this value for the subsequent replacement
               operation, so make sure it doesn't change.  Incrementing
               NAMEDCNT would be the obvious way, but if NAMEDCNT 
               was already non-zero, that leads to undesirable duplication
               later (even if the increment is later undone).  Making sure
               that NAMEDCNT isn't zero seems to be sufficient. */

            if (NAMEDCNT_EQ_0(s[d+1].value)) 
                SET_NAMEDCNT_1(s[d+1].value);

            e = LCONS (CAR(s[d].expr), CONS (prom, s[d].fetch_args));
            PROTECT(e);
            e = evalv (e, rho, VARIANT_QUERY_UNSHARED_SUBSET);
            UNPROTECT(1);
            s[d].in_next = R_variant_result;  /* 0, 1, or 2 */

            if (R_variant_result == 0 && NAMEDCNT_GT_0(e)) 
                e = dup_top_level(e);
            R_variant_result = 0;

            s[d].value = e;
            PROTECT(e);
        }

        /* Call the replacement functions at levels 1 to depth, changing the
           values at each level, using the fetched value at that level 
           (was perhaps duplicated), and the new value after replacement at 
           the lower level.  Except we don't do that if it's not necessary
           because the new value is already part of the larger object.
           The new value at the outermost level is the rhs value. */
        
        PROTECT(rhsprom = mkPROMISE(rhs_uneval, rho));
        SET_PRVALUE(rhsprom, rhs);
        s[0].in_next = 0;

        for (d = 1; ; d++) {

            if (s[d-1].in_next == 1) { /* don't need to do replacement */
                newval = s[d].value;
                UNPROTECT(1);  /* s[d].value protected in previous loop */
            }

            else {

                PROTECT (lhsprom = mkPROMISE(s[d].expr, rho));
                SET_PRVALUE (lhsprom, s[d].value);
                if (d == 1) {
                    /* original args, no value cell at end, assgnfcn set above*/
                    PROTECT(e = replaceCall (assgnfcn, lhsprom, 
                                             s[d-1].store_args, rhsprom));
                }
                else { 
                    assgnfcn = installAssignFcnName(CAR(s[d-1].expr));
                    SETCAR (s[d-1].value_arg, rhsprom);
                    PROTECT(e = LCONS (assgnfcn, CONS (lhsprom,
                                                   s[d-1].store_args)));
                }

                newval = eval(e,rho);

                /* Unprotect e, lhsprom, rhsprom, and s[d].value from the
                   previous loop, which went from depth-1 to 1 in the 
                   opposite order as this one (plus unprotect one more from
                   before that).  Note: e used below; no alloc before. */

                UNPROTECT(4);
            }

            /* See if we're done, with the final value in newval. */

            if (d == depth) break;

            /* If the replacement function returned a different object, 
               that new object won't be part of the object at the next
               level, even if the old one was. */

            if (s[d].value != newval)
                s[d].in_next = 0;

            /* Create a rhs promise if this value needs to be put into
               the next-higher object. */

            if (s[d].in_next != 1) {
                PROTECT(newval);
                rhsprom = mkPROMISE (e, rho);
                SET_PRVALUE (rhsprom, newval);
                UNPROTECT(1);
                PROTECT(rhsprom);
            }
        }

        UNPROTECT(2*(depth-1)+2);  /* fetch_args, store_args + two more */
    }

    /* Assign the final result after the top level replacement.  We
       can sometimes avoid the cost of this by looking at the saved
       binding cell, if we have one. */

    if (bcell != R_NilValue && CAR(bcell) == newval) {
        SET_MISSING(bcell,0);
        /* The replacement function might have changed NAMEDCNT to 0. */
        if (NAMEDCNT_EQ_0(varval))
            SET_NAMEDCNT_1(varval);
    }
    else {
        if (opval == 2) /* <<- */
            set_var_nonlocal (var, newval, ENCLOS(rho), 3);
        else
            set_var_in_frame (var, newval, rho, TRUE, 3);
    }

    if ( ! (variant & VARIANT_NULL))
        DEC_NAMEDCNT(rhs);

    return rhs;
}


/* Evaluate each expression in "el" in the environment "rho".  
   The evaluation is done by calling evalv with the given variant.

   The MISSING gp field in the CONS cell for a missing argument is 
   set to the result of R_isMissing, which will allow identification 
   of missing arguments resulting from '_'.

   Used in eval and applyMethod (object.c) for builtin primitives,
   do_internal (names.c) for builtin .Internals and in evalArgs.
 */

SEXP attribute_hidden evalList_v(SEXP el, SEXP rho, int variant)
{
    BEGIN_PROTECT4 (head, tail, ev, h);

    head = R_NilValue;

    while (el != R_NilValue) {

	if (CAR(el) == R_DotsSymbol) {
            /* If we have a ... symbol, we look to see what it is bound to.
               If its binding is Null (i.e. zero length) or missing we just
               ignore it and return the cdr with all its expressions evaluated.
               If it is bound to a ... list of promises, we force all the 
               promises and then splice the list of resulting values into
               the return value. Anything else bound to a ... symbol is an 
               error. */
	    h = findVar(CAR(el), rho);
	    if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    ev = cons_with_tag (EVALV (CAR(h), rho, variant),
                                        R_NilValue, TAG(h));
                    if (head==R_NilValue)
                        head = ev;
                    else
                        SETCDR(tail, ev);
                    tail = ev;
                    if (CAR(ev) == R_MissingArg && isSymbol(CAR(h)))
                        SET_MISSING (ev, R_isMissing(CAR(h),rho));
		    h = CDR(h);
		}
	    }
	    else if (h != R_NilValue && h != R_MissingArg)
		dotdotdot_error();

	} else {
            ev = cons_with_tag(EVALV(CAR(el),rho,variant), R_NilValue, TAG(el));
            if (head==R_NilValue)
                head = ev;
            else
                SETCDR(tail, ev);
            tail = ev;
            if (CAR(ev) == R_MissingArg && isSymbol(CAR(el)))
                SET_MISSING (ev, R_isMissing(CAR(el),rho));
	}

	el = CDR(el);
    }

    RETURN_SEXP_INSIDE_PROTECT (head);
    END_PROTECT;

} /* evalList_v */


/* evalListUnshared evaluates each expression in "el" in the
   environment "rho", ensuring that the values of variables evaluated
   are unshared, if they are atomic scalars without attributes, by
   assigning a duplicate to them if necessary.

   Used in .External and .Call as a defensive measure against argument 
   abuse.  Waits for arguments to be computed.  Does not allow missing 
   arguments. */

static inline SEXP eval_unshared (SEXP e, SEXP rho, int variant)
{
    SEXP res;

    if (!isSymbol(e) || e == R_DotsSymbol || DDVAL(e)) {
        res = evalv (e, rho, variant);
    }
    else {

        res = findVarPendingOK (e, rho);

        if (res == R_UnboundValue)
            unbound_var_error(e);
        else if (res == R_MissingArg) {
            if ( ! (variant & VARIANT_MISSING_OK))
                if (!DDVAL(e))  /* revert bug fix for the moment */
                    arg_missing_error(e);
        }
        else if (TYPEOF(res) == PROMSXP) {
            if (PRVALUE_PENDING_OK(res) == R_UnboundValue)
                res = forcePromiseUnbound(res,VARIANT_PENDING_OK);
            else
                res = PRVALUE_PENDING_OK(res);
        }
        else {
            if (NAMEDCNT_GT_1(res) && R_binding_cell != R_NilValue
              && isVectorAtomic(res) && LENGTH(res) == 1 && !HAS_ATTRIB(res)) {
                if (0) { /* Enable for debugging */
                    if (installed_already("UNSHARED.DEBUG") != R_NoObject)
                        Rprintf("Making %s unshared\n",CHAR(PRINTNAME(e)));
                }
                res = duplicate(res);
                SETCAR (R_binding_cell, res);
                /* DON'T clear MISSING, though may not get here if it matters */
            }
            if (NAMEDCNT_EQ_0(res))
                SET_NAMEDCNT_1(res);
        }
    }

    return res;
}

SEXP attribute_hidden evalListUnshared(SEXP el, SEXP rho)
{
    BEGIN_PROTECT4 (head, tail, ev, h);

    int variant = VARIANT_PENDING_OK;

    head = R_NilValue;

    while (el != R_NilValue) {

        if (CDR(el) == R_NilValue)
            variant = 0;  /* would need to wait for last immediately anyway */

	if (CAR(el) == R_DotsSymbol) {
            /* If we have a ... symbol, we look to see what it is bound to.
               If its binding is Null (i.e. zero length) or missing we just
               ignore it and return the cdr with all its expressions evaluated.
               If it is bound to a ... list of promises, we force all the 
               promises and then splice the list of resulting values into
               the return value. Anything else bound to a ... symbol is an 
               error. */
	    h = findVar(CAR(el), rho);
	    if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    ev = cons_with_tag (eval_unshared (CAR(h), rho, variant),
                                        R_NilValue, TAG(h));
                    if (head==R_NilValue)
                        head = ev;
                    else
                        SETCDR(tail, ev);
                    tail = ev;
                    if (CAR(ev) == R_MissingArg && isSymbol(CAR(h)))
                        SET_MISSING (ev, R_isMissing(CAR(h),rho));
		    h = CDR(h);
		}
	    }
	    else if (h != R_NilValue && h != R_MissingArg)
		dotdotdot_error();

	} else {
            ev = cons_with_tag (eval_unshared (CAR(el), rho, variant), 
                                R_NilValue, TAG(el));
            if (head==R_NilValue)
                head = ev;
            else
                SETCDR(tail, ev);
            tail = ev;
            if (CAR(ev) == R_MissingArg && isSymbol(CAR(el)))
                SET_MISSING (ev, R_isMissing(CAR(el),rho));
	}

	el = CDR(el);
    }

    WAIT_UNTIL_ARGUMENTS_COMPUTED (head);
    RETURN_SEXP_INSIDE_PROTECT (head);
    END_PROTECT;

} /* evalListUnshared() */

/* Evaluate argument list, waiting for any pending computations of arguments. */

SEXP attribute_hidden evalList(SEXP el, SEXP rho)
{
    return evalList_v (el, rho, 0);
}

/* Evaluate argument list, waiting for pending computations, and with no 
   error for missing arguments. */

SEXP attribute_hidden evalListKeepMissing(SEXP el, SEXP rho)
{ 
    return evalList_v (el, rho, VARIANT_MISSING_OK);
}


/* Create a promise to evaluate each argument.	If the argument is itself
   a promise, it is used unchanged, except that it has its NAMEDCNT
   incremented, and the NAMEDCNT of its value (if not unbound) incremented
   unless it is zero.  See inside for handling of ... */

SEXP attribute_hidden promiseArgs(SEXP el, SEXP rho)
{
    BEGIN_PROTECT4 (head, tail, ev, h);

    head = R_NilValue;

    while (el != R_NilValue) {

        SEXP a = CAR(el);

	/* If we have a ... symbol, we look to see what it is bound to.
	   If its binding is R_NilValue we just ignore it.  If it is bound
           to a ... list of promises, we repromise all the promises and 
           then splice the list of resulting values into the return value.
	   Anything else bound to a ... symbol is an error. */

	if (a == R_DotsSymbol) {
	    h = findVar(a, rho);
            if (h == R_NilValue) {
                /* nothing */
            }
	    else if (TYPEOF(h) == DOTSXP) {
		while (h != R_NilValue) {
                    a = CAR(h);
                    if (TYPEOF(a) == PROMSXP) {
                        INC_NAMEDCNT(a);
                        SEXP p = PRVALUE_PENDING_OK(a);
                        if (p != R_UnboundValue && NAMEDCNT_GT_0(p))
                            INC_NAMEDCNT(p);
                    }
                    else if (a != R_MissingArg && a != R_MissingUnder)
                        a = mkPROMISE (a, rho);
                    ev = cons_with_tag (a, R_NilValue, TAG(h));
                    if (head==R_NilValue)
                        head = ev;
                    else
                        SETCDR(tail,ev);
                    tail = ev;
		    h = CDR(h);
		}
	    }
	    else if (h != R_MissingArg)
		dotdotdot_error();
	}
        else {
            if (TYPEOF(a) == PROMSXP) {
                INC_NAMEDCNT(a);
                SEXP p = PRVALUE_PENDING_OK(a);
                if (p != R_UnboundValue && NAMEDCNT_GT_0(p))
                    INC_NAMEDCNT(p);
            }
            else if (a != R_MissingArg && a != R_MissingUnder)
               a = mkPROMISE (a, rho);
            ev = cons_with_tag (a, R_NilValue, TAG(el));
            if (head == R_NilValue)
                head = ev;
            else
                SETCDR(tail, ev);
            tail = ev;
        }
	el = CDR(el);
    }

    RETURN_SEXP_INSIDE_PROTECT (head);
    END_PROTECT;
}
 
/* Create promises for arguments, with values for promises filled in.  
   Values for arguments that don't become promises are silently ignored.  
   This is used in method dispatch, hence the text of the error message 
   (which should never occur). */
 
SEXP attribute_hidden promiseArgsWithValues(SEXP el, SEXP rho, SEXP values)
{
    SEXP s, a, b;
    PROTECT(s = promiseArgs(el, rho));
    if (length(s) != length(values)) error(_("dispatch error"));
    for (a = values, b = s; a != R_NilValue; a = CDR(a), b = CDR(b))
        if (TYPEOF(CAR(b)) == PROMSXP) {
            SET_PRVALUE(CAR(b), CAR(a));
            INC_NAMEDCNT(CAR(a));
        }
    UNPROTECT(1);
    return s;
}

/* Like promiseArgsWithValues except it sets only the first value. */

SEXP attribute_hidden promiseArgsWith1Value(SEXP el, SEXP rho, SEXP value)
{
    SEXP s;
    PROTECT(s = promiseArgs(el, rho));
    if (s == R_NilValue) error(_("dispatch error"));
    if (TYPEOF(CAR(s)) == PROMSXP) {
        SET_PRVALUE(CAR(s), value);
        INC_NAMEDCNT(value);
    }
    UNPROTECT(1);
    return s;
}


/* Check that each formal is a symbol */

/* used in coerce.c */
void attribute_hidden CheckFormals(SEXP ls)
{
    if (isList(ls)) {
	for (; ls != R_NilValue; ls = CDR(ls))
	    if (TYPEOF(TAG(ls)) != SYMSXP)
		goto err;
	return;
    }
 err:
    error(_("invalid formal argument list for \"function\""));
}


static SEXP VectorToPairListNamed(SEXP x)
{
    SEXP xptr, xnew, xnames;
    int i, len, len_x = length(x);

    PROTECT(x);
    PROTECT(xnames = getAttrib(x, R_NamesSymbol)); 
                       /* isn't this protected via x?  Or could be concocted? */

    len = 0;
    if (xnames != R_NilValue) {
	for (i = 0; i < len_x; i++)
	    if (CHAR(STRING_ELT(xnames,i))[0] != 0) len += 1;
    }

    PROTECT(xnew = allocList(len));

    if (len > 0) {
	xptr = xnew;
	for (i = 0; i < len_x; i++) {
	    if (CHAR(STRING_ELT(xnames,i))[0] != 0) {
		SETCAR (xptr, VECTOR_ELT(x,i));
		SET_TAG (xptr, install (translateChar (STRING_ELT(xnames,i))));
		xptr = CDR(xptr);
	    }
	}
    } 

    UNPROTECT(3);
    return xnew;
}

#define simple_as_environment(arg) (IS_S4_OBJECT(arg) && (TYPEOF(arg) == S4SXP) ? R_getS4DataSlot(arg, ENVSXP) : R_NilValue)

/* "eval" and "eval.with.vis" : Evaluate the first argument */
/* in the environment specified by the second argument. */

static SEXP do_eval (SEXP call, SEXP op, SEXP args, SEXP rho, int variant)
{
    SEXP encl, x, xptr;
    volatile SEXP expr, env, tmp;

    int frame;
    RCNTXT cntxt;

    checkArity(op, args);

    expr = CAR(args);
    env = CADR(args);
    encl = CADDR(args);
    if (isNull(encl)) {
	/* This is supposed to be defunct, but has been kept here
	   (and documented as such) */
	encl = R_BaseEnv;
    } else if ( !isEnvironment(encl) &&
		!isEnvironment((encl = simple_as_environment(encl))) )
	error(_("invalid '%s' argument"), "enclos");
    if(IS_S4_OBJECT(env) && (TYPEOF(env) == S4SXP))
	env = R_getS4DataSlot(env, ANYSXP); /* usually an ENVSXP */
    switch(TYPEOF(env)) {
    case NILSXP:
	env = encl;     /* so eval(expr, NULL, encl) works */
        break;
    case ENVSXP:
	break;
    case LISTSXP:
	/* This usage requires all the pairlist to be named */
	env = NewEnvironment(R_NilValue, duplicate(CADR(args)), encl);
	break;
    case VECSXP:
	/* PR#14035 */
	x = VectorToPairListNamed(CADR(args));
	for (xptr = x ; xptr != R_NilValue ; xptr = CDR(xptr))
	    SET_NAMEDCNT_MAX(CAR(xptr));
	env = NewEnvironment(R_NilValue, x, encl);
	break;
    case INTSXP:
    case REALSXP:
	if (length(env) != 1)
	    error(_("numeric 'envir' arg not of length one"));
	frame = asInteger(env);
	if (frame == NA_INTEGER)
	    error(_("invalid '%s' argument"), "envir");
	env = R_sysframe(frame, R_GlobalContext);
	break;
    default:
	error(_("invalid '%s' argument"), "envir");
    }

    PROTECT(env); /* may no longer be what was passed in arg */

    /* isLanguage includes NILSXP, and that does not need to be evaluated,
       so don't use isLanguage(expr) || isSymbol(expr) || isByteCode(expr) */
    if (TYPEOF(expr) == LANGSXP || TYPEOF(expr) == SYMSXP || isByteCode(expr)) {
	begincontext(&cntxt, CTXT_RETURN, call, env, rho, args, op);
	if (!SETJMP(cntxt.cjmpbuf))
	    expr = evalv (expr, env, VARIANT_PASS_ON(variant));
	else {
	    expr = R_ReturnedValue;
	    if (expr == R_RestartToken) {
		cntxt.callflag = CTXT_RETURN;  /* turn restart off */
		error(_("restarts not supported in 'eval'"));
	    }
            if ( ! (variant & VARIANT_PENDING_OK))
                WAIT_UNTIL_COMPUTED(R_ReturnedValue);
	}
	UNPROTECT(1);
	PROTECT(expr);
	endcontext(&cntxt);
    }
    else if (TYPEOF(expr) == EXPRSXP) {
	int i, n;
        int len;
        SEXP *srcrefs;
        getBlockSrcrefs(expr,&srcrefs,&len);
	n = LENGTH(expr);
	tmp = R_NilValue;
	begincontext(&cntxt, CTXT_RETURN, call, env, rho, args, op);
        SEXP savedsrcref = R_Srcref;
	if (!SETJMP(cntxt.cjmpbuf)) {
	    for (i = 0 ; i < n ; i++) {
                R_Srcref = getSrcref (srcrefs, len, i); 
		tmp = evalv (VECTOR_ELT(expr, i), env, 
                        i==n-1 ? VARIANT_PASS_ON(variant) 
                               : VARIANT_NULL | VARIANT_PENDING_OK);
            }
        }
	else {
	    tmp = R_ReturnedValue;
	    if (tmp == R_RestartToken) {
		cntxt.callflag = CTXT_RETURN;  /* turn restart off */
		error(_("restarts not supported in 'eval'"));
	    }
            if ( ! (variant & VARIANT_PENDING_OK))
                WAIT_UNTIL_COMPUTED(R_ReturnedValue);
	}
	UNPROTECT(1);
	PROTECT(tmp);
        R_Srcref = savedsrcref;
	endcontext(&cntxt);
	expr = tmp;
    }
    else if( TYPEOF(expr) == PROMSXP ) {
	expr = forcePromise(expr);
    } 
    else 
        ; /* expr is returned unchanged */

    if (PRIMVAL(op)) { /* eval.with.vis(*) : */
	PROTECT(expr);
	PROTECT(env = allocVector(VECSXP, 2));
	PROTECT(encl = allocVector(STRSXP, 2));
	SET_STRING_ELT(encl, 0, mkChar("value"));
	SET_STRING_ELT(encl, 1, mkChar("visible"));
	SET_VECTOR_ELT(env, 0, expr);
	SET_VECTOR_ELT(env, 1, ScalarLogicalMaybeConst(R_Visible));
	setAttrib(env, R_NamesSymbol, encl);
	expr = env;
	UNPROTECT(3);
    }

    UNPROTECT(1);
    return expr;
}

/* This is a special .Internal */
static SEXP do_withVisible(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP x, nm, ret;

    checkArity(op, args);
    x = CAR(args);
    x = eval(x, rho);
    PROTECT(x);
    PROTECT(ret = allocVector(VECSXP, 2));
    PROTECT(nm = allocVector(STRSXP, 2));
    SET_STRING_ELT(nm, 0, mkChar("value"));
    SET_STRING_ELT(nm, 1, mkChar("visible"));
    SET_VECTOR_ELT(ret, 0, x);
    SET_VECTOR_ELT(ret, 1, ScalarLogicalMaybeConst(R_Visible));
    setAttrib(ret, R_NamesSymbol, nm);
    UNPROTECT(3);
    return ret;
}

/* This is a special .Internal */
static SEXP do_recall(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    RCNTXT *cptr;
    SEXP s, ans ;
    cptr = R_GlobalContext;
    /* get the args supplied */
    while (cptr != NULL) {
	if (cptr->callflag == CTXT_RETURN && cptr->cloenv == rho)
	    break;
	cptr = cptr->nextcontext;
    }
    if (cptr != NULL) {
	args = cptr->promargs;
    }
    /* get the env recall was called from */
    s = R_GlobalContext->sysparent;
    while (cptr != NULL) {
	if (cptr->callflag == CTXT_RETURN && cptr->cloenv == s)
	    break;
	cptr = cptr->nextcontext;
    }
    if (cptr == NULL)
	error(_("'Recall' called from outside a closure"));

    /* If the function has been recorded in the context, use it
       otherwise search for it by name or evaluate the expression
       originally used to get it.
    */
    if (cptr->callfun != R_NilValue)
	PROTECT(s = cptr->callfun);
    else if( TYPEOF(CAR(cptr->call)) == SYMSXP)
	PROTECT(s = findFun(CAR(cptr->call), cptr->sysparent));
    else
	PROTECT(s = eval(CAR(cptr->call), cptr->sysparent));
    if (TYPEOF(s) != CLOSXP) 
    	error(_("'Recall' called from outside a closure"));
    ans = applyClosure_v(cptr->call, s, args, cptr->sysparent, R_NoObject, 0);
    UNPROTECT(1);
    return ans;
}


static SEXP evalArgs(SEXP el, SEXP rho, int dropmissing)
{
    return dropmissing ? evalList(el,rho) : evalListKeepMissing(el,rho);
}


/* A version of DispatchOrEval that checks for possible S4 methods for
 * any argument, not just the first.  Used in the code for `[` in
 * do_subset.  Differs in that all arguments are evaluated
 * immediately, rather than after the call to R_possible_dispatch.
 * NOT ACTUALLY USED AT PRESENT.
 */
attribute_hidden
int DispatchAnyOrEval(SEXP call, SEXP op, const char *generic, SEXP args,
		      SEXP rho, SEXP *ans, int dropmissing, int argsevald)
{
    if(R_has_methods(op)) {
        SEXP argValue, el,  value; 
	/* Rboolean hasS4 = FALSE; */ 
	int nprotect = 0, dispatch;
	if(!argsevald) {
            PROTECT(argValue = evalArgs(args, rho, dropmissing));
	    nprotect++;
	    argsevald = TRUE;
	}
	else argValue = args;
	for(el = argValue; el != R_NilValue; el = CDR(el)) {
	    if(IS_S4_OBJECT(CAR(el))) {
	        value = R_possible_dispatch(call, op, argValue, rho, TRUE);
	        if (value != R_NoObject) {
		    *ans = value;
		    UNPROTECT(nprotect);
		    return 1;
	        }
		else break;
	    }
	}
	 /* else, use the regular DispatchOrEval, but now with evaluated args */
	dispatch = DispatchOrEval(call, op, generic, argValue, rho, ans, dropmissing, argsevald);
	UNPROTECT(nprotect);
	return dispatch;
    }
    return DispatchOrEval(call, op, generic, args, rho, ans, dropmissing, argsevald);
}


/* DispatchOrEval is used in internal functions which dispatch to
 * object methods (e.g. "[" or "[[").  The code either builds promises
 * and dispatches to the appropriate method, or it evaluates the
 * arguments it comes in with (if argsevald is 0) and returns them so that
 * the generic built-in C code can continue.  Note that CDR(call) is
 * used to obtain the unevaluated arguments when creating promises, even
 * when argsevald is 1 (so args is the evaluated arguments).  If argsevald 
 * is -1, only the first argument will have been evaluated.
 *
 * The arg list is protected by this function, and needn't be by the caller.
 */
attribute_hidden
int DispatchOrEval(SEXP call, SEXP op, const char *generic, SEXP args,
		   SEXP rho, SEXP *ans, int dropmissing, int argsevald)
{
/* DispatchOrEval is called very frequently, most often in cases where
   no dispatching is needed and the isObject or the string-based
   pre-test fail.  To avoid degrading performance it is therefore
   necessary to avoid creating promises in these cases.  The pre-test
   does require that we look at the first argument, so that needs to
   be evaluated.  The complicating factor is that the first argument
   might come in with a "..." and that there might be other arguments
   in the "..." as well.  LT */

    BEGIN_PROTECT1 (x);
    ALSO_PROTECT1 (args);

    int dots = FALSE;

    if (argsevald != 0)
	x = CAR(args);
    else {
	/* Find the object to dispatch on, dropping any leading
	   ... arguments with missing or empty values.  If there are no
	   arguments, R_NilValue is used. */
        x = R_NilValue;
	for (; args != R_NilValue; args = CDR(args)) {
	    if (CAR(args) == R_DotsSymbol) {
		SEXP h = findVar(R_DotsSymbol, rho);
		if (TYPEOF(h) == DOTSXP) {
#ifdef DODO
		    /**** any self-evaluating value should be OK; this
			  is used in byte compiled code. LT */
		    /* just a consistency check */
		    if (TYPEOF(CAR(h)) != PROMSXP)
			error(_("value in '...' is not a promise"));
#endif
		    dots = TRUE;
		    x = eval(CAR(h), rho);
                    break;
		}
		else if (h != R_NilValue && h != R_MissingArg)
		    dotdotdot_error();
	    }
	    else {
                dots = FALSE;
                x = eval(CAR(args), rho);
                break;
	    }
	}
    }

    if (isObject(x)) { /* try to dispatch on the object */
	char *pt;
	/* Try for formal method. */
	if(IS_S4_OBJECT(x) && R_has_methods(op)) {

	    BEGIN_INNER_PROTECT2 (value, argValue);

	    /* create a promise to pass down to applyClosure  */
	    if (argsevald < 0)
                argValue = promiseArgsWith1Value(CDR(call), rho, x);
            else if (argsevald == 0)
		argValue = promiseArgsWith1Value(args, rho, x);
	    else 
                argValue = args;
	    /* This means S4 dispatch */
	    value = R_possible_dispatch (call, op, argValue, rho, argsevald<=0);
	    if (value != R_NoObject) {
		*ans = value;
		RETURN_OUTSIDE_PROTECT (1);
	    }
	    else {
		/* go on, with the evaluated args.  Not guaranteed to have
		   the same semantics as if the arguments were not
		   evaluated, in special cases (e.g., arg values that are
		   LANGSXP).
		   The use of the promiseArgs is supposed to prevent
		   multiple evaluation after the call to possible_dispatch.
		*/
		if (dots)
		    argValue = evalArgs(argValue, rho, dropmissing);
		else {
		    argValue = CONS(x, evalArgs(CDR(argValue),rho,dropmissing));
		    SET_TAG(argValue, CreateTag(TAG(args)));
		}
		args = argValue; 
		argsevald = 1;
	    }

            END_INNER_PROTECT;
	}
	if (TYPEOF(CAR(call)) == SYMSXP)
	    pt = Rf_strrchr(CHAR(PRINTNAME(CAR(call))), '.');
	else
	    pt = NULL;

	if (pt == NULL || strcmp(pt,".default")) {

	    BEGIN_INNER_PROTECT2 (pargs, rho1);
	    RCNTXT cntxt;

            if (argsevald > 0) {  /* handle as in R_possible_dispatch */
                pargs = promiseArgsWithValues(CDR(call), rho, args);
            }
            else
                pargs = promiseArgsWith1Value(args, rho, x); 

	    /* The context set up here is needed because of the way
	       usemethod() is written.  DispatchGroup() repeats some
	       internal usemethod() code and avoids the need for a
	       context; perhaps the usemethod() code should be
	       refactored so the contexts around the usemethod() calls
	       in this file can be removed.

	       Using rho for current and calling environment can be
	       confusing for things like sys.parent() calls captured
	       in promises (Gabor G had an example of this).  Also,
	       since the context is established without a SETJMP using
	       an R-accessible environment allows a segfault to be
	       triggered (by something very obscure, but still).
	       Hence here and in the other usemethod() uses below a
	       new environment rho1 is created and used.  LT */
	    rho1 = NewEnvironment(R_NilValue, R_NilValue, rho);
	    begincontext(&cntxt, CTXT_RETURN, call, rho1, rho, pargs, op);
	    if(usemethod(generic, x, call, pargs, rho1, rho, R_BaseEnv, 0, ans))
	    {   endcontext(&cntxt);
		RETURN_OUTSIDE_PROTECT (1);
	    }
	    endcontext(&cntxt);

            END_INNER_PROTECT;
	}
    }

    if (argsevald <= 0) {
	if (dots)
	    /* The first call argument was ... and may contain more than the
	       object, so it needs to be evaluated here.  The object should be
	       in a promise, so evaluating it again should be no problem. */
	    args = evalArgs(args, rho, dropmissing);
	else {
	    args = cons_with_tag (x, evalArgs(CDR(args), rho, dropmissing),
                                  TAG(args));
	}
    }

    *ans = args;
    END_PROTECT;
    return 0;
}


/* gr needs to be protected on return from this function. */
static void findmethod(SEXP Class, const char *group, const char *generic,
		       SEXP *sxp,  SEXP *gr, SEXP *meth, int *which,
		       SEXP rho)
{
    int len, whichclass;
    char buf[512];

    len = length(Class);

    /* Need to interleave looking for group and generic methods
       e.g. if class(x) is c("foo", "bar)" then x > 3 should invoke
       "Ops.foo" rather than ">.bar"
    */
    for (whichclass = 0 ; whichclass < len ; whichclass++) {
	const char *ss = translateChar(STRING_ELT(Class, whichclass));
	if (!copy_3_strings (buf, sizeof buf, generic, ".", ss))
	    error(_("class name too long in '%s'"), generic);
	*meth = install(buf);
	*sxp = R_LookupMethod(*meth, rho, rho, R_BaseEnv);
	if (isFunction(*sxp)) {
	    *gr = R_BlankScalarString;
	    break;
	}
        if (!copy_3_strings (buf, sizeof buf, group, ".", ss))
	    error(_("class name too long in '%s'"), group);
	*meth = install(buf);
	*sxp = R_LookupMethod(*meth, rho, rho, R_BaseEnv);
	if (isFunction(*sxp)) {
	    *gr = mkString(group);
	    break;
	}
    }
    *which = whichclass;
}

attribute_hidden
int DispatchGroup(const char* group, SEXP call, SEXP op, SEXP args, SEXP rho,
		  SEXP *ans)
{
    int i, j, nargs, lwhich, rwhich, set;
    SEXP lclass, s, t, m, lmeth, lsxp, lgr, newrho;
    SEXP rclass, rmeth, rgr, rsxp, value;
    char *generic;
    Rboolean useS4 = TRUE, isOps = FALSE;

    /* pre-test to avoid string computations when there is nothing to
       dispatch on because either there is only one argument and it
       isn't an object or there are two or more arguments but neither
       of the first two is an object -- both of these cases would be
       rejected by the code following the string examination code
       below */
    if (args != R_NilValue && ! isObject(CAR(args)) &&
	(CDR(args) == R_NilValue || ! isObject(CADR(args))))
	return 0;

    isOps = strcmp(group, "Ops") == 0;

    /* try for formal method */
    if(length(args) == 1 && !IS_S4_OBJECT(CAR(args))) useS4 = FALSE;
    if(length(args) == 2 &&
       !IS_S4_OBJECT(CAR(args)) && !IS_S4_OBJECT(CADR(args))) useS4 = FALSE;
    if(useS4) {
	/* Remove argument names to ensure positional matching */
	if(isOps)
	    for(s = args; s != R_NilValue; s = CDR(s)) SET_TAG(s, R_NilValue);
	if(R_has_methods(op)) {
	    value = R_possible_dispatch(call, op, args, rho, FALSE);
            if (value != R_NoObject) {
	        *ans = value;
	        return 1;
            }
	}
	/* else go on to look for S3 methods */
    }

    /* check whether we are processing the default method */
    if ( isSymbol(CAR(call)) ) {
        const char *pt;
        pt = CHAR(PRINTNAME(CAR(call)));
        while (*pt == '.') pt += 1;   /* duplicate previous behaviour exactly */
        while (*pt != 0 && *pt != '.') pt += 1;
        if (*pt != 0) {
            while (*pt == '.') pt += 1;
            if (strcmp(pt,"default") == 0)
                return 0;
        }
    }

    if(isOps)
	nargs = length(args);
    else
	nargs = 1;

    if( nargs == 1 && !isObject(CAR(args)) )
	return 0;

    if(!isObject(CAR(args)) && !isObject(CADR(args)))
	return 0;

    generic = PRIMNAME(op);

    lclass = IS_S4_OBJECT(CAR(args)) ? R_data_class2(CAR(args))
              : getAttrib00(CAR(args), R_ClassSymbol);
    PROTECT(lclass);

    if( nargs == 2 )
	rclass = IS_S4_OBJECT(CADR(args)) ? R_data_class2(CADR(args))
                  : getAttrib00(CADR(args), R_ClassSymbol);
    else
	rclass = R_NilValue;
    PROTECT(rclass);

    lsxp = R_NilValue; lgr = R_NilValue; lmeth = R_NilValue;
    rsxp = R_NilValue; rgr = R_NilValue; rmeth = R_NilValue;

    findmethod(lclass, group, generic, &lsxp, &lgr, &lmeth, &lwhich, rho);
    PROTECT(lgr);
    if(isFunction(lsxp) && IS_S4_OBJECT(CAR(args)) && lwhich > 0
       && isBasicClass(translateChar(STRING_ELT(lclass, lwhich)))) {
	/* This and the similar test below implement the strategy
	 for S3 methods selected for S4 objects.  See ?Methods */
        value = CAR(args);
	if (NAMEDCNT_GT_0(value)) SET_NAMEDCNT_MAX(value);
	value = R_getS4DataSlot(value, S4SXP); /* the .S3Class obj. or NULL*/
	if(value != R_NilValue) /* use the S3Part as the inherited object */
	    SETCAR(args, value);
    }

    if( nargs == 2 )
	findmethod(rclass, group, generic, &rsxp, &rgr, &rmeth, &rwhich, rho);
    else
	rwhich = 0;

    if(isFunction(rsxp) && IS_S4_OBJECT(CADR(args)) && rwhich > 0
       && isBasicClass(translateChar(STRING_ELT(rclass, rwhich)))) {
        value = CADR(args);
	if (NAMEDCNT_GT_0(value)) SET_NAMEDCNT_MAX(value);
	value = R_getS4DataSlot(value, S4SXP);
	if(value != R_NilValue) SETCADR(args, value);
    }

    PROTECT(rgr);

    if( !isFunction(lsxp) && !isFunction(rsxp) ) {
	UNPROTECT(4);
	return 0; /* no generic or group method so use default*/
    }

    if( lsxp != rsxp ) {
	if ( isFunction(lsxp) && isFunction(rsxp) ) {
	    /* special-case some methods involving difftime */
	    const char *lname = CHAR(PRINTNAME(lmeth)),
		*rname = CHAR(PRINTNAME(rmeth));
	    if( streql(rname, "Ops.difftime") && 
		(streql(lname, "+.POSIXt") || streql(lname, "-.POSIXt") ||
		 streql(lname, "+.Date") || streql(lname, "-.Date")) )
		rsxp = R_NilValue;
	    else if (streql(lname, "Ops.difftime") && 
		     (streql(rname, "+.POSIXt") || streql(rname, "+.Date")) )
		lsxp = R_NilValue;
	    else {
		warning(_("Incompatible methods (\"%s\", \"%s\") for \"%s\""),
			lname, rname, generic);
		UNPROTECT(4);
		return 0;
	    }
	}
	/* if the right hand side is the one */
	if( !isFunction(lsxp) ) { /* copy over the righthand stuff */
	    lsxp = rsxp;
	    lmeth = rmeth;
	    lgr = rgr;
	    lclass = rclass;
	    lwhich = rwhich;
	}
    }

    /* we either have a group method or a class method */

    PROTECT(newrho = allocSExp(ENVSXP));
    PROTECT(m = allocVector(STRSXP,nargs));
    s = args;
    for (i = 0 ; i < nargs ; i++) {
	t = IS_S4_OBJECT(CAR(s)) ? R_data_class2(CAR(s))
	  : getAttrib00(CAR(s), R_ClassSymbol);
	set = 0;
	if (isString(t)) {
	    for (j = 0 ; j < LENGTH(t) ; j++) {
		if (!strcmp(translateChar(STRING_ELT(t, j)),
			    translateChar(STRING_ELT(lclass, lwhich)))) {
		    SET_STRING_ELT(m, i, PRINTNAME(lmeth));
		    set = 1;
		    break;
		}
	    }
	}
	if( !set )
	    SET_STRING_ELT(m, i, R_BlankString);
	s = CDR(s);
    }

    defineVar(R_dot_Method, m, newrho);
    UNPROTECT(1);
    PROTECT(t = mkString(generic));
    defineVar(R_dot_Generic, t, newrho);
    UNPROTECT(1);
    defineVar(R_dot_Group, lgr, newrho);
    set = length(lclass) - lwhich;
    t = allocVector(STRSXP, set);
    copy_string_elements (t, 0, lclass, lwhich, set);
    defineVar(R_dot_Class, t, newrho);
    defineVar(R_dot_GenericCallEnv, rho, newrho);
    defineVar(R_dot_GenericDefEnv, R_BaseEnv, newrho);

    PROTECT(t = LCONS(lmeth, CDR(call)));

    /* the arguments have been evaluated; since we are passing them */
    /* out to a closure we need to wrap them in promises so that */
    /* they get duplicated and things like missing/substitute work. */

    PROTECT(s = promiseArgsWithValues(CDR(call), rho, args));
    if (isOps) {
        /* ensure positional matching for operators */
        for (m = s; m != R_NilValue; m = CDR(m))
            SET_TAG(m, R_NilValue);
    }

    *ans = applyClosure_v(t, lsxp, s, rho, newrho, 0);
    UNPROTECT(7);
    return 1;
}


/* START OF BYTECODE SECTION. */

static int R_bcVersion = 7;
static int R_bcMinVersion = 6;

static SEXP R_SqrtSymbol = R_NoObject;
static SEXP R_ExpSymbol = R_NoObject;
static SEXP R_CSymbol = R_NoObject;

static SEXP R_TrueValue = R_NoObject;
static SEXP R_FalseValue = R_NoObject;

#if defined(__GNUC__) && ! defined(BC_PROFILING) && (! defined(NO_THREADED_CODE))
# define THREADED_CODE
#endif

attribute_hidden
void R_initialize_bcode(void)
{
  R_SqrtSymbol = install("sqrt");
  R_ExpSymbol = install("exp");
  R_CSymbol = install("c");

  R_TrueValue = mkTrue();
  SET_NAMEDCNT_MAX(R_TrueValue);
  R_PreserveObject(R_TrueValue);
  R_FalseValue = mkFalse();
  SET_NAMEDCNT_MAX(R_FalseValue);
  R_PreserveObject(R_FalseValue);
#ifdef THREADED_CODE
  bcEval(R_NoObject, R_NoObject, FALSE);
#endif
}

enum {
  BCMISMATCH_OP,
  RETURN_OP,
  GOTO_OP,
  BRIFNOT_OP,
  POP_OP,
  DUP_OP,
  PRINTVALUE_OP,
  STARTLOOPCNTXT_OP,
  ENDLOOPCNTXT_OP,
  DOLOOPNEXT_OP,
  DOLOOPBREAK_OP,
  STARTFOR_OP,
  STEPFOR_OP,
  ENDFOR_OP,
  SETLOOPVAL_OP,
  INVISIBLE_OP,
  LDCONST_OP,
  LDNULL_OP,
  LDTRUE_OP,
  LDFALSE_OP,
  GETVAR_OP,
  DDVAL_OP,
  SETVAR_OP,
  GETFUN_OP,
  GETGLOBFUN_OP,
  GETSYMFUN_OP,
  GETBUILTIN_OP,
  GETINTLBUILTIN_OP,
  CHECKFUN_OP,
  MAKEPROM_OP,
  DOMISSING_OP,
  SETTAG_OP,
  DODOTS_OP,
  PUSHARG_OP,
  PUSHCONSTARG_OP,
  PUSHNULLARG_OP,
  PUSHTRUEARG_OP,
  PUSHFALSEARG_OP,
  CALL_OP,
  CALLBUILTIN_OP,
  CALLSPECIAL_OP,
  MAKECLOSURE_OP,
  UMINUS_OP,
  UPLUS_OP,
  ADD_OP,
  SUB_OP,
  MUL_OP,
  DIV_OP,
  EXPT_OP,
  SQRT_OP,
  EXP_OP,
  EQ_OP,
  NE_OP,
  LT_OP,
  LE_OP,
  GE_OP,
  GT_OP,
  AND_OP,
  OR_OP,
  NOT_OP,
  DOTSERR_OP,
  STARTASSIGN_OP,
  ENDASSIGN_OP,
  STARTSUBSET_OP,
  DFLTSUBSET_OP,
  STARTSUBASSIGN_OP,
  DFLTSUBASSIGN_OP,
  STARTC_OP,
  DFLTC_OP,
  STARTSUBSET2_OP,
  DFLTSUBSET2_OP,
  STARTSUBASSIGN2_OP,
  DFLTSUBASSIGN2_OP,
  DOLLAR_OP,
  DOLLARGETS_OP,
  ISNULL_OP,
  ISLOGICAL_OP,
  ISINTEGER_OP,
  ISDOUBLE_OP,
  ISCOMPLEX_OP,
  ISCHARACTER_OP,
  ISSYMBOL_OP,
  ISOBJECT_OP,
  ISNUMERIC_OP,
  VECSUBSET_OP,
  MATSUBSET_OP,
  SETVECSUBSET_OP,
  SETMATSUBSET_OP,
  AND1ST_OP,
  AND2ND_OP,
  OR1ST_OP,
  OR2ND_OP,
  GETVAR_MISSOK_OP,
  DDVAL_MISSOK_OP,
  VISIBLE_OP,
  SETVAR2_OP,
  STARTASSIGN2_OP,
  ENDASSIGN2_OP,
  SETTER_CALL_OP,
  GETTER_CALL_OP,
  SWAP_OP,
  DUP2ND_OP,
  SWITCH_OP,
  RETURNJMP_OP,
  STARTVECSUBSET_OP,
  STARTMATSUBSET_OP,
  STARTSETVECSUBSET_OP,
  STARTSETMATSUBSET_OP,
  OPCOUNT
};

#define GETSTACK_PTR(s) (*(s))
#define GETSTACK(i) GETSTACK_PTR(R_BCNodeStackTop + (i))

#define SETSTACK_PTR(s, v) do { \
    SEXP __v__ = (v); \
    *(s) = __v__; \
} while (0)

#define SETSTACK(i, v) SETSTACK_PTR(R_BCNodeStackTop + (i), v)

#define SETSTACK_REAL_PTR(s, v) SETSTACK_PTR(s, ScalarReal(v))

#define SETSTACK_REAL(i, v) SETSTACK_REAL_PTR(R_BCNodeStackTop + (i), v)

#define SETSTACK_INTEGER_PTR(s, v) SETSTACK_PTR(s, ScalarInteger(v))

#define SETSTACK_INTEGER(i, v) SETSTACK_INTEGER_PTR(R_BCNodeStackTop + (i), v)

#define SETSTACK_LOGICAL_PTR(s, v) do { \
    int __ssl_v__ = (v); \
    if (__ssl_v__ == NA_LOGICAL) \
	SETSTACK_PTR(s, ScalarLogical(NA_LOGICAL)); \
    else \
	SETSTACK_PTR(s, __ssl_v__ ? R_TrueValue : R_FalseValue); \
} while(0)

#define SETSTACK_LOGICAL(i, v) SETSTACK_LOGICAL_PTR(R_BCNodeStackTop + (i), v)

typedef union { double dval; int ival; } scalar_value_t;

/* bcStackScalar() checks whether the object in the specified stack
   location is a simple real, integer, or logical scalar (i.e. length
   one and no attributes.  If so, the type is returned as the function
   value and the value is returned in the structure pointed to by the
   second argument; if not, then zero is returned as the function
   value. */
static R_INLINE int bcStackScalar(R_bcstack_t *s, scalar_value_t *v)
{
    SEXP x = *s;

    if (HAS_ATTRIB(x) || LENGTH(x) != 1)
        return 0;

    int type = TYPEOF(x);

    switch (type) {
        case REALSXP:
            v->dval = REAL(x)[0];
            break;
        case INTSXP:  /* INT and LGL assumed to have same representation */
        case LGLSXP:
            v->ival = INTEGER(x)[0];
            break;
        default: 
            return 0;
    }

    return type;
}

#define DO_FAST_RELOP2(op,a,b) do { \
    SKIP_OP(); \
    SETSTACK_LOGICAL(-2, ((a) op (b)) ? TRUE : FALSE);	\
    R_BCNodeStackTop--; \
    NEXT(); \
} while (0)

# define FastRelop2(op,opval,opsym) do { \
    scalar_value_t vx; \
    scalar_value_t vy; \
    int typex = bcStackScalar(R_BCNodeStackTop - 2, &vx); \
    int typey = bcStackScalar(R_BCNodeStackTop - 1, &vy); \
    if (typex == REALSXP && ! ISNAN(vx.dval)) { \
	if (typey == REALSXP && ! ISNAN(vy.dval)) \
	    DO_FAST_RELOP2(op, vx.dval, vy.dval); \
	else if (typey == INTSXP && vy.ival != NA_INTEGER) \
	    DO_FAST_RELOP2(op, vx.dval, vy.ival); \
    } \
    else if (typex == INTSXP && vx.ival != NA_INTEGER) { \
	if (typey == REALSXP && ! ISNAN(vy.dval)) \
	    DO_FAST_RELOP2(op, vx.ival, vy.dval); \
	else if (typey == INTSXP && vy.ival != NA_INTEGER) { \
	    DO_FAST_RELOP2(op, vx.ival, vy.ival); \
	} \
    } \
    Relop2(opval, opsym); \
} while (0)

/* Handle when probably a package redefined a base function,
   so try to get the real thing from the internal table of
   primitives */

static SEXP getLostPrimitive(SEXP symbol, SEXPTYPE type)
{
    SEXP value = R_Primitive(CHAR(PRINTNAME(symbol)));
    if (TYPEOF(value) != type)
        /* if that doesn't work we signal an error */
        error(_("\"%s\" is not a %s function"),
              CHAR(PRINTNAME(symbol)),
              type == BUILTINSXP ? "BUILTIN" : "SPECIAL");
    return value;
}

static R_INLINE SEXP getPrimitive(SEXP symbol, SEXPTYPE type)
{
    SEXP value = SYMVALUE(symbol);
    if (TYPEOF(value) == PROMSXP) {
	value = forcePromise(value);
	SET_NAMEDCNT_MAX(value);
    }
    if (TYPEOF(value) != type)
        value = getLostPrimitive (symbol, type);
    return value;
}

static SEXP cmp_relop(SEXP call, int opval, SEXP opsym, SEXP x, SEXP y,
		      SEXP rho)
{
    SEXP op = getPrimitive(opsym, SPECIALSXP);
    if (isObject(x) || isObject(y)) {
	SEXP args, ans;
	args = CONS(x, CONS(y, R_NilValue));
	PROTECT(args);
	if (DispatchGroup("Ops", call, op, args, rho, &ans)) {
	    UNPROTECT(1);
	    return ans;
	}
	UNPROTECT(1);
    }
    return R_relop (call, op, x, y, isObject(x), isObject(y), rho, 0);
}

static SEXP cmp_arith1(SEXP call, SEXP opsym, SEXP x, SEXP rho)
{
    SEXP op = getPrimitive(opsym, SPECIALSXP);
    if (isObject(x)) {
	SEXP args, ans;
	args = CONS(x, R_NilValue);
	PROTECT(args);
	if (DispatchGroup("Ops", call, op, args, rho, &ans)) {
	    UNPROTECT(1);
	    return ans;
	}
	UNPROTECT(1);
    }
    return R_unary(call, op, x, isObject(x), rho, 0);
}

static SEXP cmp_arith2(SEXP call, int opval, SEXP opsym, SEXP x, SEXP y,
		       SEXP rho)
{
    SEXP op = getPrimitive(opsym, SPECIALSXP);
    if (TYPEOF(op) == PROMSXP) {
	op = forcePromise(op);
	SET_NAMEDCNT_MAX(op);
    }
    if (isObject(x) || isObject(y)) {
	SEXP args, ans;
	args = CONS(x, CONS(y, R_NilValue));
	PROTECT(args);
	if (DispatchGroup("Ops", call, op, args, rho, &ans)) {
	    UNPROTECT(1);
	    return ans;
	}
	UNPROTECT(1);
    }
    return R_binary(call, op, x, y, isObject(x), isObject(y), rho, 0);
}

#define Builtin1(do_fun,which,rho) do { \
  SEXP call = constants[GETOP()]; \
  SETSTACK(-1, CONS(GETSTACK(-1), R_NilValue));		     \
  SETSTACK(-1, do_fun(call, getPrimitive(which, BUILTINSXP), \
		      GETSTACK(-1), rho, 0));		     \
  NEXT(); \
} while(0)

#define Builtin2(do_fun,which,rho) do {		     \
  SEXP call = constants[GETOP()]; \
  SEXP tmp = CONS(GETSTACK(-1), R_NilValue); \
  SETSTACK(-2, CONS(GETSTACK(-2), tmp));     \
  R_BCNodeStackTop--; \
  SETSTACK(-1, do_fun(call, getPrimitive(which, BUILTINSXP),	\
		      GETSTACK(-1), rho, 0));			\
  NEXT(); \
} while(0)

#define Special2(do_fun,which,rho) do {		     \
  SEXP call = constants[GETOP()]; \
  SEXP tmp = CONS(GETSTACK(-1), R_NilValue); \
  SETSTACK(-2, CONS(GETSTACK(-2), tmp));     \
  R_BCNodeStackTop--; \
  SETSTACK(-1, do_fun(call, getPrimitive(which, SPECIALSXP),	\
		      GETSTACK(-1), rho, 0));			\
  NEXT(); \
} while(0)

#define NewBuiltin2(do_fun,opval,opsym,rho) do {	\
  SEXP call = constants[GETOP()]; \
  SEXP x = GETSTACK(-2); \
  SEXP y = GETSTACK(-1); \
  SETSTACK(-2, do_fun(call, opval, opsym, x, y,rho));	\
  R_BCNodeStackTop--; \
  NEXT(); \
} while(0)

#define Arith1(opsym) do {		\
  SEXP call = constants[GETOP()]; \
  SEXP x = GETSTACK(-1); \
  SETSTACK(-1, cmp_arith1(call, opsym, x, rho)); \
  NEXT(); \
} while(0)


#define Arith2(opval,opsym) NewBuiltin2(cmp_arith2,opval,opsym,rho)
#define Math1(which) Builtin1(do_math1,which,rho)
#define Relop2(opval,opsym) NewBuiltin2(cmp_relop,opval,opsym,rho)

# define DO_FAST_BINOP(op,a,b) do { \
    SKIP_OP(); \
    SETSTACK_REAL(-2, (a) op (b)); \
    R_BCNodeStackTop--; \
    NEXT(); \
} while (0)

# define DO_FAST_BINOP_INT(op, a, b) do { \
    double dval = ((double) (a)) op ((double) (b)); \
    if (dval <= INT_MAX && dval >= INT_MIN + 1) { \
        SKIP_OP(); \
	SETSTACK_INTEGER(-2, (int) dval); \
	R_BCNodeStackTop--; \
	NEXT(); \
    } \
} while(0)

# define FastBinary(op,opval,opsym) do { \
    scalar_value_t vx; \
    scalar_value_t vy; \
    int typex = bcStackScalar(R_BCNodeStackTop - 2, &vx); \
    int typey = bcStackScalar(R_BCNodeStackTop - 1, &vy); \
    if (typex == REALSXP) { \
        if (typey == REALSXP) \
	    DO_FAST_BINOP(op, vx.dval, vy.dval); \
	else if (typey == INTSXP && vy.ival != NA_INTEGER) \
	    DO_FAST_BINOP(op, vx.dval, vy.ival); \
    } \
    else if (typex == INTSXP && vx.ival != NA_INTEGER) { \
	if (typey == REALSXP) \
	    DO_FAST_BINOP(op, vx.ival, vy.dval); \
	else if (typey == INTSXP && vy.ival != NA_INTEGER) { \
	    if (opval == DIVOP) \
		DO_FAST_BINOP(op, (double) vx.ival, (double) vy.ival); \
            else \
		DO_FAST_BINOP_INT(op, vx.ival, vy.ival); \
	} \
    } \
    Arith2(opval, opsym); \
} while (0)

#define BCNPUSH(v) do { \
  SEXP __value__ = (v); \
  R_bcstack_t *__ntop__ = R_BCNodeStackTop + 1; \
  if (__ntop__ > R_BCNodeStackEnd) nodeStackOverflow(); \
  __ntop__[-1] = __value__; \
  R_BCNodeStackTop = __ntop__; \
} while (0)

#define BCNDUP() do { \
    R_bcstack_t *__ntop__ = R_BCNodeStackTop + 1; \
    if (__ntop__ > R_BCNodeStackEnd) nodeStackOverflow(); \
    __ntop__[-1] = __ntop__[-2]; \
    R_BCNodeStackTop = __ntop__; \
} while(0)

#define BCNDUP2ND() do { \
    R_bcstack_t *__ntop__ = R_BCNodeStackTop + 1; \
    if (__ntop__ > R_BCNodeStackEnd) nodeStackOverflow(); \
    __ntop__[-1] = __ntop__[-3]; \
    R_BCNodeStackTop = __ntop__; \
} while(0)

#define BCNPOP() (R_BCNodeStackTop--, GETSTACK(0))
#define BCNPOP_IGNORE_VALUE() R_BCNodeStackTop--

#define BCNSTACKCHECK(n)  do { \
  if (R_BCNodeStackTop + 1 > R_BCNodeStackEnd) nodeStackOverflow(); \
} while (0)

#define BCIPUSHPTR(v)  do { \
  void *__value__ = (v); \
  IStackval *__ntop__ = R_BCIntStackTop + 1; \
  if (__ntop__ > R_BCIntStackEnd) intStackOverflow(); \
  *__ntop__[-1].p = __value__; \
  R_BCIntStackTop = __ntop__; \
} while (0)

#define BCIPUSHINT(v)  do { \
  int __value__ = (v); \
  IStackval *__ntop__ = R_BCIntStackTop + 1; \
  if (__ntop__ > R_BCIntStackEnd) intStackOverflow(); \
  __ntop__[-1].i = __value__; \
  R_BCIntStackTop = __ntop__; \
} while (0)

#define BCIPOPPTR() ((--R_BCIntStackTop)->p)
#define BCIPOPINT() ((--R_BCIntStackTop)->i)

#define BCCONSTS(e) BCODE_CONSTS(e)

static void nodeStackOverflow()
{
    error(_("node stack overflow"));
}

#ifdef BC_INT_STACK
static void intStackOverflow()
{
    error(_("integer stack overflow"));
}
#endif

static SEXP bytecodeExpr(SEXP e)
{
    if (isByteCode(e)) {
	if (LENGTH(BCCONSTS(e)) > 0)
	    return VECTOR_ELT(BCCONSTS(e), 0);
	else return R_NilValue;
    }
    else return e;
}

SEXP R_PromiseExpr(SEXP p)
{
    return bytecodeExpr(PRCODE(p));
}

SEXP R_ClosureExpr(SEXP p)
{
    return bytecodeExpr(BODY(p));
}

#ifdef THREADED_CODE
typedef union { void *v; int i; } BCODE;

static struct { void *addr; int argc; } opinfo[OPCOUNT];

#define OP(name,n) \
  case name##_OP: opinfo[name##_OP].addr = (__extension__ &&op_##name); \
    opinfo[name##_OP].argc = (n); \
    goto loop; \
    op_##name

#define BEGIN_MACHINE  NEXT(); init: { loop: switch(which++)
#define LASTOP } value = R_NilValue; goto done
#define INITIALIZE_MACHINE() if (body == R_NoObject) goto init

#define NEXT() (__extension__ ({goto *(*pc++).v;}))
#define GETOP() (*pc++).i
#define SKIP_OP() (pc++)

#define BCCODE(e) (BCODE *) INTEGER(BCODE_CODE(e))
#else
typedef int BCODE;

#define OP(name,argc) case name##_OP

#ifdef BC_PROFILING
#define BEGIN_MACHINE  loop: current_opcode = *pc; switch(*pc++)
#else
#define BEGIN_MACHINE  loop: switch(*pc++)
#endif
#define LASTOP  default: error(_("Bad opcode"))
#define INITIALIZE_MACHINE()

#define NEXT() goto loop
#define GETOP() *pc++
#define SKIP_OP() (pc++)

#define BCCODE(e) INTEGER(BCODE_CODE(e))
#endif

static R_INLINE SEXP GET_BINDING_CELL(SEXP symbol, SEXP rho)
{
    if (IS_BASE(rho))
	return R_NilValue;
    else {
	SEXP loc = (SEXP) R_findVarLocInFrame(rho, symbol);
	return loc != R_NoObject ? loc : R_NilValue;
    }
}

static R_INLINE Rboolean SET_BINDING_VALUE(SEXP loc, SEXP value) {
    /* This depends on the current implementation of bindings */
    if (loc != R_NilValue &&
	! BINDING_IS_LOCKED(loc) && ! IS_ACTIVE_BINDING(loc)) {
	if (CAR(loc) != value) {
	    SETCAR(loc, value);
	    if (MISSING(loc))
		SET_MISSING(loc, 0);
	}
	return TRUE;
    }
    else
	return FALSE;
}

static R_INLINE SEXP BINDING_VALUE(SEXP loc)
{
    if (loc != R_NilValue && ! IS_ACTIVE_BINDING(loc))
	return CAR(loc);
    else
	return R_UnboundValue;
}

#define BINDING_SYMBOL(loc) TAG(loc)

/* Defining USE_BINDING_CACHE enables a cache for GETVAR, SETVAR, and
   others to more efficiently locate bindings in the top frame of the
   current environment.  The index into of the symbol in the constant
   table is used as the cache index.  Two options can be used to chose
   among implementation strategies:

       If CACHE_ON_STACK is defined the the cache is allocated on the
       byte code stack. Otherwise it is allocated on the heap as a
       VECSXP.  The stack-based approach is more efficient, but runs
       the risk of running out of stack space.

       If CACHE_MAX is defined, then a cache of at most that size is
       used. The value must be a power of 2 so a modulus computation x
       % CACHE_MAX can be done as x & (CACHE_MAX - 1). More than 90%
       of the closures in base have constant pools with fewer than 128
       entries when compiled, to that is a good value to use.

   On average about 1/3 of constant pool entries are symbols, so this
   approach wastes some space.  This could be avoided by grouping the
   symbols at the beginning of the constant pool and recording the
   number.

   Bindings recorded may become invalid if user code removes a
   variable.  The code in envir.c has been modified to insert
   R_unboundValue as the value of a binding when it is removed, and
   code using cached bindings checks for this.

   It would be nice if we could also cache bindings for variables
   found in enclosing environments. These would become invalid if a
   new variable is defined in an intervening frame. Some mechanism for
   invalidating the cache would be needed. This is certainly possible,
   but finding an efficient mechanism does not seem to be easy.   LT */

/* Both mechanisms implemented here make use of the stack to hold
   cache information.  This is not a problem except for "safe" for()
   loops using the STARTLOOPCNTXT instruction to run the body in a
   separate bcEval call.  Since this approach expects loop setup
   information to be passed on the stack from the outer bcEval call to
   an inner one the inner one cannot put things on the stack. For now,
   bcEval takes an additional argument that disables the cache in
   calls via STARTLOOPCNTXT for all "safe" loops. It would be better
   to deal with this in some other way, for example by having a
   specific STARTFORLOOPCNTXT instruction that deals with transferring
   the information in some other way. For now disabling the cache is
   an expedient solution. LT */

#define USE_BINDING_CACHE
# ifdef USE_BINDING_CACHE
/* CACHE_MAX must be a power of 2 for modulus using & CACHE_MASK to work*/
# define CACHE_MAX 128
# ifdef CACHE_MAX
#  define CACHE_MASK (CACHE_MAX - 1)
#  define CACHEIDX(i) ((i) & CACHE_MASK)
# else
#  define CACHEIDX(i) (i)
# endif

# define CACHE_ON_STACK
# ifdef CACHE_ON_STACK
typedef R_bcstack_t * R_binding_cache_t;
#  define GET_CACHED_BINDING_CELL(vcache, sidx) \
    (vcache ? vcache[CACHEIDX(sidx)] : R_NilValue)
#  define GET_SMALLCACHE_BINDING_CELL(vcache, sidx) \
    (vcache ? vcache[sidx] : R_NilValue)

#  define SET_CACHED_BINDING(cvache, sidx, cell) \
    do { if (vcache) vcache[CACHEIDX(sidx)] = (cell); } while (0)
# else
typedef SEXP R_binding_cache_t;
#  define GET_CACHED_BINDING_CELL(vcache, sidx) \
    (vcache ? VECTOR_ELT(vcache, CACHEIDX(sidx)) : R_NilValue)
#  define GET_SMALLCACHE_BINDING_CELL(vcache, sidx) \
    (vcache ? VECTOR_ELT(vcache, sidx) : R_NilValue)

#  define SET_CACHED_BINDING(vcache, sidx, cell) \
    do { if (vcache) SET_VECTOR_ELT(vcache, CACHEIDX(sidx), cell); } while (0)
# endif
#else
typedef void *R_binding_cache_t;
# define GET_CACHED_BINDING_CELL(vcache, sidx) R_NilValue
# define GET_SMALLCACHE_BINDING_CELL(vcache, sidx) R_NilValue

# define SET_CACHED_BINDING(vcache, sidx, cell)
#endif

static R_INLINE SEXP GET_BINDING_CELL_CACHE(SEXP symbol, SEXP rho,
					    R_binding_cache_t vcache, int idx)
{
    SEXP cell = GET_CACHED_BINDING_CELL(vcache, idx);
    /* The value returned by GET_CACHED_BINDING_CELL is either a
       binding cell or R_NilValue.  TAG(R_NilValue) is R_NilVelue, and
       that will no equal symbol. So a separate test for cell !=
       R_NilValue is not needed. */
    if (TAG(cell) == symbol && CAR(cell) != R_UnboundValue)
	return cell;
    else {
	SEXP ncell = GET_BINDING_CELL(symbol, rho);
	if (ncell != R_NilValue)
	    SET_CACHED_BINDING(vcache, idx, ncell);
	else if (cell != R_NilValue && CAR(cell) == R_UnboundValue)
	    SET_CACHED_BINDING(vcache, idx, R_NilValue);
	return ncell;
    }
}

static R_INLINE SEXP FORCE_PROMISE(SEXP value, SEXP symbol, SEXP rho,
				   Rboolean keepmiss)
{
    if (PRVALUE(value) == R_UnboundValue) {
	/**** R_isMissing is inefficient */
	if (keepmiss && R_isMissing(symbol, rho))
	    value = R_MissingArg;
	else 
            value = forcePromise(value);
    }
    else 
        value = PRVALUE(value);
    return value;
}

static R_INLINE SEXP getddvar(SEXP symbol, SEXP rho, Rboolean keepmiss)
{
    SEXP value = ddfindVar(symbol, rho);

    if (TYPEOF(value) == PROMSXP)
        return FORCE_PROMISE(value, symbol, rho, keepmiss);

    if (value == R_UnboundValue)
	unbound_var_error(symbol);

    if (value == R_MissingArg && !keepmiss)
	arg_missing_error(symbol);

    if (NAMEDCNT_EQ_0(value))
	SET_NAMEDCNT_1(value);

    return value;
}

static R_INLINE SEXP getvar(SEXP symbol, SEXP rho, Rboolean keepmiss,
                            R_binding_cache_t vcache, int sidx)
                            /* use getddvar if a .. var */
{
    SEXP value;

    if (vcache != NULL) {
	SEXP cell = GET_BINDING_CELL_CACHE(symbol, rho, vcache, sidx);
	value = BINDING_VALUE(cell);
	if (value == R_UnboundValue) {
            /* only need to search the current frame again if
               binding was special or frame is a base frame */
            if (cell != R_NilValue || IS_BASE(rho))
                value =  findVar(symbol, rho);
            else
                value =  findVar(symbol, ENCLOS(rho));
        }
    }
    else
	value = findVar(symbol, rho);

    if (TYPEOF(value) == PROMSXP)
        return FORCE_PROMISE(value, symbol, rho, keepmiss);

    if (value == R_UnboundValue)
	unbound_var_error(symbol);

    if (value == R_MissingArg && !keepmiss)
	arg_missing_error(symbol);

    if (NAMEDCNT_EQ_0(value))
	SET_NAMEDCNT_1(value);

    return value;
}

#define INLINE_GETVAR
#ifdef INLINE_GETVAR
/* Try to handle the most common case as efficiently as possible.  If
   smallcache is true then a modulus operation on the index is not
   needed, nor is a check that a non-null value corresponds to the
   requested symbol. The symbol from the constant pool is also usually
   not needed. The test TYPOF(value) != SYMBOL rules out R_MissingArg
   and R_UnboundValue as these are implemented s symbols.  It also
   rules other symbols, but as those are rare they are handled by the
   getvar() call. */
#define DO_GETVAR(dd,keepmiss) do { \
    int sidx = GETOP(); \
    if (!dd && smallcache) { \
	SEXP cell = GET_SMALLCACHE_BINDING_CELL(vcache, sidx); \
	/* try fast handling of REALSXP, INTSXP, LGLSXP */ \
	/* (cell won't be R_NilValue or an active binding) */ \
	value = CAR(cell); \
	int type = TYPEOF(value); \
	switch(type) { \
	case REALSXP: \
	case INTSXP: \
	case LGLSXP: \
	    /* may be ok to skip this test: */ \
	    if (NAMEDCNT_EQ_0(value)) \
		SET_NAMEDCNT_1(value); \
	    R_Visible = TRUE; \
	    BCNPUSH(value); \
	    NEXT(); \
	} \
	if (cell != R_NilValue && ! IS_ACTIVE_BINDING(cell)) { \
	    value = CAR(cell); \
	    if (TYPEOF(value) != SYMSXP) {	\
		if (TYPEOF(value) == PROMSXP) {		\
		    SEXP pv = PRVALUE(value);		\
		    if (pv == R_UnboundValue) {		\
			SEXP symbol = constants[sidx];	\
			value = FORCE_PROMISE(value, symbol, rho, keepmiss); \
		    }							\
		    else value = pv;					\
		}							\
		else if (NAMEDCNT_EQ_0(value))				\
		    SET_NAMEDCNT_1(value);				\
		R_Visible = TRUE;					\
		BCNPUSH(value);						\
		NEXT();							\
	    }								\
	}								\
    }									\
    SEXP symbol = constants[sidx];					\
    R_Visible = TRUE;							\
    BCNPUSH(dd ? getddvar (symbol, rho, keepmiss)			\
               : getvar (symbol, rho, keepmiss, vcache, sidx));		\
    NEXT();								\
} while (0)
#else
#define DO_GETVAR(dd,keepmiss) do { \
  int sidx = GETOP(); \
  SEXP symbol = constants[sidx]; \
  R_Visible = TRUE; \
  BCNPUSH(dd ? getddvar (symbol, rho, keepmiss)				\
               getvar (symbol, rho, dd, keepmiss, vcache, sidx);	\
  NEXT(); \
} while (0)
#endif

#define PUSHCALLARG(v) PUSHCALLARG_CELL(CONS(v, R_NilValue))

#define PUSHCALLARG_CELL(c) do { \
  SEXP __cell__ = (c); \
  if (GETSTACK(-2) == R_NilValue) SETSTACK(-2, __cell__); \
  else SETCDR(GETSTACK(-1), __cell__); \
  SETSTACK(-1, __cell__);	       \
} while (0)

static int tryDispatch(char *generic, SEXP call, SEXP x, SEXP rho, SEXP *pv)
{
  RCNTXT cntxt;
  SEXP pargs, rho1;
  int dispatched = FALSE;
  SEXP op = SYMVALUE(install(generic)); /**** avoid this */

  PROTECT(pargs = promiseArgsWith1Value(CDR(call), rho, x));

  /**** Minimal hack to try to handle the S4 case.  If we do the check
	and do not dispatch then some arguments beyond the first might
	have been evaluated; these will then be evaluated again by the
	compiled argument code. */
  if (IS_S4_OBJECT(x) && R_has_methods(op)) {
    SEXP val = R_possible_dispatch(call, op, pargs, rho, TRUE);
    if (val != R_NoObject) {
      *pv = val;
      UNPROTECT(1);
      return TRUE;
    }
  }

  /* See comment at first usemethod() call in this file. LT */
  PROTECT(rho1 = NewEnvironment(R_NilValue, R_NilValue, rho));
  begincontext(&cntxt, CTXT_RETURN, call, rho1, rho, pargs, op);
  if (usemethod(generic, x, call, pargs, rho1, rho, R_BaseEnv, 0, pv))
    dispatched = TRUE;
  endcontext(&cntxt);
  UNPROTECT(2);
  return dispatched;
}

static int tryAssignDispatch(char *generic, SEXP call, SEXP lhs, SEXP rhs,
			     SEXP rho, SEXP *pv)
{
    int result;
    SEXP ncall, last, prom;

    PROTECT(ncall = duplicate(call));
    last = ncall;
    while (CDR(last) != R_NilValue)
	last = CDR(last);
    prom = mkPROMISE(CAR(last), rho);
    SET_PRVALUE(prom, rhs);
    INC_NAMEDCNT(rhs);
    SETCAR(last, prom);
    result = tryDispatch(generic, ncall, lhs, rho, pv);
    UNPROTECT(1);
    return result;
}

#define DO_STARTDISPATCH(generic) do { \
  SEXP call = constants[GETOP()]; \
  int label = GETOP(); \
  value = GETSTACK(-1); \
  if (isObject(value) && tryDispatch(generic, call, value, rho, &value)) {\
    SETSTACK(-1, value);						\
    BC_CHECK_SIGINT(); \
    pc = codebase + label; \
  } \
  else { \
    SEXP tag = TAG(CDR(call)); \
    SEXP cell = CONS(value, R_NilValue); \
    BCNSTACKCHECK(3); \
    SETSTACK(0, call); \
    SETSTACK(1, cell); \
    SETSTACK(2, cell); \
    R_BCNodeStackTop += 3; \
    if (tag != R_NilValue) \
      SET_TAG(cell, CreateTag(tag)); \
  } \
  NEXT(); \
} while (0)

#define DO_DFLTDISPATCH0(fun, symbol) do { \
  SEXP call = GETSTACK(-3); \
  SEXP args = GETSTACK(-2); \
  value = fun(call, symbol, args, rho, 0); \
  R_BCNodeStackTop -= 3; \
  SETSTACK(-1, value); \
  NEXT(); \
} while (0)

#define DO_DFLTDISPATCH(fun, symbol) do { \
  SEXP call = GETSTACK(-3); \
  SEXP args = GETSTACK(-2); \
  value = fun(call, symbol, args, rho); \
  R_BCNodeStackTop -= 3; \
  SETSTACK(-1, value); \
  NEXT(); \
} while (0)

#define DO_START_ASSIGN_DISPATCH(generic) do { \
  SEXP call = constants[GETOP()]; \
  int label = GETOP(); \
  SEXP lhs = GETSTACK(-2); \
  SEXP rhs = GETSTACK(-1); \
  if (NAMEDCNT_GT_1(lhs) && lhs != R_NilValue) { \
    lhs = duplicate(lhs); \
    SETSTACK(-2, lhs); \
    SET_NAMEDCNT_1(lhs); \
  } \
  if (isObject(lhs) && \
      tryAssignDispatch(generic, call, lhs, rhs, rho, &value)) { \
    R_BCNodeStackTop--;	\
    SETSTACK(-1, value); \
    BC_CHECK_SIGINT(); \
    pc = codebase + label; \
  } \
  else { \
    SEXP tag = TAG(CDR(call)); \
    SEXP cell = CONS(lhs, R_NilValue); \
    BCNSTACKCHECK(3); \
    SETSTACK(0, call); \
    SETSTACK(1, cell); \
    SETSTACK(2, cell); \
    R_BCNodeStackTop += 3; \
    if (tag != R_NilValue) \
      SET_TAG(cell, CreateTag(tag)); \
  } \
  NEXT(); \
} while (0)

#define DO_DFLT_ASSIGN_DISPATCH(fun, symbol) do { \
  SEXP rhs = GETSTACK(-4); \
  SEXP call = GETSTACK(-3); \
  SEXP args = GETSTACK(-2); \
  PUSHCALLARG(rhs); \
  value = fun(call, symbol, args, rho); \
  R_BCNodeStackTop -= 4; \
  SETSTACK(-1, value);	 \
  NEXT(); \
} while (0)

#define DO_STARTDISPATCH_N(generic) do { \
    int callidx = GETOP(); \
    int label = GETOP(); \
    value = GETSTACK(-1); \
    if (isObject(value)) { \
	SEXP call = constants[callidx]; \
	if (tryDispatch(generic, call, value, rho, &value)) { \
	    SETSTACK(-1, value); \
	    BC_CHECK_SIGINT(); \
	    pc = codebase + label; \
	} \
    } \
    NEXT(); \
} while (0)

#define DO_START_ASSIGN_DISPATCH_N(generic) do { \
    int callidx = GETOP(); \
    int label = GETOP(); \
    SEXP lhs = GETSTACK(-2); \
    if (isObject(lhs)) { \
	SEXP call = constants[callidx]; \
	SEXP rhs = GETSTACK(-1); \
	if (NAMEDCNT_GT_1(lhs) && lhs != R_NilValue) { \
	    lhs = duplicate(lhs); \
	    SETSTACK(-2, lhs); \
	    SET_NAMEDCNT_1(lhs); \
	} \
	if (tryAssignDispatch(generic, call, lhs, rhs, rho, &value)) { \
	    R_BCNodeStackTop--; \
	    SETSTACK(-1, value); \
	    BC_CHECK_SIGINT(); \
	    pc = codebase + label; \
	} \
    } \
    NEXT(); \
} while (0)

#define DO_ISTEST(fun) do { \
  SETSTACK(-1, fun(GETSTACK(-1)) ? R_TrueValue : R_FalseValue);	\
  NEXT(); \
} while(0)
#define DO_ISTYPE(type) do { \
  SETSTACK(-1, TYPEOF(GETSTACK(-1)) == type ? mkTrue() : mkFalse()); \
  NEXT(); \
} while (0)
#define isNumericOnly(x) (isNumeric(x) && ! isLogical(x))

#ifdef BC_PROFILING
#define NO_CURRENT_OPCODE -1
static int current_opcode = NO_CURRENT_OPCODE;
static int opcode_counts[OPCOUNT];
#endif

#define BC_COUNT_DELTA 1000

#define BC_CHECK_SIGINT() do { \
  if (++eval_count > BC_COUNT_DELTA) { \
      R_CheckUserInterrupt(); \
      eval_count = 0; \
  } \
} while (0)

static void loopWithContext(volatile SEXP code, volatile SEXP rho)
{
    RCNTXT cntxt;
    begincontext(&cntxt, CTXT_LOOP, R_NilValue, rho, R_BaseEnv, R_NilValue,
		 R_NilValue);
    if (SETJMP(cntxt.cjmpbuf) != CTXT_BREAK)
	bcEval(code, rho, FALSE);
    endcontext(&cntxt);
}

static R_INLINE int bcStackIndex(R_bcstack_t *s)
{
    SEXP idx = *s;
    int ival = -1;

    if (LENGTH(idx) == 1) {

        switch(TYPEOF(idx)) {
        case INTSXP:
            ival = INTEGER(idx)[0];
            if (ival == NA_INTEGER)
                ival = -1;
            break;
        case REALSXP: ;
            double val = REAL(idx)[0];
            if (!ISNAN(val) && val <= INT_MAX && val > INT_MIN)
                ival = (int) val;
            break;
        }
    }

    return ival;
}

static void VECSUBSET_PTR(R_bcstack_t *sx, R_bcstack_t *si,
                          R_bcstack_t *sv, SEXP rho)
{
    SEXP idx, args, value;
    SEXP vec = GETSTACK_PTR(sx);
    int i = bcStackIndex(si) - 1;

    if (!HAS_ATTRIB(vec) && i >= 0) {
	switch (TYPEOF(vec)) {
	case REALSXP:
	    if (LENGTH(vec) <= i) break;
	    SETSTACK_REAL_PTR(sv, REAL(vec)[i]);
	    return;
	case INTSXP:
	    if (LENGTH(vec) <= i) break;
	    SETSTACK_INTEGER_PTR(sv, INTEGER(vec)[i]);
	    return;
	case LGLSXP:
	    if (LENGTH(vec) <= i) break;
	    SETSTACK_LOGICAL_PTR(sv, LOGICAL(vec)[i]);
	    return;
	case CPLXSXP:
	    if (LENGTH(vec) <= i) break;
	    SETSTACK_PTR(sv, ScalarComplex(COMPLEX(vec)[i]));
	    return;
	case RAWSXP:
	    if (LENGTH(vec) <= i) break;
	    SETSTACK_PTR(sv, ScalarRaw(RAW(vec)[i]));
	    return;
	}
    }

    /* fall through to the standard default handler */
    idx = GETSTACK_PTR(si);
    args = CONS(idx, R_NilValue);
    args = CONS(vec, args);
    PROTECT(args);
    value = do_subset_dflt(R_NilValue, R_BracketSymbol, args, rho);
    UNPROTECT(1);
    SETSTACK_PTR(sv, value);
}

#define DO_VECSUBSET(rho) do { \
    VECSUBSET_PTR(R_BCNodeStackTop - 2, R_BCNodeStackTop - 1, \
		  R_BCNodeStackTop - 2, rho); \
    R_BCNodeStackTop--; \
} while(0)

static R_INLINE SEXP getMatrixDim(SEXP mat)
{
    if (! OBJECT(mat) &&
	TAG(ATTRIB(mat)) == R_DimSymbol &&
	CDR(ATTRIB(mat)) == R_NilValue) {
	SEXP dim = CAR(ATTRIB(mat));
	if (TYPEOF(dim) == INTSXP && LENGTH(dim) == 2)
	    return dim;
	else return R_NilValue;
    }
    else return R_NilValue;
}

static R_INLINE void DO_MATSUBSET(SEXP rho)
{
    SEXP idx, jdx, args, value;
    SEXP mat = GETSTACK(-3);
    SEXP dim = getMatrixDim(mat);

    if (dim != R_NilValue) {
	int i = bcStackIndex(R_BCNodeStackTop - 2);
	int j = bcStackIndex(R_BCNodeStackTop - 1);
	int nrow = INTEGER(dim)[0];
	int ncol = INTEGER(dim)[1];
	if (i > 0 && j > 0 && i <= nrow && j <= ncol) {
	    int k = i - 1 + nrow * (j - 1);
	    switch (TYPEOF(mat)) {
	    case REALSXP:
		if (LENGTH(mat) <= k) break;
		R_BCNodeStackTop -= 2;
		SETSTACK_REAL(-1, REAL(mat)[k]);
		return;
	    case INTSXP:
		if (LENGTH(mat) <= k) break;
		R_BCNodeStackTop -= 2;
		SETSTACK_INTEGER(-1, INTEGER(mat)[k]);
		return;
	    case LGLSXP:
		if (LENGTH(mat) <= k) break;
		R_BCNodeStackTop -= 2;
		SETSTACK_LOGICAL(-1, LOGICAL(mat)[k]);
		return;
	    case CPLXSXP:
		if (LENGTH(mat) <= k) break;
		R_BCNodeStackTop -= 2;
		SETSTACK(-1, ScalarComplex(COMPLEX(mat)[k]));
		return;
	    }
	}
    }

    /* fall through to the standard default handler */
    idx = GETSTACK(-2);
    jdx = GETSTACK(-1);
    args = CONS(jdx, R_NilValue);
    args = CONS(idx, args);
    args = CONS(mat, args);
    SETSTACK(-1, args); /* for GC protection */
    value = do_subset_dflt(R_NilValue, R_BracketSymbol, args, rho);
    R_BCNodeStackTop -= 2;
    SETSTACK(-1, value);
}

#define INTEGER_TO_REAL(x) ((x) == NA_INTEGER ? NA_REAL : (x))
#define LOGICAL_TO_REAL(x) ((x) == NA_LOGICAL ? NA_REAL : (x))

static R_INLINE Rboolean setElementFromScalar(SEXP vec, int i, int typev,
					      scalar_value_t *v)
{
    if (i < 0) return FALSE;

    if (TYPEOF(vec) == REALSXP) {
	if (LENGTH(vec) <= i) return FALSE;
	switch(typev) {
	case REALSXP: REAL(vec)[i] = v->dval; return TRUE;
	case INTSXP: REAL(vec)[i] = INTEGER_TO_REAL(v->ival); return TRUE;
	case LGLSXP: REAL(vec)[i] = LOGICAL_TO_REAL(v->ival); return TRUE;
	}
    }
    else if (typev == TYPEOF(vec)) {
	if (LENGTH(vec) <= i) return FALSE;
	switch (typev) {
	case INTSXP: INTEGER(vec)[i] = v->ival; return TRUE;
	case LGLSXP: LOGICAL(vec)[i] = v->ival; return TRUE;
	}
    }
    return FALSE;
}

static R_INLINE void SETVECSUBSET_PTR(R_bcstack_t *sx, R_bcstack_t *srhs,
				      R_bcstack_t *si, R_bcstack_t *sv,
				      SEXP rho)
{
    SEXP idx, args, value;
    SEXP vec = GETSTACK_PTR(sx);

    if (NAMEDCNT_GT_1(vec)) {
	vec = duplicate(vec);
	SETSTACK_PTR(sx, vec);
    }
    else
	SET_NAMEDCNT_0(vec);

    if (!HAS_ATTRIB(vec)) {
	int i = bcStackIndex(si);
	if (i > 0) {
	    scalar_value_t v;
	    int typev = bcStackScalar(srhs, &v);
	    if (setElementFromScalar(vec, i - 1, typev, &v)) {
		SETSTACK_PTR(sv, vec);
		return;
	    }
	}
    }

    /* fall through to the standard default handler */
    value = GETSTACK_PTR(srhs);
    idx = GETSTACK_PTR(si);
    args = CONS(value, R_NilValue);
    SET_TAG(args, R_ValueSymbol);
    args = CONS(idx, args);
    args = CONS(vec, args);
    PROTECT(args);
    vec = do_subassign_dflt(R_NilValue, R_SubAssignSymbol, args, rho);
    UNPROTECT(1);
    SETSTACK_PTR(sv, vec);
}

static void DO_SETVECSUBSET(SEXP rho)
{
    SETVECSUBSET_PTR(R_BCNodeStackTop - 3, R_BCNodeStackTop - 2,
		     R_BCNodeStackTop - 1, R_BCNodeStackTop - 3, rho);
    R_BCNodeStackTop -= 2;
}

static void DO_SETMATSUBSET(SEXP rho)
{
    SEXP dim, idx, jdx, args, value;
    SEXP mat = GETSTACK(-4);

    if (NAMEDCNT_GT_1(mat)) {
	mat = duplicate(mat);
	SETSTACK(-4, mat);
    }
    else
	SET_NAMEDCNT_0(mat);

    dim = getMatrixDim(mat);

    if (dim != R_NilValue) {
	int i = bcStackIndex(R_BCNodeStackTop - 2);
	int j = bcStackIndex(R_BCNodeStackTop - 1);
	int nrow = INTEGER(dim)[0];
	int ncol = INTEGER(dim)[1];
	if (i > 0 && j > 0 && i <= nrow && j <= ncol) {
	    scalar_value_t v;
	    int typev = bcStackScalar(R_BCNodeStackTop - 3, &v);
	    int k = i - 1 + nrow * (j - 1);
	    if (setElementFromScalar(mat, k, typev, &v)) {
		R_BCNodeStackTop -= 3;
		SETSTACK(-1, mat);
		return;
	    }
	}
    }

    /* fall through to the standard default handler */
    value = GETSTACK(-3);
    idx = GETSTACK(-2);
    jdx = GETSTACK(-1);
    args = CONS(value, R_NilValue);
    SET_TAG(args, R_ValueSymbol);
    args = CONS(jdx, args);
    args = CONS(idx, args);
    args = CONS(mat, args);
    SETSTACK(-1, args); /* for GC protection */
    mat = do_subassign_dflt(R_NilValue, R_SubAssignSymbol, args, rho);
    R_BCNodeStackTop -= 3;
    SETSTACK(-1, mat);
}

#define FIXUP_SCALAR_LOGICAL(callidx, arg, op) do { \
	SEXP val = GETSTACK(-1); \
	if (TYPEOF(val) != LGLSXP || LENGTH(val) != 1) { \
	    if (!isNumber(val))	\
		errorcall(constants[callidx], \
			  _("invalid %s type in 'x %s y'"), arg, op);	\
	    SETSTACK(-1, ScalarLogical(asLogical(val))); \
	} \
    } while(0)

static void maybe_missing_error(SEXP call)
{
    SEXP c;
    int k;

    if (call == R_NilValue) 
        return;

    /* check for an empty argument in the call -- start from
       the beginning in case of ... arguments */
    for (k = 1, c = CDR(call); c != R_NilValue; c = CDR(c), k++)
        if (CAR(c) == R_MissingArg)
            errorcall(call, "argument %d is empty", k);

    /* An error from evaluating a symbol will already have
       been signaled.  The interpreter, in evalList, does
       _not_ signal an error for a call expression that
       produces an R_MissingArg value; for example
       
           c(alist(a=)$a)

       does not signal an error. */
}

static R_INLINE void checkForMissings(SEXP args, SEXP call)
{
    SEXP a;
    for (a = args; a != R_NilValue; a = CDR(a))
	if (CAR(a) == R_MissingArg) {
            maybe_missing_error(call);
            return;
        }
}

#define GET_VEC_LOOP_VALUE(var, pos) do {		\
    (var) = GETSTACK(pos);				\
    if (NAMEDCNT_GT_1(var)) {				\
	(var) = allocVector(TYPEOF(seq), 1);		\
	SETSTACK(pos, var);				\
	SET_NAMEDCNT_1(var);				\
    }							\
} while (0)

static R_NORETURN void bad_function_error (void)
{
    error(_("bad function"));
}

static SEXP bcEval(SEXP body, SEXP rho, Rboolean useCache)
{
  SEXP value;
  SEXP *constants;
  BCODE *pc, *codebase;
  int ftype = 0;
  R_bcstack_t *oldntop = R_BCNodeStackTop;
  static int eval_count = 0;
#ifdef BC_INT_STACK
  IStackval *olditop = R_BCIntStackTop;
#endif
#ifdef BC_PROFILING
  int old_current_opcode = current_opcode;
#endif
#ifdef THREADED_CODE
  int which = 0;
#endif

  BC_CHECK_SIGINT();

  INITIALIZE_MACHINE();
  codebase = pc = BCCODE(body);
  constants = (SEXP *) DATAPTR(BCCONSTS(body));

  /* allow bytecode to be disabled for testing */
  if (R_disable_bytecode)
      return eval(bytecodeExpr(body), rho);

  /* check version */
  {
      int version = GETOP();
      if (version < R_bcMinVersion || version > R_bcVersion) {
	  if (version >= 2) {
	      static Rboolean warned = FALSE;
	      if (! warned) {
		  warned = TRUE;
		  warning(_("bytecode version mismatch; using eval"));
	      }
	      return eval(bytecodeExpr(body), rho);
	  }
	  else if (version < R_bcMinVersion)
	      error(_("bytecode version is too old"));
	  else error(_("bytecode version is too new"));
      }
  }

  R_binding_cache_t vcache = NULL;
  Rboolean smallcache = TRUE;
#ifdef USE_BINDING_CACHE
  if (useCache) {
      R_len_t n = LENGTH(BCCONSTS(body));
# ifdef CACHE_MAX
      if (n > CACHE_MAX) {
	  n = CACHE_MAX;
	  smallcache = FALSE;
      }
# endif
# ifdef CACHE_ON_STACK
      /* initialize binding cache on the stack */
      vcache = R_BCNodeStackTop;
      if (R_BCNodeStackTop + n > R_BCNodeStackEnd)
	  nodeStackOverflow();
      while (n > 0) {
	  *R_BCNodeStackTop = R_NilValue;
	  R_BCNodeStackTop++;
	  n--;
      }
# else
      /* allocate binding cache and protect on stack */
      vcache = allocVector(VECSXP, n);
      BCNPUSH(vcache);
# endif
  }
#endif

  BEGIN_MACHINE {
    OP(BCMISMATCH, 0): error(_("byte code version mismatch"));
    OP(RETURN, 0): value = GETSTACK(-1); goto done;
    OP(GOTO, 1):
      {
	int label = GETOP();
	BC_CHECK_SIGINT();
	pc = codebase + label;
	NEXT();
      }
    OP(BRIFNOT, 2):
      {
	int callidx = GETOP();
	int label = GETOP();
	int cond;
	SEXP call = constants[callidx];
	value = BCNPOP();
	cond = asLogicalNoNA(value, call);
	if (! cond) {
	    BC_CHECK_SIGINT(); /**** only on back branch?*/
	    pc = codebase + label;
	}
	NEXT();
      }
    OP(POP, 0): BCNPOP_IGNORE_VALUE(); NEXT();
    OP(DUP, 0): BCNDUP(); NEXT();
    OP(PRINTVALUE, 0): PrintValue(BCNPOP()); NEXT();
    OP(STARTLOOPCNTXT, 1):
	{
	    SEXP code = constants[GETOP()];
	    loopWithContext(code, rho);
	    NEXT();
	}
    OP(ENDLOOPCNTXT, 0): value = R_NilValue; goto done;
    OP(DOLOOPNEXT, 0): findcontext(CTXT_NEXT, rho, R_NilValue);
    OP(DOLOOPBREAK, 0): findcontext(CTXT_BREAK, rho, R_NilValue);
    OP(STARTFOR, 3):
      {
	SEXP seq = GETSTACK(-1);
	int callidx = GETOP();
	SEXP symbol = constants[GETOP()];
	int label = GETOP();

	/* if we are iterating over a factor, coerce to character first */
	if (inherits(seq, "factor")) {
	    seq = asCharacterFactor(seq);
	    SETSTACK(-1, seq);
	}

	defineVar(symbol, R_NilValue, rho);
	BCNPUSH(GET_BINDING_CELL(symbol, rho));

	value = allocVector(INTSXP, 2);
	INTEGER(value)[0] = -1;
	if (isVector(seq))
	  INTEGER(value)[1] = LENGTH(seq);
	else if (isList(seq) || isNull(seq))
	  INTEGER(value)[1] = length(seq);
	else errorcall(constants[callidx],
		       _("invalid for() loop sequence"));
	BCNPUSH(value);

	/* bump up NAMED count of seq to avoid modification by loop code */
	INC_NAMEDCNT(seq);

	/* place initial loop variable value object on stack */
	switch(TYPEOF(seq)) {
	case LGLSXP:
	case INTSXP:
	case REALSXP:
	case CPLXSXP:
	case STRSXP:
	case RAWSXP:
	    value = allocVector(TYPEOF(seq), 1);
	    BCNPUSH(value);
	    break;
	default: BCNPUSH(R_NilValue);
	}

	BC_CHECK_SIGINT();
	pc = codebase + label;
	NEXT();
      }
    OP(STEPFOR, 1):
      {
	int label = GETOP();
	int i = ++(INTEGER(GETSTACK(-2))[0]);
	int n = INTEGER(GETSTACK(-2))[1];
	if (i < n) {
	  SEXP seq = GETSTACK(-4);
	  SEXP cell = GETSTACK(-3);
	  switch (TYPEOF(seq)) {
	  case LGLSXP:
	    GET_VEC_LOOP_VALUE(value, -1);
	    LOGICAL(value)[0] = LOGICAL(seq)[i];
	    break;
	  case INTSXP:
	    GET_VEC_LOOP_VALUE(value, -1);
	    INTEGER(value)[0] = INTEGER(seq)[i];
	    break;
	  case REALSXP:
	    GET_VEC_LOOP_VALUE(value, -1);
	    REAL(value)[0] = REAL(seq)[i];
	    break;
	  case CPLXSXP:
	    GET_VEC_LOOP_VALUE(value, -1);
	    COMPLEX(value)[0] = COMPLEX(seq)[i];
	    break;
	  case STRSXP:
	    GET_VEC_LOOP_VALUE(value, -1);
	    SET_STRING_ELT(value, 0, STRING_ELT(seq, i));
	    break;
	  case RAWSXP:
	    GET_VEC_LOOP_VALUE(value, -1);
	    RAW(value)[0] = RAW(seq)[i];
	    break;
	  case EXPRSXP:
	  case VECSXP:
	    value = VECTOR_ELT(seq, i);
	    SET_NAMEDCNT_MAX(value);
	    break;
	  case LISTSXP:
	    value = CAR(seq);
	    SETSTACK(-4, CDR(seq));
	    SET_NAMEDCNT_MAX(value);
	    break;
	  default:
	    error(_("invalid sequence argument in for loop"));
	  }
	  if (! SET_BINDING_VALUE(cell, value))
	      defineVar(BINDING_SYMBOL(cell), value, rho);
	  BC_CHECK_SIGINT();
	  pc = codebase + label;
	}
	NEXT();
      }
    OP(ENDFOR, 0):
      {
	R_BCNodeStackTop -= 3;
	SETSTACK(-1, R_NilValue);
	NEXT();
      }
    OP(SETLOOPVAL, 0):
      BCNPOP_IGNORE_VALUE(); SETSTACK(-1, R_NilValue); NEXT();
    OP(INVISIBLE,0): R_Visible = FALSE; NEXT();
    /**** for now LDCONST, LDTRUE, and LDFALSE duplicate/allocate to
	  be defensive against bad package C code */
    OP(LDCONST, 1):
      R_Visible = TRUE;
      value = constants[GETOP()];
      /* make sure NAMED = 2 -- lower values might be safe in some cases but
	 not in general, especially if the constant pool was created by
	 unserializing a compiled expression. */
      /*if (NAMED(value) < 2) SET_NAMED(value, 2);*/
      BCNPUSH(duplicate(value));
      NEXT();
    OP(LDNULL, 0): R_Visible = TRUE; BCNPUSH(R_NilValue); NEXT();
    OP(LDTRUE, 0): R_Visible = TRUE; BCNPUSH(mkTrue()); NEXT();
    OP(LDFALSE, 0): R_Visible = TRUE; BCNPUSH(mkFalse()); NEXT();
    OP(GETVAR, 1): DO_GETVAR(FALSE, FALSE);
    OP(DDVAL, 1): DO_GETVAR(TRUE, FALSE);
    OP(SETVAR, 1):
      {
	int sidx = GETOP();
	SEXP loc;
	if (smallcache)
	    loc = GET_SMALLCACHE_BINDING_CELL(vcache, sidx);
	else {
	    SEXP symbol = constants[sidx];
	    loc = GET_BINDING_CELL_CACHE(symbol, rho, vcache, sidx);
	}
	value = GETSTACK(-1);
        INC_NAMEDCNT(value);
	if (! SET_BINDING_VALUE(loc, value)) {
	    SEXP symbol = constants[sidx];
	    PROTECT(value);
	    defineVar(symbol, value, rho);
	    UNPROTECT(1);
	}
	NEXT();
      }
    OP(GETFUN, 1):
      {
	/* get the function */
	SEXP symbol = constants[GETOP()];
	value = findFun(symbol, rho);
	if(RTRACE(value)) {
            Rprintf("trace: ");
            PrintValue(symbol);
	}

	/* initialize the function type register, push the function, and
	   push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	SETSTACK(0, value);
	SETSTACK(1, R_NilValue);
	SETSTACK(2, R_NilValue);
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETGLOBFUN, 1):
      {
	/* get the function */
	SEXP symbol = constants[GETOP()];
	value = findFun(symbol, R_GlobalEnv);
	if(RTRACE(value)) {
            Rprintf("trace: ");
            PrintValue(symbol);
	}

	/* initialize the function type register, push the function, and
	   push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	SETSTACK(0, value);
	SETSTACK(1, R_NilValue);
	SETSTACK(2, R_NilValue);
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETSYMFUN, 1):
      {
	/* get the function */
	SEXP symbol = constants[GETOP()];
	value = SYMVALUE(symbol);
	if (TYPEOF(value) == PROMSXP) {
	    value = forcePromise(value);
	    SET_NAMEDCNT_MAX(value);
	}
	if(RTRACE(value)) {
            Rprintf("trace: ");
            PrintValue(symbol);
	}

	/* initialize the function type register, push the function, and
	   push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	SETSTACK(0, value);
	SETSTACK(1, R_NilValue);
	SETSTACK(2, R_NilValue);
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETBUILTIN, 1):
      {
	/* get the function */
	SEXP symbol = constants[GETOP()];
	value = getPrimitive(symbol, BUILTINSXP);
	if (RTRACE(value)) {
            Rprintf("trace: ");
            PrintValue(symbol);
	}

	/* push the function and push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	SETSTACK(0, value);
	SETSTACK(1, R_NilValue);
	SETSTACK(2, R_NilValue);
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(GETINTLBUILTIN, 1):
      {
	/* get the function */
	SEXP symbol = constants[GETOP()];
	value = INTERNAL(symbol);
	if (TYPEOF(value) != BUILTINSXP)
	  error(_("there is no .Internal function '%s'"), 
		CHAR(PRINTNAME(symbol)));

	/* push the function and push space for creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(3);
	SETSTACK(0, value);
	SETSTACK(1, R_NilValue);
	SETSTACK(2, R_NilValue);
	R_BCNodeStackTop += 3;
	NEXT();
      }
    OP(CHECKFUN, 0):
      {
	/* check then the value on the stack is a function */
	value = GETSTACK(-1);
	if (TYPEOF(value) != CLOSXP && TYPEOF(value) != BUILTINSXP &&
	    TYPEOF(value) != SPECIALSXP)
	  apply_non_function_error();

	/* initialize the function type register, and push space for
	   creating the argument list. */
	ftype = TYPEOF(value);
	BCNSTACKCHECK(2);
	SETSTACK(0, R_NilValue);
	SETSTACK(1, R_NilValue);
	R_BCNodeStackTop += 2;
	NEXT();
      }
    OP(MAKEPROM, 1):
      {
	SEXP code = constants[GETOP()];
	if (ftype != SPECIALSXP) {
	  if (ftype == BUILTINSXP)
	      value = bcEval(code, rho, TRUE);
	  else
	    value = mkPROMISE(code, rho);
	  PUSHCALLARG(value);
	}
	NEXT();
      }
    OP(DOMISSING, 0):
      {
	if (ftype != SPECIALSXP)
	  PUSHCALLARG(R_MissingArg);
	NEXT();
      }
    OP(SETTAG, 1):
      {
	SEXP tag = constants[GETOP()];
	SEXP cell = GETSTACK(-1);
	if (ftype != SPECIALSXP && cell != R_NilValue)
	  SET_TAG(cell, CreateTag(tag));
	NEXT();
      }
    OP(DODOTS, 0):
      {
	if (ftype != SPECIALSXP) {
	  SEXP h = findVar(R_DotsSymbol, rho);
	  if (TYPEOF(h) == DOTSXP || h == R_NilValue) {
	    for (; h != R_NilValue; h = CDR(h)) {
	      SEXP val, cell;
	      if (ftype == BUILTINSXP) val = eval(CAR(h), rho);
	      else val = mkPROMISE(CAR(h), rho);
	      cell = CONS(val, R_NilValue);
	      PUSHCALLARG_CELL(cell);
	      if (TAG(h) != R_NilValue) SET_TAG(cell, CreateTag(TAG(h)));
	    }
	  }
	  else if (h != R_MissingArg)
	    dotdotdot_error();
	}
	NEXT();
      }
    OP(PUSHARG, 0): PUSHCALLARG(BCNPOP()); NEXT();
    /**** for now PUSHCONST, PUSHTRUE, and PUSHFALSE duplicate/allocate to
	  be defensive against bad package C code */
    OP(PUSHCONSTARG, 1):
      value = constants[GETOP()];
      PUSHCALLARG(duplicate(value));
      NEXT();
    OP(PUSHNULLARG, 0): PUSHCALLARG(R_NilValue); NEXT();
    OP(PUSHTRUEARG, 0): PUSHCALLARG(mkTrue()); NEXT();
    OP(PUSHFALSEARG, 0): PUSHCALLARG(mkFalse()); NEXT();
    OP(CALL, 1):
      {
	SEXP fun = GETSTACK(-3);
	SEXP call = constants[GETOP()];
	SEXP args = GETSTACK(-2);
	int flag;
	switch (ftype) {
	case BUILTINSXP:
	  checkForMissings(args, call);
	  flag = PRIMPRINT(fun);
	  R_Visible = flag != 1;
          value = CALL_PRIMFUN(call, fun, args, rho, 0);
	  if (flag < 2) R_Visible = flag != 1;
	  break;
	case SPECIALSXP:
	  flag = PRIMPRINT(fun);
	  R_Visible = flag != 1;
          value = CALL_PRIMFUN(call, fun, CDR(call), rho, 0);
	  if (flag < 2) R_Visible = flag != 1;
	  break;
	case CLOSXP:
	  value = applyClosure_v(call, fun, args, rho, R_NoObject, 0);
	  break;
	default: bad_function_error();
	}
	R_BCNodeStackTop -= 2;
	SETSTACK(-1, value);
	ftype = 0;
	NEXT();
      }
    OP(CALLBUILTIN, 1):
      {
	SEXP fun = GETSTACK(-3);
	SEXP call = constants[GETOP()];
	SEXP args = GETSTACK(-2);
	int flag;
	const void *vmax = VMAXGET();
	if (TYPEOF(fun) != BUILTINSXP)
          error(_("not a BUILTIN function"));
	flag = PRIMPRINT(fun);
	R_Visible = flag != 1;
        value = CALL_PRIMFUN(call, fun, args, rho, 0);
	if (flag < 2) R_Visible = flag != 1;
	VMAXSET(vmax);
	R_BCNodeStackTop -= 2;
	SETSTACK(-1, value);
	NEXT();
      }
    OP(CALLSPECIAL, 1):
      {
	SEXP call = constants[GETOP()];
	SEXP symbol = CAR(call);
	SEXP fun = getPrimitive(symbol, SPECIALSXP);
	int flag;
	const void *vmax = VMAXGET();
	if (RTRACE(fun)) {
            Rprintf("trace: ");
            PrintValue(symbol);
	}
	BCNPUSH(fun);  /* for GC protection */
	flag = PRIMPRINT(fun);
	R_Visible = flag != 1;
        value = CALL_PRIMFUN(call, fun, CDR(call), rho, 0);
	if (flag < 2) R_Visible = flag != 1;
	VMAXSET(vmax);
	SETSTACK(-1, value); /* replaces fun on stack */
	NEXT();
      }
    OP(MAKECLOSURE, 1):
      {
	SEXP fb = constants[GETOP()];
	SEXP forms = VECTOR_ELT(fb, 0);
	SEXP body = VECTOR_ELT(fb, 1);
	value = mkCLOSXP(forms, body, rho);
	BCNPUSH(value);
	NEXT();
      }
    OP(UMINUS, 1): Arith1(R_SubSymbol);
    OP(UPLUS, 1): Arith1(R_AddSymbol);
    OP(ADD, 1): FastBinary(+, PLUSOP, R_AddSymbol);
    OP(SUB, 1): FastBinary(-, MINUSOP, R_SubSymbol);
    OP(MUL, 1): FastBinary(*, TIMESOP, R_MulSymbol);
    OP(DIV, 1): FastBinary(/, DIVOP, R_DivSymbol);
    OP(EXPT, 1): Arith2(POWOP, R_ExptSymbol);
    OP(SQRT, 1): Math1(R_SqrtSymbol);
    OP(EXP, 1): Math1(R_ExpSymbol);
    OP(EQ, 1): FastRelop2(==, EQOP, R_EqSymbol);
    OP(NE, 1): FastRelop2(!=, NEOP, R_NeSymbol);
    OP(LT, 1): FastRelop2(<, LTOP, R_LtSymbol);
    OP(LE, 1): FastRelop2(<=, LEOP, R_LeSymbol);
    OP(GE, 1): FastRelop2(>=, GEOP, R_GeSymbol);
    OP(GT, 1): FastRelop2(>, GTOP, R_GtSymbol);
    OP(AND, 1): Special2(do_andor, R_AndSymbol, rho);
    OP(OR, 1): Special2(do_andor, R_OrSymbol, rho);
    OP(NOT, 1): Builtin1(do_not, R_NotSymbol, rho);
    OP(DOTSERR, 0): dotdotdot_error();
    OP(STARTASSIGN, 1):
      {
	int sidx = GETOP();
	SEXP symbol = constants[sidx];
	SEXP cell = GET_BINDING_CELL_CACHE(symbol, rho, vcache, sidx);
	value = BINDING_VALUE(cell);
        if (TYPEOF(value) == PROMSXP)
            value = forcePromise(value);
	if (value == R_UnboundValue || NAMEDCNT(value) != 1) {
            /* Used to call EnsureLocal, now changed, so old code is here. */
            value = findVarInFrame3 (rho, symbol, TRUE);
            if (value != R_UnboundValue) {
                if (TYPEOF(value) == PROMSXP)
                    value = forcePromise(value);
        	if (!NAMEDCNT_GT_1(value)) 
                    goto in_frame;
            }
            else {
                if (rho != R_EmptyEnv) {
                    value = findVar (symbol, ENCLOS(rho));
                    if (TYPEOF(value) == PROMSXP)
                        value = forcePromise(value);
                }
                if (value == R_UnboundValue)
                    unbound_var_error(symbol);
            }
            value = dup_top_level(value);
            set_var_in_frame (symbol, value, rho, TRUE, 3);
          in_frame: ;
        }
	BCNPUSH(value);
	BCNDUP2ND();
	/* top three stack entries are now RHS value, LHS value, RHS value */
	NEXT();
      }
    OP(ENDASSIGN, 1):
      {
	int sidx = GETOP();
	SEXP symbol = constants[sidx];
	SEXP cell = GET_BINDING_CELL_CACHE(symbol, rho, vcache, sidx);
	value = GETSTACK(-1); /* leave on stack for GC protection */
        INC_NAMEDCNT(value);
	if (! SET_BINDING_VALUE(cell, value))
	    defineVar(symbol, value, rho);
	R_BCNodeStackTop--; /* now pop LHS value off the stack */
	/* original right-hand side value is now on top of stack again */
	/* we do not duplicate the right-hand side value, so to be
	   conservative mark the value as NAMED = 2 */
	SET_NAMEDCNT_MAX(GETSTACK(-1));
	NEXT();
      }
    OP(STARTSUBSET, 2): DO_STARTDISPATCH("[");
    OP(DFLTSUBSET, 0): DO_DFLTDISPATCH(do_subset_dflt, R_BracketSymbol);
    OP(STARTSUBASSIGN, 2): DO_START_ASSIGN_DISPATCH("[<-");
    OP(DFLTSUBASSIGN, 0):
      DO_DFLT_ASSIGN_DISPATCH(do_subassign_dflt, R_SubAssignSymbol);
    OP(STARTC, 2): DO_STARTDISPATCH("c");
    OP(DFLTC, 0): DO_DFLTDISPATCH(do_c_dflt, R_CSymbol);
    OP(STARTSUBSET2, 2): DO_STARTDISPATCH("[[");
    OP(DFLTSUBSET2, 0): DO_DFLTDISPATCH0(do_subset2_dflt, R_Bracket2Symbol);
    OP(STARTSUBASSIGN2, 2): DO_START_ASSIGN_DISPATCH("[[<-");
    OP(DFLTSUBASSIGN2, 0):
      DO_DFLT_ASSIGN_DISPATCH(do_subassign2_dflt, R_SubSubAssignSymbol);
    OP(DOLLAR, 2):
      {
	int dispatched = FALSE;
	SEXP call = constants[GETOP()];
	SEXP symbol = constants[GETOP()];
	SEXP x = GETSTACK(-1);
	if (isObject(x)) {
	    SEXP ncall;
	    PROTECT(ncall = duplicate(call));
	    /**** hack to avoid evaluating the symbol */
	    SETCAR(CDDR(ncall), ScalarString(PRINTNAME(symbol)));
	    dispatched = tryDispatch("$", ncall, x, rho, &value);
	    UNPROTECT(1);
	}
	if (dispatched)
	    SETSTACK(-1, value);
	else
	    SETSTACK(-1, R_subset3_dflt(x, R_NilValue, symbol, R_NilValue, 0));
	NEXT();
      }
    OP(DOLLARGETS, 2):
      {
	int dispatched = FALSE;
	SEXP call = constants[GETOP()];
	SEXP symbol = constants[GETOP()];
	SEXP x = GETSTACK(-2);
	SEXP rhs = GETSTACK(-1);
	if (NAMEDCNT_GT_1(x) && x != R_NilValue) {
	    x = duplicate(x);
	    SETSTACK(-2, x);
	    SET_NAMEDCNT_1(x);
	}
	if (isObject(x)) {
	    SEXP ncall, prom;
	    PROTECT(ncall = duplicate(call));
	    /**** hack to avoid evaluating the symbol */
	    SETCAR(CDDR(ncall), ScalarString(PRINTNAME(symbol)));
	    prom = mkPROMISE(CADDDR(ncall), rho);
	    SET_PRVALUE(prom, rhs);
            INC_NAMEDCNT(rhs);
	    SETCAR(CDR(CDDR(ncall)), prom);
	    dispatched = tryDispatch("$<-", ncall, x, rho, &value);
	    UNPROTECT(1);
	}
	if (! dispatched)
	    value = R_subassign3_dflt(call, x, symbol, rhs);
	R_BCNodeStackTop--;
	SETSTACK(-1, value);
	NEXT();
      }
    OP(ISNULL, 0): DO_ISTEST(isNull);
    OP(ISLOGICAL, 0): DO_ISTYPE(LGLSXP);
    OP(ISINTEGER, 0): {
	SEXP arg = GETSTACK(-1);
	Rboolean test = (TYPEOF(arg) == INTSXP) && ! inherits(arg, "factor");
	SETSTACK(-1, test ? mkTrue() : mkFalse());
	NEXT();
      }
    OP(ISDOUBLE, 0): DO_ISTYPE(REALSXP);
    OP(ISCOMPLEX, 0): DO_ISTYPE(CPLXSXP);
    OP(ISCHARACTER, 0): DO_ISTYPE(STRSXP);
    OP(ISSYMBOL, 0): DO_ISTYPE(SYMSXP); /**** S4 thingy allowed now???*/
    OP(ISOBJECT, 0): DO_ISTEST(OBJECT);
    OP(ISNUMERIC, 0): DO_ISTEST(isNumericOnly);
    OP(VECSUBSET, 0): DO_VECSUBSET(rho); NEXT();
    OP(MATSUBSET, 0): DO_MATSUBSET(rho); NEXT();
    OP(SETVECSUBSET, 0): DO_SETVECSUBSET(rho); NEXT();
    OP(SETMATSUBSET, 0): DO_SETMATSUBSET(rho); NEXT();
    OP(AND1ST, 2): {
	int callidx = GETOP();
	int label = GETOP();
        FIXUP_SCALAR_LOGICAL(callidx, "'x'", "&&");
        value = GETSTACK(-1);
	if (LOGICAL(value)[0] == FALSE)
	    pc = codebase + label;
	NEXT();
    }
    OP(AND2ND, 1): {
	int callidx = GETOP();
	FIXUP_SCALAR_LOGICAL(callidx, "'y'", "&&");
        value = GETSTACK(-1);
	/* The first argument is TRUE or NA. If the second argument is
	   not TRUE then its value is the result. If the second
	   argument is TRUE, then the first argument's value is the
	   result. */
	if (LOGICAL(value)[0] != TRUE)
	    SETSTACK(-2, value);
	R_BCNodeStackTop -= 1;
	NEXT();
    }
    OP(OR1ST, 2):  {
	int callidx = GETOP();
	int label = GETOP();
        FIXUP_SCALAR_LOGICAL(callidx, "'x'", "||");
        value = GETSTACK(-1);
	if (LOGICAL(value)[0] != NA_LOGICAL && LOGICAL(value)[0]) /* is true */
	    pc = codebase + label;
	NEXT();
    }
    OP(OR2ND, 1):  {
	int callidx = GETOP();
	FIXUP_SCALAR_LOGICAL(callidx, "'y'", "||");
        value = GETSTACK(-1);
	/* The first argument is FALSE or NA. If the second argument is
	   not FALSE then its value is the result. If the second
	   argument is FALSE, then the first argument's value is the
	   result. */
	if (LOGICAL(value)[0] != FALSE)
	    SETSTACK(-2, value);
	R_BCNodeStackTop -= 1;
	NEXT();
    }
    OP(GETVAR_MISSOK, 1): DO_GETVAR(FALSE, TRUE);
    OP(DDVAL_MISSOK, 1): DO_GETVAR(TRUE, TRUE);
    OP(VISIBLE, 0): R_Visible = TRUE; NEXT();
    OP(SETVAR2, 1):
      {
	SEXP symbol = constants[GETOP()];
	value = GETSTACK(-1);
	if (NAMEDCNT_GT_0(value)) {
	    value = duplicate(value);
	    SETSTACK(-1, value);
	}
	set_var_nonlocal (symbol, value, ENCLOS(rho), 3);
	NEXT();
      }
    OP(STARTASSIGN2, 1):
      {
	SEXP symbol = constants[GETOP()];
	value = GETSTACK(-1);
	BCNPUSH(getvar(symbol, ENCLOS(rho), FALSE, NULL, 0));
	BCNPUSH(value);
	/* top three stack entries are now RHS value, LHS value, RHS value */
	NEXT();
      }
    OP(ENDASSIGN2, 1):
      {
	SEXP symbol = constants[GETOP()];
	value = BCNPOP();
	set_var_nonlocal (symbol, value, ENCLOS(rho), 3);
	/* original right-hand side value is now on top of stack again */
	/* we do not duplicate the right-hand side value, so to be
	   conservative mark the value as NAMED = 2 */
	SET_NAMEDCNT_MAX(GETSTACK(-1));
	NEXT();
      }
    OP(SETTER_CALL, 2):
      {
        SEXP lhs = GETSTACK(-5);
        SEXP rhs = GETSTACK(-4);
	SEXP fun = GETSTACK(-3);
	SEXP call = constants[GETOP()];
	SEXP vexpr = constants[GETOP()];
	SEXP args, prom, last;
	if (NAMEDCNT_GT_1(lhs) && lhs != R_NilValue) {
	  lhs = duplicate(lhs);
	  SETSTACK(-5, lhs);
	  SET_NAMEDCNT_1(lhs);
	}
	switch (ftype) {
	case BUILTINSXP:
	  /* push RHS value onto arguments with 'value' tag */
	  PUSHCALLARG(rhs);
	  SET_TAG(GETSTACK(-1), R_ValueSymbol);
	  /* replace first argument with LHS value */
	  args = GETSTACK(-2);
	  SETCAR(args, lhs);
	  /* make the call */
	  checkForMissings(args, call);
          value = CALL_PRIMFUN(call, fun, args, rho, 0);
	  break;
	case SPECIALSXP:
	  /* duplicate arguments and put into stack for GC protection */
	  args = duplicate(CDR(call));
	  SETSTACK(-2, args);
	  /* insert evaluated promise for LHS as first argument */
          prom = mkPROMISE(R_TmpvalSymbol, rho);
	  SET_PRVALUE(prom, lhs);
          INC_NAMEDCNT(lhs);
	  SETCAR(args, prom);
	  /* insert evaluated promise for RHS as last argument */
	  last = args;
	  while (CDR(last) != R_NilValue)
	      last = CDR(last);
	  prom = mkPROMISE(vexpr, rho);
	  SET_PRVALUE(prom, rhs);
          INC_NAMEDCNT(rhs);
	  SETCAR(last, prom);
	  /* make the call */
          value = CALL_PRIMFUN(call, fun, args, rho, 0);
	  break;
	case CLOSXP:
	  /* push evaluated promise for RHS onto arguments with 'value' tag */
	  prom = mkPROMISE(vexpr, rho);
	  SET_PRVALUE(prom, rhs);
          INC_NAMEDCNT(rhs);
	  PUSHCALLARG(prom);
	  SET_TAG(GETSTACK(-1), R_ValueSymbol);
	  /* replace first argument with evaluated promise for LHS */
          prom = mkPROMISE(R_TmpvalSymbol, rho);
	  SET_PRVALUE(prom, lhs);
          INC_NAMEDCNT(lhs);
	  args = GETSTACK(-2);
	  SETCAR(args, prom);
	  /* make the call */
	  value = applyClosure_v(call, fun, args, rho, R_NoObject, 0);
	  break;
	default: bad_function_error();
	}
	R_BCNodeStackTop -= 4;
	SETSTACK(-1, value);
	ftype = 0;
	NEXT();
      }
    OP(GETTER_CALL, 1):
      {
	SEXP lhs = GETSTACK(-5);
	SEXP fun = GETSTACK(-3);
	SEXP call = constants[GETOP()];
	SEXP args, prom;
	switch (ftype) {
	case BUILTINSXP:
	  /* replace first argument with LHS value */
	  args = GETSTACK(-2);
	  SETCAR(args, lhs);
	  /* make the call */
	  checkForMissings(args, call);
          value = CALL_PRIMFUN(call, fun, args, rho, 0);
	  break;
	case SPECIALSXP:
	  /* duplicate arguments and put into stack for GC protection */
	  args = duplicate(CDR(call));
	  SETSTACK(-2, args);
	  /* insert evaluated promise for LHS as first argument */
          prom = mkPROMISE(R_TmpvalSymbol, rho);
	  SET_PRVALUE(prom, lhs);
          INC_NAMEDCNT(lhs);
	  SETCAR(args, prom);
	  /* make the call */
          value = CALL_PRIMFUN(call, fun, args, rho, 0);
	  break;
	case CLOSXP:
	  /* replace first argument with evaluated promise for LHS */
          prom = mkPROMISE(R_TmpvalSymbol, rho);
	  SET_PRVALUE(prom, lhs);
          INC_NAMEDCNT(lhs);
	  args = GETSTACK(-2);
	  SETCAR(args, prom);
	  /* make the call */
	  value = applyClosure_v(call, fun, args, rho, R_NoObject, 0);
	  break;
	default: bad_function_error();
	}
	R_BCNodeStackTop -= 2;
	SETSTACK(-1, value);
	ftype = 0;
	NEXT();
      }
    OP(SWAP, 0): {
	R_bcstack_t tmp = R_BCNodeStackTop[-1];
	R_BCNodeStackTop[-1] = R_BCNodeStackTop[-2];
	R_BCNodeStackTop[-2] = tmp;
	NEXT();
    }
    OP(DUP2ND, 0): BCNDUP2ND(); NEXT();
    OP(SWITCH, 4): {
       SEXP call = constants[GETOP()];
       SEXP names = constants[GETOP()];
       SEXP coffsets = constants[GETOP()];
       SEXP ioffsets = constants[GETOP()];
       value = BCNPOP();
       if (!isVector(value) || length(value) != 1)
	   errorcall(call, _("EXPR must be a length 1 vector"));
       if (TYPEOF(value) == STRSXP) {
	   int i, n, which;
	   if (names == R_NilValue)
	       errorcall(call, _("numeric EXPR required for switch() "
				 "without named alternatives"));
	   if (TYPEOF(coffsets) != INTSXP)
	       errorcall(call, _("bad character switch offsets"));
	   if (TYPEOF(names) != STRSXP || LENGTH(names) != LENGTH(coffsets))
	       errorcall(call, _("bad switch names"));
	   n = LENGTH(names);
	   which = n - 1;
	   for (i = 0; i < n - 1; i++)
	       if (ep_match_exprs(STRING_ELT(value, 0),
			          STRING_ELT(names, i))==1 /* exact */) {
		   which = i;
		   break;
	       }
	   pc = codebase + INTEGER(coffsets)[which];
       }
       else {
	   int which = asInteger(value) - 1;
	   if (TYPEOF(ioffsets) != INTSXP)
	       errorcall(call, _("bad numeric switch offsets"));
	   if (which < 0 || which >= LENGTH(ioffsets))
	       which = LENGTH(ioffsets) - 1;
	   pc = codebase + INTEGER(ioffsets)[which];
       }
       NEXT();
    }
    OP(RETURNJMP, 0): {
      value = BCNPOP();
      findcontext(CTXT_BROWSER | CTXT_FUNCTION, rho, value);
    }
    OP(STARTVECSUBSET, 2): DO_STARTDISPATCH_N("[");
    OP(STARTMATSUBSET, 2): DO_STARTDISPATCH_N("[");
    OP(STARTSETVECSUBSET, 2): DO_START_ASSIGN_DISPATCH_N("[<-");
    OP(STARTSETMATSUBSET, 2): DO_START_ASSIGN_DISPATCH_N("[<-");
    LASTOP;
  }

 done:
  R_BCNodeStackTop = oldntop;
#ifdef BC_INT_STACK
  R_BCIntStackTop = olditop;
#endif
#ifdef BC_PROFILING
  current_opcode = old_current_opcode;
#endif
  return value;
}

#ifdef THREADED_CODE
SEXP R_bcEncode(SEXP bytes)
{
    SEXP code;
    BCODE *pc;
    int *ipc, i, n, m, v;

    m = (sizeof(BCODE) + sizeof(int) - 1) / sizeof(int);

    n = LENGTH(bytes);
    ipc = INTEGER(bytes);

    v = ipc[0];
    if (v < R_bcMinVersion || v > R_bcVersion) {
	code = allocVector(INTSXP, m * 2);
	pc = (BCODE *) INTEGER(code);
	pc[0].i = v;
	pc[1].v = opinfo[BCMISMATCH_OP].addr;
	return code;
    }
    else {
	code = allocVector(INTSXP, m * n);
	pc = (BCODE *) INTEGER(code);

	for (i = 0; i < n; i++) pc[i].i = ipc[i];

	/* install the current version number */
	pc[0].i = R_bcVersion;

	for (i = 1; i < n;) {
	    int op = pc[i].i;
	    if (op < 0 || op >= OPCOUNT)
		error("unknown instruction code");
	    pc[i].v = opinfo[op].addr;
	    i += opinfo[op].argc + 1;
	}

	return code;
    }
}

static int findOp(void *addr)
{
    int i;

    for (i = 0; i < OPCOUNT; i++)
	if (opinfo[i].addr == addr)
	    return i;
    error(_("cannot find index for threaded code address"));
}

SEXP R_bcDecode(SEXP code) {
    int n, i, j, *ipc;
    BCODE *pc;
    SEXP bytes;

    int m = (sizeof(BCODE) + sizeof(int) - 1) / sizeof(int);

    n = LENGTH(code) / m;
    pc = (BCODE *) INTEGER(code);

    bytes = allocVector(INTSXP, n);
    ipc = INTEGER(bytes);

    /* copy the version number */
    ipc[0] = pc[0].i;

    for (i = 1; i < n;) {
	int op = findOp(pc[i].v);
	int argc = opinfo[op].argc;
	ipc[i] = op;
	i++;
	for (j = 0; j < argc; j++, i++)
	    ipc[i] = pc[i].i;
    }

    return bytes;
}
#else
SEXP R_bcEncode(SEXP x) { return x; }
SEXP R_bcDecode(SEXP x) { return duplicate(x); }
#endif

static SEXP do_mkcode(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP bytes, consts, ans;

    checkArity(op, args);
    bytes = CAR(args);
    consts = CADR(args);
    ans = CONS(R_bcEncode(bytes), consts);
    SET_TYPEOF(ans, BCODESXP);
    return ans;
}

static SEXP do_bcclose(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP forms, body, env;

    checkArity(op, args);
    forms = CAR(args);
    body = CADR(args);
    env = CADDR(args);

    CheckFormals(forms);

    if (! isByteCode(body))
	errorcall(call, _("invalid body"));

    if (isNull(env)) {
	error(_("use of NULL environment is defunct"));
	env = R_BaseEnv;
    } else
    if (!isEnvironment(env))
	errorcall(call, _("invalid environment"));

    return mkCLOSXP(forms, body, env);
}

static SEXP do_is_builtin_internal(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    SEXP symbol, i;

    checkArity(op, args);
    symbol = CAR(args);

    if (!isSymbol(symbol))
	errorcall(call, _("invalid symbol"));

    if ((i = INTERNAL(symbol)) != R_NilValue && TYPEOF(i) == BUILTINSXP)
	return R_TrueValue;
    else
	return R_FalseValue;
}

static SEXP disassemble(SEXP bc)
{
  SEXP ans, dconsts;
  int i;
  SEXP code = BCODE_CODE(bc);
  SEXP consts = BCODE_CONSTS(bc);
  SEXP expr = BCODE_EXPR(bc);
  int nc = LENGTH(consts);

  PROTECT(ans = allocVector(VECSXP, expr != R_NilValue ? 4 : 3));
  SET_VECTOR_ELT(ans, 0, install(".Code"));
  SET_VECTOR_ELT(ans, 1, R_bcDecode(code));
  SET_VECTOR_ELT(ans, 2, allocVector(VECSXP, nc));
  if (expr != R_NilValue)
      SET_VECTOR_ELT(ans, 3, duplicate(expr));

  dconsts = VECTOR_ELT(ans, 2);
  for (i = 0; i < nc; i++) {
    SEXP c = VECTOR_ELT(consts, i);
    if (isByteCode(c))
      SET_VECTOR_ELT(dconsts, i, disassemble(c));
    else
      SET_VECTOR_ELT(dconsts, i, duplicate(c));
  }

  UNPROTECT(1);
  return ans;
}

static SEXP do_disassemble(SEXP call, SEXP op, SEXP args, SEXP rho)
{
  SEXP code;

  checkArity(op, args);
  code = CAR(args);
  if (! isByteCode(code))
    errorcall(call, _("argument is not a byte code object"));
  return disassemble(code);
}

static SEXP do_bcversion(SEXP call, SEXP op, SEXP args, SEXP rho)
{
  SEXP ans = allocVector1INT();
  INTEGER(ans)[0] = R_bcVersion;
  return ans;
}

static SEXP do_loadfile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP file, s;
    FILE *fp;

    checkArity(op, args);

    PROTECT(file = coerceVector(CAR(args), STRSXP));

    if (! isValidStringF(file))
	errorcall(call, _("bad file name"));

    fp = RC_fopen(STRING_ELT(file, 0), "rb", TRUE);
    if (!fp)
	errorcall(call, _("unable to open 'file'"));
    s = R_LoadFromFile(fp, 0);
    fclose(fp);

    UNPROTECT(1);
    return s;
}

static SEXP do_savefile(SEXP call, SEXP op, SEXP args, SEXP env)
{
    FILE *fp;

    checkArity(op, args);

    if (!isValidStringF(CADR(args)))
	errorcall(call, _("'file' must be non-empty string"));
    if (TYPEOF(CADDR(args)) != LGLSXP)
	errorcall(call, _("'ascii' must be logical"));

    fp = RC_fopen(STRING_ELT(CADR(args), 0), "wb", TRUE);
    if (!fp)
	errorcall(call, _("unable to open 'file'"));

    R_SaveToFileV(CAR(args), fp, INTEGER(CADDR(args))[0], 0);

    fclose(fp);
    return R_NilValue;
}

#define R_COMPILED_EXTENSION ".Rc"

/* neither of these functions call R_ExpandFileName -- the caller
   should do that if it wants to */
char *R_CompiledFileName(char *fname, char *buf, size_t bsize)
{
    char *basename, *ext;

    /* find the base name and the extension */
    basename = Rf_strrchr(fname, FILESEP[0]);
    if (basename == NULL) basename = fname;
    ext = Rf_strrchr(basename, '.');

    if (ext != NULL && strcmp(ext, R_COMPILED_EXTENSION) == 0) {
	/* the supplied file name has the compiled file extension, so
	   just copy it to the buffer and return the buffer pointer */
	if (snprintf(buf, bsize, "%s", fname) < 0)
	    error(_("R_CompiledFileName: buffer too small"));
	return buf;
    }
    else if (ext == NULL) {
	/* if the requested file has no extention, make a name that
	   has the extenrion added on to the expanded name */
	if (snprintf(buf, bsize, "%s%s", fname, R_COMPILED_EXTENSION) < 0)
	    error(_("R_CompiledFileName: buffer too small"));
	return buf;
    }
    else {
	/* the supplied file already has an extention, so there is no
	   corresponding compiled file name */
	return NULL;
    }
}

FILE *R_OpenCompiledFile(char *fname, char *buf, size_t bsize)
{
    char *cname = R_CompiledFileName(fname, buf, bsize);

    if (cname != NULL && R_FileExists(cname) &&
	(strcmp(fname, cname) == 0 ||
	 ! R_FileExists(fname) ||
	 R_FileMtime(cname) > R_FileMtime(fname)))
	/* the compiled file cname exists, and either fname does not
	   exist, or it is the same as cname, or both exist and cname
	   is newer */
	return R_fopen(buf, "rb");
    else return NULL;
}

static SEXP do_growconst(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP constBuf, ans;
    int i, n;

    checkArity(op, args);
    constBuf = CAR(args);
    if (TYPEOF(constBuf) != VECSXP)
	error(_("constant buffer must be a generic vector"));

    n = LENGTH(constBuf);
    ans = allocVector(VECSXP, 2 * n);
    for (i = 0; i < n; i++)
	SET_VECTOR_ELT(ans, i, VECTOR_ELT(constBuf, i));

    return ans;
}

static SEXP do_putconst(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP constBuf, x;
    int i, constCount;

    checkArity(op, args);

    constBuf = CAR(args);
    if (TYPEOF(constBuf) != VECSXP)
	error(_("constBuf must be a generic vector"));

    constCount = asInteger(CADR(args));
    if (constCount < 0 || constCount >= LENGTH(constBuf))
	error(_("bad constCount value"));

    x = CADDR(args);

    /* check for a match and return index if one is found */
    for (i = 0; i < constCount; i++) {
	SEXP y = VECTOR_ELT(constBuf, i);
	if (x == y || R_compute_identical(x, y, 0))
	    return ScalarInteger(i);
    }

    /* otherwise insert the constant and return index */
    SET_VECTOR_ELT(constBuf, constCount, x);
    return ScalarInteger(constCount);
}

static SEXP do_getconst(SEXP call, SEXP op, SEXP args, SEXP env)
{
    SEXP constBuf, ans;
    int i, n;

    checkArity(op, args);
    constBuf = CAR(args);
    n = asInteger(CADR(args));

    if (TYPEOF(constBuf) != VECSXP)
	error(_("constant buffer must be a generic vector"));
    if (n < 0 || n > LENGTH(constBuf))
	error(_("bad constant count"));

    ans = allocVector(VECSXP, n);
    for (i = 0; i < n; i++)
	SET_VECTOR_ELT(ans, i, VECTOR_ELT(constBuf, i));

    return ans;
}

#ifdef BC_PROFILING
SEXP R_getbcprofcounts()
{
    SEXP val;
    int i;

    val = allocVector(INTSXP, OPCOUNT);
    for (i = 0; i < OPCOUNT; i++)
	INTEGER(val)[i] = opcode_counts[i];
    return val;
}

static void dobcprof(int sig)
{
    if (current_opcode >= 0 && current_opcode < OPCOUNT)
	opcode_counts[current_opcode]++;
    signal(SIGPROF, dobcprof);
}

SEXP R_startbcprof()
{
    struct itimerval itv;
    int interval;
    double dinterval = 0.02;
    int i;

    if (R_Profiling)
	error(_("profile timer in use"));
    if (bc_profiling)
	error(_("already byte code profiling"));

    /* according to man setitimer, it waits until the next clock
       tick, usually 10ms, so avoid too small intervals here */
    interval = 1e6 * dinterval + 0.5;

    /* initialize the profile data */
    current_opcode = NO_CURRENT_OPCODE;
    for (i = 0; i < OPCOUNT; i++)
	opcode_counts[i] = 0;

    signal(SIGPROF, dobcprof);

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = interval;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = interval;
    if (setitimer(ITIMER_PROF, &itv, NULL) == -1)
	error(_("setting profile timer failed"));

    bc_profiling = TRUE;

    return R_NilValue;
}

static void dobcprof_null(int sig)
{
    signal(SIGPROF, dobcprof_null);
}

SEXP R_stopbcprof()
{
    struct itimerval itv;

    if (! bc_profiling)
	error(_("not byte code profiling"));

    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 0;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 0;
    setitimer(ITIMER_PROF, &itv, NULL);
    signal(SIGPROF, dobcprof_null);

    bc_profiling = FALSE;

    return R_NilValue;
}
#else
SEXP R_getbcprofcounts() { return R_NilValue; }
SEXP R_startbcprof() { return R_NilValue; }
SEXP R_stopbcprof() { return R_NilValue; }
#endif

/* end of byte code section */

static SEXP do_setnumthreads(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int old = R_num_math_threads, new;
    checkArity(op, args);
    new = asInteger(CAR(args));
    if (new >= 0 && new <= R_max_num_math_threads)
	R_num_math_threads = new;
    return ScalarIntegerMaybeConst(old);
}

static SEXP do_setmaxnumthreads(SEXP call, SEXP op, SEXP args, SEXP rho)
{
    int old = R_max_num_math_threads, new;
    checkArity(op, args);
    new = asInteger(CAR(args));
    if (new >= 0) {
	R_max_num_math_threads = new;
	if (R_num_math_threads > R_max_num_math_threads)
	    R_num_math_threads = R_max_num_math_threads;
    }
    return ScalarIntegerMaybeConst(old);
}

/* FUNTAB entries defined in this source file. See names.c for documentation. */

attribute_hidden FUNTAB R_FunTab_eval[] =
{
/* printname	c-entry		offset	eval	arity	pp-kind	     precedence	rightassoc */

{"if",		do_if,		0,	1200,	-1,	{PP_IF,	     PREC_FN,	  1}},
{"for",		do_for,		0,	100,	-1,	{PP_FOR,     PREC_FN,	  0}},
{"while",	do_while,	0,	100,	-1,	{PP_WHILE,   PREC_FN,	  0}},
{"repeat",	do_repeat,	0,	100,	-1,	{PP_REPEAT,  PREC_FN,	  0}},
{"break",	do_break, CTXT_BREAK,	0,	-1,	{PP_BREAK,   PREC_FN,	  0}},
{"next",	do_break, CTXT_NEXT,	0,	-1,	{PP_NEXT,    PREC_FN,	  0}},
{"(",		do_paren,	0,	1000,	1,	{PP_PAREN,   PREC_FN,	  0}},
{"{",		do_begin,	0,	1200,	-1,	{PP_CURLY,   PREC_FN,	  0}},
{"return",	do_return,	0,	1200,	-1,	{PP_RETURN,  PREC_FN,	  0}},
{"function",	do_function,	0,	0,	-1,	{PP_FUNCTION,PREC_FN,	  0}},
{"<-",		do_set,		1,	1100,	2,	{PP_ASSIGN,  PREC_LEFT,	  1}},
{"=",		do_set,		3,	1100,	2,	{PP_ASSIGN,  PREC_EQ,	  1}},
{"<<-",		do_set,		2,	1100,	2,	{PP_ASSIGN2, PREC_LEFT,	  1}},
{"->",		do_set,		11,	1100,	2,	{PP_ASSIGN,  PREC_RIGHT,	  1}},
{"->>",		do_set,		12,	1100,	2,	{PP_ASSIGN2, PREC_RIGHT,	  1}},
{"eval",	do_eval,	0,	1211,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"eval.with.vis",do_eval,	1,	1211,	3,	{PP_FUNCALL, PREC_FN,	0}},
{"Recall",	do_recall,	0,	210,	-1,	{PP_FUNCALL, PREC_FN,	  0}},

{"Rprof",	do_Rprof,	0,	11,	4,	{PP_FUNCALL, PREC_FN,	0}},
{"enableJIT",    do_enablejit,  0,      11,     1,      {PP_FUNCALL, PREC_FN, 0}},
{"compilePKGS", do_compilepkgs, 0,      11,     1,      {PP_FUNCALL, PREC_FN, 0}},
{"withVisible", do_withVisible,	1,	10,	1,	{PP_FUNCALL, PREC_FN,	0}},

{"mkCode",     do_mkcode,       0,      11,     2,      {PP_FUNCALL, PREC_FN, 0}},
{"bcClose",    do_bcclose,      0,      11,     3,      {PP_FUNCALL, PREC_FN, 0}},
{"is.builtin.internal", do_is_builtin_internal, 0, 11, 1, {PP_FUNCALL, PREC_FN, 0}},
{"disassemble", do_disassemble, 0,      11,     1,      {PP_FUNCALL, PREC_FN, 0}},
{"bcVersion", do_bcversion,     0,      11,     0,      {PP_FUNCALL, PREC_FN, 0}},
{"load.from.file", do_loadfile, 0,      11,     1,      {PP_FUNCALL, PREC_FN, 0}},
{"save.to.file", do_savefile,   0,      11,     3,      {PP_FUNCALL, PREC_FN, 0}},
{"growconst", do_growconst,     0,      11,     1,      {PP_FUNCALL, PREC_FN, 0}},
{"putconst", do_putconst,       0,      11,     3,      {PP_FUNCALL, PREC_FN, 0}},
{"getconst", do_getconst,       0,      11,     2,      {PP_FUNCALL, PREC_FN, 0}},

{"setNumMathThreads", do_setnumthreads,      0, 11, 1,  {PP_FUNCALL, PREC_FN, 0}},
{"setMaxNumMathThreads", do_setmaxnumthreads,0, 11, 1,  {PP_FUNCALL, PREC_FN, 0}},

{NULL,		NULL,		0,	0,	0,	{PP_INVALID, PREC_FN,	0}},
};
