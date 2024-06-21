/*
 *	Copyright 2024 Long 4 Core
 *
 *	:set tabstop=9
 */

#ifndef	__SI_INFO_H__
#define	__SI_INFO_H__

#include	<stdio.h>
#include	<pipe/p_state.h>

extern	void	si_print_cb_blendn_control		(unsigned cb_blendn_control, unsigned n);			// CB:CB_BLEND[0-7]_CONTROL
extern	void	si_print_pipe_rt_blend_state	(const struct pipe_rt_blend_state *state);

#endif

