/*****************************************************************************
 *
 * tnumber_mathfuncs.c
 *  Temporal mathematical operators (+, -, *, /) and functions (round,
 *  degrees).
 *
 * This MobilityDB code is provided under The PostgreSQL License.
 *
 * Copyright (c) 2020, Université libre de Bruxelles and MobilityDB contributors
 *
 * Permission to use, copy, modify, and distribute this software and its documentation for any purpose, without fee, and without a written agreement is hereby
 * granted, provided that the above copyright notice and this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL UNIVERSITE LIBRE DE BRUXELLES BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST
 * PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF UNIVERSITE LIBRE DE BRUXELLES HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * UNIVERSITE LIBRE DE BRUXELLES SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS" BASIS, AND UNIVERSITE LIBRE DE BRUXELLES HAS NO OBLIGATIONS TO PROVIDE
 * MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS. 
 *
 *****************************************************************************/

#ifndef __TEMPORAL_MATHFUNCS_H__
#define __TEMPORAL_MATHFUNCS_H__

#include <postgres.h>
#include <fmgr.h>
#include <catalog/pg_type.h>

/*****************************************************************************/

extern Datum datum_round(Datum value, Datum prec);

extern Datum add_base_temporal(PG_FUNCTION_ARGS);
extern Datum add_temporal_base(PG_FUNCTION_ARGS);
extern Datum add_temporal_temporal(PG_FUNCTION_ARGS);

extern Datum sub_base_temporal(PG_FUNCTION_ARGS);
extern Datum sub_temporal_base(PG_FUNCTION_ARGS);
extern Datum sub_temporal_temporal(PG_FUNCTION_ARGS);

extern Datum mult_base_temporal(PG_FUNCTION_ARGS);
extern Datum mult_temporal_base(PG_FUNCTION_ARGS);
extern Datum mult_temporal_temporal(PG_FUNCTION_ARGS);

extern Datum div_base_temporal(PG_FUNCTION_ARGS);
extern Datum div_temporal_base(PG_FUNCTION_ARGS);
extern Datum div_temporal_temporal(PG_FUNCTION_ARGS);

extern int int_cmp(const void *a, const void *b);

/*****************************************************************************/

#endif
