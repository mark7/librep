/* timers.c -- call a function after a period of time has passed
   Copyright (C) 1999 John Harper <john@dcs.warwick.ac.uk>
   $Id$

   This file is part of librep.

   librep is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   librep is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with librep; see the file COPYING.	If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.  */

#define _GNU_SOURCE

#include "repint.h"
#include <signal.h>
#include <time.h>
#include <assert.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif

static int timer_type;

#define TIMER(v)  ((Lisp_Timer *)rep_PTR(v))
#define TIMERP(v) rep_CELL16_TYPEP(v, timer_type)

typedef struct lisp_timer {
    repv car;
    struct lisp_timer *next;
    struct lisp_timer *next_alloc;
    repv function;
    long secs, msecs;
    long rel_secs, rel_msecs;
    int fired : 1;
    int deleted : 1;
} Lisp_Timer;

/* List of all allocated timer objects, linked through next_alloc field */
static Lisp_Timer *allocated_timers;

/* List of all pending timers, linked through next field. Only ever
   touch this variable if SIGALRM is blocked! */
static Lisp_Timer *timer_chain;

/* Pipe used to trigger the input callback */
static int pipe_fds[2];

/* Contains SIGALRM */
static sigset_t alrm_sigset;

DEFSYM(timers, "timers");



static RETSIGTYPE
timer_signal_handler (int sig)
{
    int dummy = 0;
    Lisp_Timer *t = timer_chain;
    assert (t != 0);
    t->rel_secs = t->rel_msecs = 0;
    while (t != 0 && t->rel_secs == 0 && t->rel_msecs == 0)
    {
	t->fired = 1;
	t = t->next;
    }
    write (pipe_fds[1], &dummy, sizeof (dummy));
}

/* only call with SIGALRM blocked */
static void
setup_next_timer (void)
{
    if (timer_chain != 0
	&& (timer_chain->rel_secs > 0 || timer_chain->rel_msecs > 0))
    {
#ifdef HAVE_SETITIMER
	struct itimerval it, tem;
	it.it_interval.tv_usec = 0;
	it.it_interval.tv_sec = 0;
	it.it_value.tv_usec = timer_chain->rel_msecs * 1000;
	it.it_value.tv_sec = timer_chain->rel_secs;
	setitimer (ITIMER_REAL, &it, &tem);
#else
	alarm (timer_chain->secs);
#endif
	signal (SIGALRM, timer_signal_handler);
    }
    else
	signal (SIGALRM, SIG_IGN);
}

static inline void
fix_time (long *secs, long *msecs)
{
    while (*msecs < 0)
    {
	*msecs += 1000;
	(*secs)--;
    }
    while (*msecs > 1000)
    {
	*msecs -= 1000;
	(*secs)++;
    }
}

static void
insert_timer (Lisp_Timer *t)
{
    sigset_t old;
    sigprocmask (SIG_BLOCK, &alrm_sigset, &old);
    if (t->secs > 0 || t->msecs > 0)
    {
	Lisp_Timer **x;
	t->rel_secs = t->secs;
	t->rel_msecs = t->msecs;
	t->fired = 0;
	t->deleted = 0;
	x = &timer_chain;
	while (*x != 0
	       && ((*x)->rel_secs <= t->rel_secs
		   || ((*x)->rel_secs == t->rel_secs
		       && (*x)->rel_msecs <= t->rel_msecs)))
	{
	    t->rel_msecs -= (*x)->rel_msecs;
	    t->rel_secs -= (*x)->rel_secs;
	    fix_time (&t->rel_secs, &t->rel_msecs);
	    x = &((*x)->next);
	}
	if (*x != 0)
	{
	    (*x)->rel_msecs -= t->rel_msecs;
	    (*x)->rel_secs -= t->rel_secs;
	    fix_time (&(*x)->rel_secs, &(*x)->rel_msecs);
	}
	t->next = *x;
	*x = t;
	if (timer_chain == t)
	    setup_next_timer ();
    }
    sigprocmask (SIG_SETMASK, &old, 0);
}

static void
delete_timer (Lisp_Timer *t)
{
    Lisp_Timer **x;
    sigset_t old;

    sigprocmask (SIG_BLOCK, &alrm_sigset, &old);
    t->deleted = 1;
    x = &timer_chain;
    while (*x != 0 && (*x) != t)
	x = &((*x)->next);
    if (*x == t)
    {
	if (t->next != 0)
	{
	    t->next->rel_msecs += t->rel_msecs;
	    t->next->rel_secs += t->rel_secs;
	    fix_time (&t->next->rel_secs, &t->next->rel_msecs);
	}
	t->rel_secs = t->rel_msecs = 0;
	*x = t->next;
	if (x == &timer_chain)
	    setup_next_timer ();
    }
    sigprocmask (SIG_SETMASK, &old, 0);
}

