/*
 *	Copyright 2024 Long 4 Core
 *
 *	:set tabstop=9
 */

#include	"si_info.h"

void	si_print_cb_blendn_control		(unsigned cb_blendn_control, unsigned n)				// CB:CB_BLEND[0-7]_CONTROL
{
	printf	("si_print_cb_blendn_control (b)\n");

	printf	("CB:CB_BLEND[%d]_CONTROL\n", n);

	printf	("\tCOLOR_SRCBLEND         : %04X %s\n",							// bits (00:04)

			 ((cb_blendn_control & 0x0000001F) >>  0),
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x00) ? "BLEND_ZERO"                     :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x01) ? "BLEND_ONE"                      :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x02) ? "BLEND_SRC_COLOR"                :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x03) ? "BLEND_ONE_MINUS_SRC_COLOR"      :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x04) ? "BLEND_SRC_ALPHA"                :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x05) ? "BLEND_ONE_MINUS_SRC_ALPHA"      :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x06) ? "BLEND_DST_ALPHA"                :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x07) ? "BLEND_ONE_MINUS_DST_ALPHA"      :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x08) ? "BLEND_DST_COLOR"                :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x09) ? "BLEND_ONE_MINUS_DST_COLOR"      :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x0A) ? "BLEND_SRC_ALPHA_SATURATE"       :


			(((cb_blendn_control & 0x0000001F) >>  0) == 0x0D) ? "BLEND_CONSTANT_COLOR"           :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x0E) ? "BLEND_ONE_MINUS_CONSTANT_COLOR" :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x0F) ? "BLEND_SRC1_COLOR"               :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x10) ? "BLEND_INV_SRC1_COLOR"           :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x11) ? "BLEND_SRC1_ALPHA"               :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x12) ? "BLEND_INV_SRC1_ALPHA"           :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x13) ? "BLEND_CONSTANT_ALPHA"           :
			(((cb_blendn_control & 0x0000001F) >>  0) == 0x14) ? "BLEND_ONE_MINUS_CONSTANT_ALPHA" : "");

	printf	("\tCOLOR_DESTBLEND        : %04X %s\n",							// bits (08:12)

			 ((cb_blendn_control & 0x00001F00) >>  8),
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x00) ? "BLEND_ZERO"                     :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x01) ? "BLEND_ONE"                      :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x02) ? "BLEND_SRC_COLOR"                :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x03) ? "BLEND_ONE_MINUS_SRC_COLOR"      :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x04) ? "BLEND_SRC_ALPHA"                :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x05) ? "BLEND_ONE_MINUS_SRC_ALPHA"      :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x06) ? "BLEND_DST_ALPHA"                :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x07) ? "BLEND_ONE_MINUS_DST_ALPHA"      :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x08) ? "BLEND_DST_COLOR"                :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x09) ? "BLEND_ONE_MINUS_DST_COLOR"      :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x0A) ? "BLEND_SRC_ALPHA_SATURATE"       :


			(((cb_blendn_control & 0x00001F00) >>  8) == 0x0D) ? "BLEND_CONSTANT_COLOR"           :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x0E) ? "BLEND_ONE_MINUS_CONSTANT_COLOR" :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x0F) ? "BLEND_SRC1_COLOR"               :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x10) ? "BLEND_INV_SRC1_COLOR"           :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x11) ? "BLEND_SRC1_ALPHA"               :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x12) ? "BLEND_INV_SRC1_ALPHA"           :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x13) ? "BLEND_CONSTANT_ALPHA"           :
			(((cb_blendn_control & 0x00001F00) >>  8) == 0x14) ? "BLEND_ONE_MINUS_CONSTANT_ALPHA" : "");


	printf	("\tALPHA_SRCBLEND         : %04X %s\n",							// bits (16:20)

			 ((cb_blendn_control & 0x001F0000) >> 16),
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x00) ? "BLEND_ZERO"                     :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x01) ? "BLEND_ONE"                      :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x02) ? "BLEND_SRC_COLOR"                :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x03) ? "BLEND_ONE_MINUS_SRC_COLOR"      :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x04) ? "BLEND_SRC_ALPHA"                :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x05) ? "BLEND_ONE_MINUS_SRC_ALPHA"      :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x06) ? "BLEND_DST_ALPHA"                :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x07) ? "BLEND_ONE_MINUS_DST_ALPHA"      :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x08) ? "BLEND_DST_COLOR"                :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x09) ? "BLEND_ONE_MINUS_DST_COLOR"      :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x0A) ? "BLEND_SRC_ALPHA_SATURATE"       :


			(((cb_blendn_control & 0x001F0000) >> 16) == 0x0D) ? "BLEND_CONSTANT_COLOR"           :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x0E) ? "BLEND_ONE_MINUS_CONSTANT_COLOR" :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x0F) ? "BLEND_SRC1_COLOR"               :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x10) ? "BLEND_INV_SRC1_COLOR"           :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x11) ? "BLEND_SRC1_ALPHA"               :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x12) ? "BLEND_INV_SRC1_ALPHA"           :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x13) ? "BLEND_CONSTANT_ALPHA"           :
			(((cb_blendn_control & 0x001F0000) >> 16) == 0x14) ? "BLEND_ONE_MINUS_CONSTANT_ALPHA" : "");

	printf	("\tALPHA_DESTBLEND        : %04X %s\n",							// bits (24:28)

			 ((cb_blendn_control & 0x1F000000) >> 24),
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x00) ? "BLEND_ZERO"                     :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x01) ? "BLEND_ONE"                      :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x02) ? "BLEND_SRC_COLOR"                :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x03) ? "BLEND_ONE_MINUS_SRC_COLOR"      :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x04) ? "BLEND_SRC_ALPHA"                :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x05) ? "BLEND_ONE_MINUS_SRC_ALPHA"      :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x06) ? "BLEND_DST_ALPHA"                :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x07) ? "BLEND_ONE_MINUS_DST_ALPHA"      :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x08) ? "BLEND_DST_COLOR"                :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x09) ? "BLEND_ONE_MINUS_DST_COLOR"      :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x0A) ? "BLEND_SRC_ALPHA_SATURATE"       :


			(((cb_blendn_control & 0x1F000000) >> 24) == 0x0D) ? "BLEND_CONSTANT_COLOR"           :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x0E) ? "BLEND_ONE_MINUS_CONSTANT_COLOR" :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x0F) ? "BLEND_SRC1_COLOR"               :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x10) ? "BLEND_INV_SRC1_COLOR"           :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x11) ? "BLEND_SRC1_ALPHA"               :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x12) ? "BLEND_INV_SRC1_ALPHA"           :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x13) ? "BLEND_CONSTANT_ALPHA"           :
			(((cb_blendn_control & 0x1F000000) >> 24) == 0x14) ? "BLEND_ONE_MINUS_CONSTANT_ALPHA" : "");

	printf	("\tCOLOR_COMB_FCN         : %04X %s\n",							// bits (05:07)

			 ((cb_blendn_control & 0x000000E0) >>  5),
			(((cb_blendn_control & 0x000000E0) >>  5) == 0x00) ? "COMB_DST_PLUS_SRC"              :
			(((cb_blendn_control & 0x000000E0) >>  5) == 0x01) ? "COMB_SRC_MINUS_DST"             :
			(((cb_blendn_control & 0x000000E0) >>  5) == 0x02) ? "COMB_MIN_DST_SRC"               :
			(((cb_blendn_control & 0x000000E0) >>  5) == 0x03) ? "COMB_MAX_DST_SRC"               :
			(((cb_blendn_control & 0x000000E0) >>  5) == 0x04) ? "COMB_DST_MINUS_SRC"             : "");

	printf	("\tALPHA_COMB_FCN         : %04X %s\n",							// bits (21:23)

			 ((cb_blendn_control & 0x00E00000) >> 21),
			(((cb_blendn_control & 0x00E00000) >> 21) == 0x00) ? "COMB_DST_PLUS_SRC"              :
			(((cb_blendn_control & 0x00E00000) >> 21) == 0x01) ? "COMB_SRC_MINUS_DST"             :
			(((cb_blendn_control & 0x00E00000) >> 21) == 0x02) ? "COMB_MIN_DST_SRC"               :
			(((cb_blendn_control & 0x00E00000) >> 21) == 0x03) ? "COMB_MAX_DST_SRC"               :
			(((cb_blendn_control & 0x00E00000) >> 21) == 0x04) ? "COMB_DST_MINUS_SRC"             : "");

	printf	("\tSEPARATE_ALPHA_BLEND   : %04X %s\n",							// bits (29:29)

			 ((cb_blendn_control & 0x20000000) >> 29),
			(((cb_blendn_control & 0x20000000) >> 29) == 0x00) ? "false"                          :
			(((cb_blendn_control & 0x20000000) >> 29) == 0x01) ? "true"                           : "");

	printf	("\tENABLE                 : %04X %s\n",							// bits (30:30)

			 ((cb_blendn_control & 0x40000000) >> 30),
			(((cb_blendn_control & 0x40000000) >> 30) == 0x00) ? "false"                          :
			(((cb_blendn_control & 0x40000000) >> 30) == 0x01) ? "true"                           : "");

	printf	("\tDISABLE_ROP3           : %04X %s\n",							// bits (31:31)

			 ((cb_blendn_control & 0x80000000) >> 31),
			(((cb_blendn_control & 0x80000000) >> 31) == 0x00) ? "false"                          :
			(((cb_blendn_control & 0x80000000) >> 31) == 0x01) ? "true"                           : "");

	printf	("si_print_cb_blendn_control (e)\n");
}

void	si_print_pipe_rt_blend_state	(const struct pipe_rt_blend_state *state)
{
	printf	("si_print_pipe_rt_blend_state (b)\n");

	printf	("\tfact_src_rgb           : %04X %s\n",

			 state->rgb_src_factor,
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_ONE)                ? "PIPE_BLENDFACTOR_ONE"                :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_SRC_COLOR)          ? "PIPE_BLENDFACTOR_SRC_COLOR"          :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_SRC_ALPHA)          ? "PIPE_BLENDFACTOR_SRC_ALPHA"          :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_DST_ALPHA)          ? "PIPE_BLENDFACTOR_DST_ALPHA"          :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_DST_COLOR)          ? "PIPE_BLENDFACTOR_DST_COLOR"          :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE) ? "PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE" :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_CONST_COLOR)        ? "PIPE_BLENDFACTOR_CONST_COLOR"        :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_CONST_ALPHA)        ? "PIPE_BLENDFACTOR_CONST_ALPHA"        :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_SRC1_COLOR)         ? "PIPE_BLENDFACTOR_SRC1_COLOR"         :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_SRC1_ALPHA)         ? "PIPE_BLENDFACTOR_SRC1_ALPHA"         :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_ZERO)               ? "PIPE_BLENDFACTOR_ZERO"               :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_SRC_COLOR)      ? "PIPE_BLENDFACTOR_INV_SRC_COLOR"      :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_SRC_ALPHA)      ? "PIPE_BLENDFACTOR_INV_SRC_ALPHA"      :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_DST_ALPHA)      ? "PIPE_BLENDFACTOR_INV_DST_ALPHA"      :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_DST_COLOR)      ? "PIPE_BLENDFACTOR_INV_DST_COLOR"      :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_CONST_COLOR)    ? "PIPE_BLENDFACTOR_INV_CONST_COLOR"    :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_CONST_ALPHA)    ? "PIPE_BLENDFACTOR_INV_CONST_ALPHA"    :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_SRC1_COLOR)     ? "PIPE_BLENDFACTOR_INV_SRC1_COLOR"     :
			(state->rgb_src_factor   == PIPE_BLENDFACTOR_INV_SRC1_ALPHA)     ? "PIPE_BLENDFACTOR_INV_SRC1_ALPHA"     : "");

	printf	("\tfact_dst_rgb           : %04X %s\n",

			 state->rgb_dst_factor,
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_ONE)                ? "PIPE_BLENDFACTOR_ONE"                :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_SRC_COLOR)          ? "PIPE_BLENDFACTOR_SRC_COLOR"          :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_SRC_ALPHA)          ? "PIPE_BLENDFACTOR_SRC_ALPHA"          :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_DST_ALPHA)          ? "PIPE_BLENDFACTOR_DST_ALPHA"          :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_DST_COLOR)          ? "PIPE_BLENDFACTOR_DST_COLOR"          :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE) ? "PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE" :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_CONST_COLOR)        ? "PIPE_BLENDFACTOR_CONST_COLOR"        :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_CONST_ALPHA)        ? "PIPE_BLENDFACTOR_CONST_ALPHA"        :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_SRC1_COLOR)         ? "PIPE_BLENDFACTOR_SRC1_COLOR"         :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_SRC1_ALPHA)         ? "PIPE_BLENDFACTOR_SRC1_ALPHA"         :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_ZERO)               ? "PIPE_BLENDFACTOR_ZERO"               :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_SRC_COLOR)      ? "PIPE_BLENDFACTOR_INV_SRC_COLOR"      :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_SRC_ALPHA)      ? "PIPE_BLENDFACTOR_INV_SRC_ALPHA"      :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_DST_ALPHA)      ? "PIPE_BLENDFACTOR_INV_DST_ALPHA"      :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_DST_COLOR)      ? "PIPE_BLENDFACTOR_INV_DST_COLOR"      :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_CONST_COLOR)    ? "PIPE_BLENDFACTOR_INV_CONST_COLOR"    :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_CONST_ALPHA)    ? "PIPE_BLENDFACTOR_INV_CONST_ALPHA"    :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_SRC1_COLOR)     ? "PIPE_BLENDFACTOR_INV_SRC1_COLOR"     :
			(state->rgb_dst_factor   == PIPE_BLENDFACTOR_INV_SRC1_ALPHA)     ? "PIPE_BLENDFACTOR_INV_SRC1_ALPHA"     : "");

	printf	("\tfact_src_a             : %04X %s\n",

			 state->alpha_src_factor,
			(state->alpha_src_factor == PIPE_BLENDFACTOR_ONE)                ? "PIPE_BLENDFACTOR_ONE"                :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_SRC_COLOR)          ? "PIPE_BLENDFACTOR_SRC_COLOR"          :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_SRC_ALPHA)          ? "PIPE_BLENDFACTOR_SRC_ALPHA"          :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_DST_ALPHA)          ? "PIPE_BLENDFACTOR_DST_ALPHA"          :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_DST_COLOR)          ? "PIPE_BLENDFACTOR_DST_COLOR"          :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE) ? "PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE" :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_CONST_COLOR)        ? "PIPE_BLENDFACTOR_CONST_COLOR"        :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_CONST_ALPHA)        ? "PIPE_BLENDFACTOR_CONST_ALPHA"        :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_SRC1_COLOR)         ? "PIPE_BLENDFACTOR_SRC1_COLOR"         :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_SRC1_ALPHA)         ? "PIPE_BLENDFACTOR_SRC1_ALPHA"         :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_ZERO)               ? "PIPE_BLENDFACTOR_ZERO"               :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_SRC_COLOR)      ? "PIPE_BLENDFACTOR_INV_SRC_COLOR"      :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA)      ? "PIPE_BLENDFACTOR_INV_SRC_ALPHA"      :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_DST_ALPHA)      ? "PIPE_BLENDFACTOR_INV_DST_ALPHA"      :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_DST_COLOR)      ? "PIPE_BLENDFACTOR_INV_DST_COLOR"      :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_CONST_COLOR)    ? "PIPE_BLENDFACTOR_INV_CONST_COLOR"    :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_CONST_ALPHA)    ? "PIPE_BLENDFACTOR_INV_CONST_ALPHA"    :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_SRC1_COLOR)     ? "PIPE_BLENDFACTOR_INV_SRC1_COLOR"     :
			(state->alpha_src_factor == PIPE_BLENDFACTOR_INV_SRC1_ALPHA)     ? "PIPE_BLENDFACTOR_INV_SRC1_ALPHA"     : "");

	printf	("\tfact_dst_a             : %04X %s\n",

			 state->alpha_dst_factor,
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_ONE)                ? "PIPE_BLENDFACTOR_ONE"                :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_SRC_COLOR)          ? "PIPE_BLENDFACTOR_SRC_COLOR"          :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_SRC_ALPHA)          ? "PIPE_BLENDFACTOR_SRC_ALPHA"          :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_DST_ALPHA)          ? "PIPE_BLENDFACTOR_DST_ALPHA"          :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_DST_COLOR)          ? "PIPE_BLENDFACTOR_DST_COLOR"          :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE) ? "PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE" :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_CONST_COLOR)        ? "PIPE_BLENDFACTOR_CONST_COLOR"        :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_CONST_ALPHA)        ? "PIPE_BLENDFACTOR_CONST_ALPHA"        :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_SRC1_COLOR)         ? "PIPE_BLENDFACTOR_SRC1_COLOR"         :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_SRC1_ALPHA)         ? "PIPE_BLENDFACTOR_SRC1_ALPHA"         :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_ZERO)               ? "PIPE_BLENDFACTOR_ZERO"               :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_SRC_COLOR)      ? "PIPE_BLENDFACTOR_INV_SRC_COLOR"      :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_SRC_ALPHA)      ? "PIPE_BLENDFACTOR_INV_SRC_ALPHA"      :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_DST_ALPHA)      ? "PIPE_BLENDFACTOR_INV_DST_ALPHA"      :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_DST_COLOR)      ? "PIPE_BLENDFACTOR_INV_DST_COLOR"      :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_CONST_COLOR)    ? "PIPE_BLENDFACTOR_INV_CONST_COLOR"    :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_CONST_ALPHA)    ? "PIPE_BLENDFACTOR_INV_CONST_ALPHA"    :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_SRC1_COLOR)     ? "PIPE_BLENDFACTOR_INV_SRC1_COLOR"     :
			(state->alpha_dst_factor == PIPE_BLENDFACTOR_INV_SRC1_ALPHA)     ? "PIPE_BLENDFACTOR_INV_SRC1_ALPHA"     : "");

	printf	("\tfunc_rgb               : %04X %s\n",

			 state->rgb_func,
			(state->rgb_func         == PIPE_BLEND_ADD)                      ? "PIPE_BLEND_ADD"                      :
			(state->rgb_func         == PIPE_BLEND_SUBTRACT)                 ? "PIPE_BLEND_SUBTRACT"                 :
			(state->rgb_func         == PIPE_BLEND_REVERSE_SUBTRACT)         ? "PIPE_BLEND_REVERSE_SUBTRACT"         :
			(state->rgb_func         == PIPE_BLEND_MIN)                      ? "PIPE_BLEND_MIN"                      :
			(state->rgb_func         == PIPE_BLEND_MAX)                      ? "PIPE_BLEND_MAX"                      : "");

	printf	("\tfunc_a                 : %04X %s\n",

			 state->alpha_func,
			(state->alpha_func       == PIPE_BLEND_ADD)                      ? "PIPE_BLEND_ADD"                      :
			(state->alpha_func       == PIPE_BLEND_SUBTRACT)                 ? "PIPE_BLEND_SUBTRACT"                 :
			(state->alpha_func       == PIPE_BLEND_REVERSE_SUBTRACT)         ? "PIPE_BLEND_REVERSE_SUBTRACT"         :
			(state->alpha_func       == PIPE_BLEND_MIN)                      ? "PIPE_BLEND_MIN"                      :
			(state->alpha_func       == PIPE_BLEND_MAX)                      ? "PIPE_BLEND_MAX"                      : "");

	printf	("si_print_pipe_rt_blend_state (e)\n");
}