static void
timer_fd_handler (int fd)
{
    int dummy;
    int ready, i;
    repv *timers;
    rep_GC_n_roots gc_timers;
    Lisp_Timer *t;
    sigset_t old;

    read (pipe_fds[0], &dummy, sizeof (dummy));
    sigprocmask (SIG_BLOCK, &alrm_sigset, &old);
    ready = 0;
    for (t = timer_chain; t != 0 && t->fired; t = t->next)
	ready++;
    timers = alloca (sizeof (repv) * ready);
    for (i = 0; i < ready; i++)
    {
	timers[i] = rep_VAL(timer_chain);
	timer_chain = timer_chain->next;
    }
    setup_next_timer ();
    sigprocmask (SIG_SETMASK, &old, 0);
    rep_PUSHGCN(gc_timers, timers, ready);
    for (i = 0; i < ready; i++)
    {
	if (!TIMER(timers[i])->deleted)
	    rep_call_lisp1 (TIMER(timers[i])->function, timers[i]);
    }
    rep_POPGCN;
}


/* Lisp interface */

DEFUN("make-timer", Fmake_timer, Smake_timer,
      (repv fun, repv secs, repv msecs), rep_Subr3) /*
::doc:Smake-timer::
make-timer FUNCTION [SECONDS] [MILLISECONDS]
::end:: */
{
    Lisp_Timer *t = rep_ALLOC_CELL (sizeof (Lisp_Timer));
    t->car = timer_type;
    t->function = fun;
    t->secs = rep_INTP(secs) ? rep_INT(secs) : 0;
    t->msecs = rep_INTP(msecs) ? rep_INT(msecs) : 0;
    t->next_alloc = allocated_timers;
    allocated_timers = t;
    insert_timer (t);
    return rep_VAL(t);
}

DEFUN("delete-timer", Fdelete_timer, Sdelete_timer, (repv timer), rep_Subr1) /*
::doc:Sdelete-timer::
delete-timer TIMER
::end:: */
{
    rep_DECLARE1(timer, TIMERP);
    delete_timer (TIMER(timer));
    return timer;
}

DEFUN("set-timer", Fset_timer, Sset_timer,
      (repv timer, repv secs, repv msecs), rep_Subr3) /*
::doc:Sset-timer::
set-timer TIMER [SECONDS] [MILLISECONDS]
::end:: */
{
    rep_DECLARE1(timer, TIMERP);
    delete_timer (TIMER(timer));
    if (rep_INTP(secs) || rep_INTP(msecs))
    {
	TIMER(timer)->secs = rep_INTP(secs) ? rep_INT(secs) : 0;
	TIMER(timer)->msecs = rep_INTP(msecs) ? rep_INT(msecs) : 0;
    }
    insert_timer (TIMER(timer));
    return timer;
}


/* Type hooks */

static void
timer_mark (repv val)
{
    rep_MARKVAL (TIMER(val)->function);
}

static void
timer_mark_active (void)
{
    Lisp_Timer *t;
    sigset_t old;
    sigprocmask (SIG_BLOCK, &alrm_sigset, &old);
    t = timer_chain;
    while (t != 0)
    {
	rep_MARKVAL (rep_VAL(t));
	t = t->next;
    }
    sigprocmask (SIG_SETMASK, &old, 0);
}

static void
timer_sweep (void)
{
    Lisp_Timer *x = allocated_timers;
    allocated_timers = 0;
    while (x != 0)
    {
	Lisp_Timer *next = x->next_alloc;
	if (!rep_GC_CELL_MARKEDP (rep_VAL(x)))
	    rep_FREE_CELL (x);
	else
	{
	    rep_GC_CLR_CELL (rep_VAL(x));
	    x->next_alloc = allocated_timers;
	    allocated_timers = x;
	}
	x = next;
    }
}

static void
timer_print (repv stream, repv arg)
{
    char buf[64];
    sprintf (buf, "#<timer %lds, %ldms>", TIMER(arg)->secs, TIMER(arg)->msecs);
    rep_stream_puts (stream, buf, -1, rep_FALSE);
}


/* DL hooks */

rep_xsubr *rep_dl_subrs[] = { &Smake_timer, &Sdelete_timer, &Sset_timer, 0 };
repv rep_dl_feature;

repv
rep_dl_init (void)
{
    timer_type = rep_register_new_type ("timer", 0, timer_print, timer_print,
					timer_sweep, timer_mark,
					timer_mark_active, 0, 0, 0, 0, 0, 0);
    rep_INTERN (timers);
    rep_dl_feature = Qtimers;
    pipe (pipe_fds);
    rep_register_input_fd (pipe_fds[0], timer_fd_handler);
#ifdef HAVE_UNIX
    rep_unix_set_fd_cloexec (pipe_fds[1]);
#endif
    sigemptyset (&alrm_sigset);
    sigaddset (&alrm_sigset, SIGALRM);
    rep_sig_restart (SIGALRM, rep_TRUE);
    return Qt;
}

void
rep_dl_kill (void)
{
    rep_deregister_input_fd (pipe_fds[0]);
    close (pipe_fds[0]);
    close (pipe_fds[1]);
    signal (SIGALRM, SIG_IGN);
}