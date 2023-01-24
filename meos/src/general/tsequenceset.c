/*****************************************************************************
 *
 * This MobilityDB code is provided under The PostgreSQL License.
 * Copyright (c) 2016-2023, Université libre de Bruxelles and MobilityDB
 * contributors
 *
 * MobilityDB includes portions of PostGIS version 3 source code released
 * under the GNU General Public License (GPLv2 or later).
 * Copyright (c) 2001-2023, PostGIS contributors
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written
 * agreement is hereby granted, provided that the above copyright notice and
 * this paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL UNIVERSITE LIBRE DE BRUXELLES BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
 * EVEN IF UNIVERSITE LIBRE DE BRUXELLES HAS BEEN ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * UNIVERSITE LIBRE DE BRUXELLES SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON
 * AN "AS IS" BASIS, AND UNIVERSITE LIBRE DE BRUXELLES HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS. 
 *
 *****************************************************************************/

/**
 * @brief General functions for temporal sequence sets.
 */

#include "general/tsequenceset.h"

/* C */
#include <assert.h>
/* PostgreSQL */
#include <postgres.h>
#include <utils/timestamp.h>
/* MEOS */
#include <meos.h>
#include <meos_internal.h>
#include "general/pg_types.h"
#include "general/set.h"
#include "general/spanset.h"
#include "general/temporaltypes.h"
#include "general/temporal_boxops.h"
#include "general/type_parser.h"
#include "general/type_util.h"
#include "point/tpoint_parser.h"

/*****************************************************************************
 * General functions
 *****************************************************************************/

/**
 * @brief Return the location of a timestamp in a temporal sequence set using
 * binary search
 *
 * If the timestamp is contained in the temporal sequence set, the index of the
 * sequence is returned in the output parameter. Otherwise, returns a number
 * encoding whether the timestamp is before, between two sequences, or after
 * the temporal sequence set. For example, given a value composed of 3 sequences
 * and a timestamp, the value returned in the output parameter is as follows:
 * @code
 *               0          1          2
 *            |-----|    |-----|    |-----|
 * 1)    t^                                         => loc = 0
 * 2)                 t^                            => loc = 1
 * 3)                       t^                      => loc = 1
 * 4)                             t^                => loc = 2
 * 5)                                         t^    => loc = 3
 * @endcode
 *
 * @param[in] ss Temporal sequence set
 * @param[in] t Timestamp
 * @param[out] loc Location
 * @result Return true if the timestamp is contained in the temporal sequence set
 */
bool
tsequenceset_find_timestamp(const TSequenceSet *ss, TimestampTz t, int *loc)
{
  int first = 0, last = ss->count - 1;
  int middle = 0; /* make compiler quiet */
  const TSequence *seq = NULL; /* make compiler quiet */
  while (first <= last)
  {
    middle = (first + last)/2;
    seq = tsequenceset_seq_n(ss, middle);
    if (contains_period_timestamp(&seq->period, t))
    {
      *loc = middle;
      return true;
    }
    if (t <= (TimestampTz) seq->period.lower)
      last = middle - 1;
    else
      first = middle + 1;
  }
  if (t >= (TimestampTz) seq->period.upper)
    middle++;
  *loc = middle;
  return false;
}

/*****************************************************************************
 * Constructor functions
 *****************************************************************************/

/**
 * @brief Ensure the validity of the arguments when creating a temporal sequence set
 */
static void
tsequenceset_make_valid(const TSequence **sequences, int count)
{
  bool linear = MOBDB_FLAGS_GET_LINEAR(sequences[0]->flags);
  /* Ensure that all values are of sequence subtype and of the same interpolation */
  for (int i = 0; i < count; i++)
  {
    if (sequences[i]->subtype != TSEQUENCE)
      elog(ERROR, "Input values must be temporal sequences");
    if (MOBDB_FLAGS_GET_LINEAR(sequences[i]->flags) != linear)
      elog(ERROR, "Input sequences must have the same interpolation");
  }
  return;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Set the second argument to the bounding box of a temporal sequence set
 * @sqlfunc period(), tbox(), stbox()
 * @sqlop @p ::
 */
void
tsequenceset_set_bbox(const TSequenceSet *ss, void *box)
{
  memset(box, 0, ss->bboxsize);
  memcpy(box, TSEQUENCESET_BBOX_PTR(ss), ss->bboxsize);
  return;
}

/**
 * @brief Return a pointer to the offsets array of a temporal sequence set
 */
static size_t *
tsequenceset_offsets_ptr(const TSequenceSet *ss)
{
  return (size_t *)(((char *)ss) + double_pad(sizeof(TSequenceSet)) +
    /* The period component of the bbox is already declared in the struct */
    double_pad(ss->bboxsize - sizeof(Span)));
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the n-th sequence of a temporal sequence set.
 * @pre The argument @p index is less than the number of sequences in the
 * sequence set
 */
const TSequence *
tsequenceset_seq_n(const TSequenceSet *ss, int index)
{
  return (TSequence *)(
    /* start of data */
    ((char *)ss) + double_pad(sizeof(TSequenceSet)) +
      /* The period component of the bbox is already declared in the struct */
      (ss->bboxsize - sizeof(Span)) + ss->count * sizeof(size_t) +
      /* offset */
      (tsequenceset_offsets_ptr(ss))[index]);
}

/**
 * @brief Construct a temporal sequence set from an array of temporal sequences
 *
 * For example, the memory structure of a temporal sequence set with two
 * sequences is as follows
 * @code
 * ------------------------------------------------------------
 * ( TSequenceSet )_X | ( bbox )_X | offset_0 | offset_1 | ...
 * ------------------------------------------------------------
 * ---------------------------------------
 * ( TSequence_0 )_X | ( TSequence_1 )_X |
 * ---------------------------------------
 * @endcode
 * where the `_X` are unused bytes added for double padding, `offset_0` and
 * `offset_1` are offsets for the corresponding sequences.
 *
 * @param[in] sequences Array of sequences
 * @param[in] count Number of elements in the array
 * @param[in] maxcount Maximum number of elements in the array
 * @param[in] normalize True if the resulting value should be normalized.
 * In particular, normalize is false when synchronizing two temporal sequence
 * sets before applying an operation to them.
 */
static TSequenceSet *
tsequenceset_make1_exp(const TSequence **sequences, int count, int maxcount,
  bool normalize)
{
  assert(maxcount >= count);

  /* Test the validity of the sequences */
  assert(count > 0);
  ensure_valid_tseqarr(sequences, count);
  /* Normalize the array of sequences */
  TSequence **normseqs = (TSequence **) sequences;
  int newcount = count;
  if (normalize && count > 1)
    normseqs = tseqarr_normalize(sequences, count, &newcount);

  /* Get the bounding box size */
  size_t bboxsize = temporal_bbox_size(sequences[0]->temptype);
  /* The period component of the bbox is already declared in the struct */
  size_t bboxsize_extra = (bboxsize == 0) ? 0 : bboxsize - sizeof(Span);

  /* Compute the size of the temporal sequence set */
  size_t seqs_size = 0;
  int totalcount = 0;
  for (int i = 0; i < newcount; i++)
  {
    totalcount += normseqs[i]->count;
    seqs_size += double_pad(VARSIZE(normseqs[i]));
  }
  /* Compute the total size for maxcount sequences as a proportion of the size
   * of the count sequences provided. Note that this is only an initial
   * estimation. The functions adding sequences to a sequence set must verify
   * both the maximum number of sequences and the remaining space for adding an
   * additional variable-length sequences of arbitrary size */
  if (count != maxcount)
    seqs_size *= (double) maxcount / count;
  else
    maxcount = newcount;
  /* Size of the struct and the offset array */
  size_t memsize = double_pad(sizeof(TSequenceSet)) + bboxsize_extra +
    maxcount * sizeof(size_t) + seqs_size;
  /* Create the temporal sequence set */
  TSequenceSet *result = palloc0(memsize);
  SET_VARSIZE(result, memsize);
  result->count = newcount;
  result->maxcount = maxcount;
  result->totalcount = totalcount;
  result->temptype = sequences[0]->temptype;
  result->subtype = TSEQUENCESET;
  result->bboxsize = bboxsize;
  MOBDB_FLAGS_SET_CONTINUOUS(result->flags,
    MOBDB_FLAGS_GET_CONTINUOUS(sequences[0]->flags));
  MOBDB_FLAGS_SET_INTERP(result->flags,
    MOBDB_FLAGS_GET_INTERP(sequences[0]->flags));
  MOBDB_FLAGS_SET_X(result->flags, true);
  MOBDB_FLAGS_SET_T(result->flags, true);
  if (tgeo_type(sequences[0]->temptype))
  {
    MOBDB_FLAGS_SET_Z(result->flags,
      MOBDB_FLAGS_GET_Z(sequences[0]->flags));
    MOBDB_FLAGS_SET_GEODETIC(result->flags,
      MOBDB_FLAGS_GET_GEODETIC(sequences[0]->flags));
  }
  /* Initialization of the variable-length part */
  /*
   * Compute the bounding box
   * Only external types have bounding box, internal types such
   * as double2, double3, or double4 do not have bounding box
   */
  if (bboxsize != 0)
  {
    tsequenceset_compute_bbox((const TSequence **) normseqs, newcount,
      TSEQUENCESET_BBOX_PTR(result));
  }
  /* Store the composing instants */
  size_t pdata = double_pad(sizeof(TSequenceSet)) +
    double_pad(bboxsize - sizeof(Span)) + newcount * sizeof(size_t);
  size_t pos = 0;
  for (int i = 0; i < newcount; i++)
  {
    memcpy(((char *) result) + pdata + pos, normseqs[i],
      VARSIZE(normseqs[i]));
    (tsequenceset_offsets_ptr(result))[i] = pos;
    pos += double_pad(VARSIZE(normseqs[i]));
  }
  if (normalize && count > 1)
    pfree_array((void **) normseqs, newcount);
  return result;
}

static TSequenceSet *
tsequenceset_make1(const TSequence **sequences, int count, bool normalize)
{
  return tsequenceset_make1_exp(sequences, count, count, normalize);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal sequence set from an array of temporal sequences.
 *
 * @param[in] sequences Array of sequences
 * @param[in] count Number of elements in the array
 * @param[in] normalize True if the resulting value should be normalized.
 * In particular, normalize is false when synchronizing two
 * temporal sequence sets before applying an operation to them.
 * @sqlfunc tbool_seqset(), tint_seqset(), tfloat_seqset(), ttext_seqset(), etc.
 */
TSequenceSet *
tsequenceset_make(const TSequence **sequences, int count, bool normalize)
{
  tsequenceset_make_valid(sequences, count);
  return tsequenceset_make1(sequences, count, normalize);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal sequence set from an array of temporal
 * sequences and free the array and the sequences after the creation.
 *
 * @param[in] sequences Array of sequences
 * @param[in] count Number of elements in the array
 * @param[in] normalize True if the resulting value should be normalized.
 * @see tsequenceset_make
 */
TSequenceSet *
tsequenceset_make_free(TSequence **sequences, int count, bool normalize)
{
  if (count == 0)
  {
    pfree(sequences);
    return NULL;
  }
  TSequenceSet *result = tsequenceset_make((const TSequence **) sequences,
    count, normalize);
  pfree_array((void **) sequences, count);
  return result;
}

/**
 * @brief Ensure the validity of the arguments when creating a temporal value
 * This function extends function tsequence_make_valid by spliting the
 * sequences according the maximum distance or interval between instants.
 */
static int *
tsequenceset_make_valid_gaps(const TInstant **instants, int count,
  bool lower_inc, bool upper_inc, interpType interp, double maxdist,
  Interval *maxt, int *countsplits)
{
  tsequence_make_valid1(instants, count, lower_inc, upper_inc, interp);
  return ensure_valid_tinstarr_gaps(instants, count, MERGE_NO,
    interp, maxdist, maxt, countsplits);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal sequence set from an array of temporal instants
 * introducing a gap when two consecutive instants are separated from each
 * other by at least the given distance or the given time interval.
 *
 * @param[in] instants Array of instants
 * @param[in] count Number of elements in the array
 * @param[in] interp Interpolation
 * @param[in] maxdist Maximum distance for defining a gap
 * @param[in] maxt Maximum time interval for defining a gap
 * @sqlfunc tint_seqset_gaps(), tfloat_seqset_gaps(),
 * tgeompoint_seqset_gaps(), tgeogpoint_seqset_gaps()
 */
TSequenceSet *
tsequenceset_make_gaps(const TInstant **instants, int count, interpType interp,
  float maxdist, Interval *maxt)
{
  /* Set the interval to NULL if it is negative or zero */
  Interval intervalzero;
  memset(&intervalzero, 0, sizeof(Interval));
  int cmp = pg_interval_cmp(maxt, &intervalzero);
  if (cmp <= 0)
    maxt = NULL;

  TSequence *seq;
  TSequenceSet *result;

  /* If no gaps are given construt call the standard sequence constructor */
  if (maxdist <= 0.0 && maxt == NULL)
  {
    seq = tsequence_make((const TInstant **) instants, count, true, true,
      interp, NORMALIZE);
    result = tsequenceset_make((const TSequence **) &seq, 1, NORMALIZE_NO);
    pfree(seq);
    return result;
  }

  /* Ensure that the array of instants is valid and determine the splits */
  int countsplits;
  int *splits = tsequenceset_make_valid_gaps((const TInstant **) instants,
    count, true, true, interp, maxdist, maxt, &countsplits);
  if (countsplits == 0)
  {
    /* There are no gaps  */
    pfree(splits);
    seq = tsequence_make1((const TInstant **) instants, count, true, true,
      interp, NORMALIZE);
    result = tsequenceset_make((const TSequence **) &seq, 1, NORMALIZE_NO);
    pfree(seq);
  }
  else
  {
    int newcount = 0;
    /* Split according to gaps  */
    const TInstant **newinsts = palloc(sizeof(TInstant *) * count);
    TSequence **sequences = palloc(sizeof(TSequence *) * (countsplits + 1));
    int j = 0, k = 0;
    for (int i = 0; i < count; i++)
    {
      if (splits[j] == i)
      {
        /* Finalize the current sequence and start a new one */
        assert(k > 0);
        sequences[newcount++] = tsequence_make1((const TInstant **) newinsts,
          k, true, true, interp, NORMALIZE);
        j++; k = 0;
      }
      /* Continue with the current sequence */
      newinsts[k++] = instants[i];
    }
    /* Construct last sequence */
    if (k > 0)
      sequences[newcount++] = tsequence_make1((const TInstant **) newinsts,
        k, true, true, interp, NORMALIZE);
    result = tsequenceset_make((const TSequence **) sequences, newcount,
      NORMALIZE);
    pfree(newinsts); pfree(sequences);
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_constructor
 * @brief Return a copy of a temporal sequence set.
 */
TSequenceSet *
tsequenceset_copy(const TSequenceSet *ss)
{
  TSequenceSet *result = palloc0(VARSIZE(ss));
  memcpy(result, ss, VARSIZE(ss));
  return result;
}

/*****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_constructor
 * @brief Construct a temporal sequence set from a base value and the time
 * frame of another temporal sequence set.
 *
 * @param[in] value Base value
 * @param[in] temptype Temporal type
 * @param[in] ss Period set
 * @param[in] interp Interpolation
 */
TSequenceSet *
tsequenceset_from_base(Datum value, meosType temptype, const TSequenceSet *ss,
  interpType interp)
{
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->count);
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    sequences[i] = tsequence_from_base_time(value, temptype, &seq->period,
      interp);
  }
  return tsequenceset_make_free(sequences, ss->count, NORMALIZE_NO);
}

#if MEOS
/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal boolean sequence set from a boolean and the
 * time frame of another temporal sequence set
 */
TSequenceSet *
tboolseqset_from_base(bool b, const TSequenceSet *ss)
{
  return tsequenceset_from_base(BoolGetDatum(b), T_TBOOL, ss, false);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal integer sequence set from an integer and the
 * time frame of another temporal sequence set
 */
TSequenceSet *
tintseqset_from_base(int i, const TSequenceSet *ss)
{
  return tsequenceset_from_base(Int32GetDatum(i), T_TINT, ss, false);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal float sequence set from a float and the time
 * frame of another temporal sequence set.
 */
TSequenceSet *
tfloatseqset_from_base(double d, const TSequenceSet *ss, interpType interp)
{
  return tsequenceset_from_base(Float8GetDatum(d), T_TFLOAT, ss, interp);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal text sequence set from a text and the time
 * frame of another temporal sequence set.
 */
TSequenceSet *
ttextseqset_from_base(const text *txt, const TSequenceSet *ss)
{
  return tsequenceset_from_base(PointerGetDatum(txt), T_TTEXT, ss, false);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal geometric point sequence set from a point and
 * the time frame of another temporal sequence set
 */
TSequenceSet *
tgeompointseqset_from_base(const GSERIALIZED *gs, const TSequenceSet *ss,
  interpType interp)
{
  return tsequenceset_from_base(PointerGetDatum(gs), T_TGEOMPOINT, ss, interp);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal geographic point sequence set from a point and
 * the time frame of another temporal sequence set
 */
TSequenceSet *
tgeogpointseqset_from_base(const GSERIALIZED *gs, const TSequenceSet *ss,
  interpType interp)
{
  return tsequenceset_from_base(PointerGetDatum(gs), T_TGEOGPOINT, ss, interp);
}
#endif /* MEOS */

/*****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_constructor
 * @brief Construct a temporal sequence set from a base value and a period set.
 *
 * @param[in] value Base value
 * @param[in] temptype Temporal type
 * @param[in] ps Period set
 * @param[in] interp Interpolation
 */
TSequenceSet *
tsequenceset_from_base_time(Datum value, meosType temptype,
  const SpanSet *ps, interpType interp)
{
  TSequence **sequences = palloc(sizeof(TSequence *) * ps->count);
  for (int i = 0; i < ps->count; i++)
  {
    const Span *p = spanset_sp_n(ps, i);
    sequences[i] = tsequence_from_base_time(value, temptype, p, interp);
  }
  return tsequenceset_make_free(sequences, ps->count, NORMALIZE_NO);
}

#if MEOS
/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal boolean sequence set from a boolean and a
 * period set.
 */
TSequenceSet *
tboolseqset_from_base_time(bool b, const SpanSet *ps)
{
  return tsequenceset_from_base_time(BoolGetDatum(b), T_TBOOL, ps, STEPWISE);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal integer sequence set from an integer and a
 * period set.
 */
TSequenceSet *
tintseqset_from_base_time(int i, const SpanSet *ps)
{
  return tsequenceset_from_base_time(Int32GetDatum(i), T_TINT, ps, STEPWISE);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal float sequence set from a float and a period set.
 */
TSequenceSet *
tfloatseqset_from_base_time(double d, const SpanSet *ps, interpType interp)
{
  return tsequenceset_from_base_time(Float8GetDatum(d), T_TFLOAT, ps, interp);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal text sequence set from a text and a period set.
 */
TSequenceSet *
ttextseqset_from_base_time(const text *txt, const SpanSet *ps)
{
  return tsequenceset_from_base_time(PointerGetDatum(txt), T_TTEXT, ps, false);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal geometric point sequence set from a point and a
 * period set.
 */
TSequenceSet *
tgeompointseqset_from_base_time(const GSERIALIZED *gs, const SpanSet *ps,
  interpType interp)
{
  return tsequenceset_from_base_time(PointerGetDatum(gs), T_TGEOMPOINT, ps,
    interp);
}

/**
 * @ingroup libmeos_temporal_constructor
 * @brief Construct a temporal geographic point sequence set from a point and a
 * period set.
 */
TSequenceSet *
tgeogpointseqset_from_base_time(const GSERIALIZED *gs, const SpanSet *ps,
  interpType interp)
{
  return tsequenceset_from_base_time(PointerGetDatum(gs), T_TGEOGPOINT, ps,
    interp);
}
#endif /* MEOS */

/*****************************************************************************
 * Accessor functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the array of distinct base values of a temporal sequence set
 * with stepwise interpolation
 *
 * @param[in] ss Temporal sequence set
 * @param[out] count Number of elements in the output array
 * @result Array of Datums
 * @sqlfunc getValues()
 */
Datum *
tsequenceset_values(const TSequenceSet *ss, int *count)
{
  Datum *result = palloc(sizeof(Datum *) * ss->totalcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    for (int j = 0; j < seq->count; j++)
      result[k++] = tinstant_value(tsequence_inst_n(seq, j));
  }
  if (k > 1)
  {
    meosType basetype = temptype_basetype(ss->temptype);
    datumarr_sort(result, k, basetype);
    k = datumarr_remove_duplicates(result, k, basetype);
  }
  *count = k;
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the span set of a temporal float sequence set.
 * @sqlfunc getValues()
 */
SpanSet *
tfloatseqset_spanset(const TSequenceSet *ss)
{
  int count1 = MOBDB_FLAGS_GET_LINEAR(ss->flags) ? ss->count : ss->totalcount;
  Span **spans = palloc(sizeof(Span *) * count1);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tfloatseq_spans(seq, &spans[k]);
  }
  int count;
  Span **normspans = spanarr_normalize(spans, k, SORT, &count);
  pfree_array((void **) spans, k);
  return spanset_make_free(normspans, count, NORMALIZE_NO);
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return a pointer to the instant with minimum base value of a
 * temporal sequence set.
 *
 * @note The function does not take into account whether the instant is at an
 * exclusive bound or not
 * @note Function used, e.g., for computing the shortest line between two
 * temporal points from their temporal distance.
 * @sqlfunc minInstant()
 */
const TInstant *
tsequenceset_min_instant(const TSequenceSet *ss)
{
  const TSequence *seq = tsequenceset_seq_n(ss, 0);
  const TInstant *result = tsequence_inst_n(seq, 0);
  Datum min = tinstant_value(result);
  meosType basetype = temptype_basetype(seq->temptype);
  for (int i = 0; i < ss->count; i++)
  {
    seq = tsequenceset_seq_n(ss, i);
    for (int j = 0; j < seq->count; j++)
    {
      const TInstant *inst = tsequence_inst_n(seq, j);
      Datum value = tinstant_value(inst);
      if (datum_lt(value, min, basetype))
      {
        min = value;
        result = inst;
      }
    }
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return a pointer to the instant with maximum base value of a
 * temporal sequence set.
 *
 * @note The function does not take into account whether the instant is at an
 * exclusive bound or not
 * @sqlfunc maxInstant()
 */
const TInstant *
tsequenceset_max_instant(const TSequenceSet *ss)
{
  const TSequence *seq = tsequenceset_seq_n(ss, 0);
  const TInstant *result = tsequence_inst_n(seq, 0);
  Datum max = tinstant_value(result);
  meosType basetype = temptype_basetype(seq->temptype);
  for (int i = 0; i < ss->count; i++)
  {
    seq = tsequenceset_seq_n(ss, i);
    for (int j = 0; j < seq->count; j++)
    {
      const TInstant *inst = tsequence_inst_n(seq, j);
      Datum value = tinstant_value(inst);
      if (datum_gt(value, max, basetype))
      {
        max = value;
        result = inst;
      }
    }
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the minimum base value of a temporal sequence set.
 * @sqlfunc minValue()
 */
Datum
tsequenceset_min_value(const TSequenceSet *ss)
{
  if (tnumber_type(ss->temptype))
  {
    TBox *box = TSEQUENCESET_BBOX_PTR(ss);
    Datum dmin = box->span.lower;
    meosType basetype = temptype_basetype(ss->temptype);
    Datum min = double_datum(DatumGetFloat8(dmin), basetype);
    return min;
  }

  meosType basetype = temptype_basetype(ss->temptype);
  Datum result = tsequence_min_value(tsequenceset_seq_n(ss, 0));
  for (int i = 1; i < ss->count; i++)
  {
    Datum value = tsequence_min_value(tsequenceset_seq_n(ss, i));
    if (datum_lt(value, result, basetype))
      result = value;
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the maximum base value of a temporal sequence set.
 * @sqlfunc maxValue()
 */
Datum
tsequenceset_max_value(const TSequenceSet *ss)
{
  if (tnumber_type(ss->temptype))
  {
    TBox *box = TSEQUENCESET_BBOX_PTR(ss);
    Datum dmax = box->span.upper;
    /* The span in a TBox is always a double span */
    meosType basetype = temptype_basetype(ss->temptype);
    Datum max = double_datum(DatumGetFloat8(dmax), basetype);
    return max;
  }

  meosType basetype = temptype_basetype(ss->temptype);
  Datum result = tsequence_max_value(tsequenceset_seq_n(ss, 0));
  for (int i = 1; i < ss->count; i++)
  {
    Datum value = tsequence_max_value(tsequenceset_seq_n(ss, i));
    if (datum_gt(value, result, basetype))
      result = value;
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the time frame a temporal sequence set as a period set.
 * @sqlfunc getTime()
 */
SpanSet *
tsequenceset_time(const TSequenceSet *ss)
{
  const Span **periods = palloc(sizeof(Span *) * ss->count);
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    periods[i] = &seq->period;
  }
  SpanSet *result = spanset_make(periods, ss->count, NORMALIZE_NO);
  pfree(periods);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the duration of a temporal sequence set.
 * @sqlfunc duration()
 */
Interval *
tsequenceset_duration(const TSequenceSet *ss, bool boundspan)
{
  /* Compute the duration of the bounding period */
  if (boundspan)
    return pg_timestamp_mi(ss->period.upper, ss->period.lower);

  const TSequence *seq = tsequenceset_seq_n(ss, 0);
  Interval *result = pg_timestamp_mi(seq->period.upper, seq->period.lower);
  for (int i = 1; i < ss->count; i++)
  {
    seq = tsequenceset_seq_n(ss, i);
    Interval *interval1 = pg_timestamp_mi(seq->period.upper, seq->period.lower);
    Interval *interval2 = pg_interval_pl(result, interval1);
    pfree(result); pfree(interval1);
    result = interval2;
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_cast
 * @brief Return the bounding period of a temporal sequence set.
 * @sqlfunc period()
 * @sqlop @p ::
 */
void
tsequenceset_set_period(const TSequenceSet *ss, Span *p)
{
  const TSequence *start = tsequenceset_seq_n(ss, 0);
  const TSequence *end = tsequenceset_seq_n(ss, ss->count - 1);
  span_set(start->period.lower, end->period.upper, start->period.lower_inc,
    end->period.upper_inc, T_TIMESTAMPTZ, p);
  return;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return an array of pointers to the sequences of a temporal sequence
 * set.
 */
const TSequence **
tsequenceset_sequences_p(const TSequenceSet *ss)
{
  const TSequence **result = palloc(sizeof(TSequence *) * ss->count);
  for (int i = 0; i < ss->count; i++)
    result[i] = tsequenceset_seq_n(ss, i);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the array of sequences of a temporal sequence set.
 * @post The output parameter @p count is equal to the number of sequences of
 * the input temporal sequence set
 * @sqlfunc sequences()
 */
TSequence **
tsequenceset_sequences(const TSequenceSet *ss, int *count)
{
  TSequence **result = palloc(sizeof(TSequence *) * ss->count);
  for (int i = 0; i < ss->count; i++)
    result[i] = tsequence_copy(tsequenceset_seq_n(ss, i));
  *count = ss->count;
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the array of segments of a temporal sequence set.
 * @sqlfunc segments()
 */
TSequence **
tsequenceset_segments(const TSequenceSet *ss, int *count)
{
  TSequence **result = palloc(sizeof(TSequence *) * ss->totalcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tsequence_segments1(seq, &result[k]);
  }
  *count = k;
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the number of distinct instants of a temporal sequence set.
 * @sqlfunc numInstants()
 */
int
tsequenceset_num_instants(const TSequenceSet *ss)
{
  const TInstant *lastinst;
  bool first = true;
  int result = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    result += seq->count;
    if (! first)
    {
      if (tinstant_eq(lastinst, tsequence_inst_n(seq, 0)))
        result --;
    }
    lastinst = tsequence_inst_n(seq, seq->count - 1);
    first = false;
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the n-th distinct instant of a temporal sequence set.
 * @sqlfunc instantN()
 */
const TInstant *
tsequenceset_inst_n(const TSequenceSet *ss, int n)
{
  assert (n >= 1 && n <= ss->totalcount);
  if (n == 1)
    return tsequence_inst_n(tsequenceset_seq_n(ss, 0), 0);

  /* Continue the search 0-based */
  n--;
  const TInstant *prev, *next;
  bool first = true, found = false;
  int i = 0, count = 0, prevcount = 0;
  while (i < ss->count)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    count += seq->count;
    if (! first && tinstant_eq(prev, tsequence_inst_n(seq, 0)))
    {
        prevcount --;
        count --;
    }
    if (prevcount <= n && n < count)
    {
      next = tsequence_inst_n(seq, n - prevcount);
      found = true;
      break;
    }
    prevcount = count;
    prev = tsequence_inst_n(seq, seq->count - 1);
    first = false;
    i++;
  }
  if (! found)
    return NULL;
  return next;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the distinct instants of a temporal sequence set.
 * @post The output parameter @p count is equal to the number of instants of
 * the input temporal sequence set
 * @sqlfunc instants()
 */
const TInstant **
tsequenceset_instants(const TSequenceSet *ss, int *count)
{
  const TInstant **result = palloc(sizeof(TInstant *) * ss->totalcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    for (int j = 0; j < seq->count; j++)
      result[k++] = tsequence_inst_n(seq, j);
  }
  *count = ss->totalcount;
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the start timestamp of a temporal sequence set.
 * @sqlfunc startTimestamp()
 */
TimestampTz
tsequenceset_start_timestamp(const TSequenceSet *ss)
{
  const TSequence *seq = tsequenceset_seq_n(ss, 0);
  return seq->period.lower;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the end timestamp of a temporal sequence set.
 * @sqlfunc endTimestamp()
 */
TimestampTz
tsequenceset_end_timestamp(const TSequenceSet *ss)
{
  const TSequence *seq = tsequenceset_seq_n(ss, ss->count - 1);
  return seq->period.upper;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the number of distinct timestamps of a temporal sequence set.
 * @sqlfunc numTimestamps()
 */
int
tsequenceset_num_timestamps(const TSequenceSet *ss)
{
  TimestampTz lasttime;
  bool first = true;
  int result = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    result += seq->count;
    if (! first)
    {
      if (lasttime == tsequence_inst_n(seq, 0)->t)
        result --;
    }
    lasttime = tsequence_inst_n(seq, seq->count - 1)->t;
    first = false;
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the n-th distinct timestamp of a temporal sequence set.
 * @sqlfunc timestampN()
 */
bool
tsequenceset_timestamp_n(const TSequenceSet *ss, int n, TimestampTz *result)
{
  bool found = false;
  if (n < 1)
    return false;
  if (n == 1)
  {
    *result = tsequence_inst_n(tsequenceset_seq_n(ss, 0), 0)->t;
    return true;
  }

  /* Continue the search 0-based */
  n--;
  TimestampTz prev, next;
  bool first = true;
  int i = 0, count = 0, prevcount = 0;
  while (i < ss->count)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    count += seq->count;
    if (! first && prev == tsequence_inst_n(seq, 0)->t)
    {
        prevcount --;
        count --;
    }
    if (prevcount <= n && n < count)
    {
      next = tsequence_inst_n(seq, n - prevcount)->t;
      found = true;
      break;
    }
    prevcount = count;
    prev = tsequence_inst_n(seq, seq->count - 1)->t;
    first = false;
    i++;
  }
  if (! found)
    return false;
  *result = next;
  return true;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the array of distinct timestamps of a temporal sequence set.
 * @sqlfunc timestamps()
 */
TimestampTz *
tsequenceset_timestamps(const TSequenceSet *ss, int *count)
{
  TimestampTz *result = palloc(sizeof(TimestampTz) * ss->totalcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tsequence_timestamps1(seq, &result[k]);
  }
  if (k > 1)
  {
    timestamparr_sort(result, k);
    k = timestamparr_remove_duplicates(result, k);
  }
  *count = k;
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the base value of a temporal sequence set at a timestamp.
 *
 * @param[in] ss Temporal sequence set
 * @param[in] t Timestamp
 * @param[in] strict True if exclusive bounds are taken into account
 * @param[out] result Base value
 * @result Return true if the timestamp is contained in the temporal sequence set
 * @pre A bounding box test has been done before by the calling function
 * @sqlfunc valueAtTimestamp()
 */
bool
tsequenceset_value_at_timestamp(const TSequenceSet *ss, TimestampTz t,
  bool strict, Datum *result)
{
  /* Return the value even when the timestamp is at an exclusive bound */
  if (! strict)
  {
    /* Singleton sequence set */
    if (ss->count == 1)
      return tsequence_value_at_timestamp(tsequenceset_seq_n(ss, 0), t, false,
        result);

    for (int i = 0; i < ss->count; i++)
    {
      const TSequence *seq = tsequenceset_seq_n(ss, i);
      /* Test whether the timestamp is at one of the bounds */
      const TInstant *inst = tsequence_inst_n(seq, 0);
      if (inst->t == t)
        return tinstant_value_at_timestamp(inst, t, result);
      inst = tsequence_inst_n(seq, seq->count - 1);
      if (inst->t == t)
        return tinstant_value_at_timestamp(inst, t, result);
      /* Call the function on the sequence with strict set to true */
      if (contains_period_timestamp(&seq->period, t))
        return tsequence_value_at_timestamp(seq, t, true, result);
    }
    /* Since this function is always called with a timestamp that appears
     * in the sequence set the next statement is never reached */
    return false;
  }

  /* Singleton sequence set */
  if (ss->count == 1)
    return tsequence_value_at_timestamp(tsequenceset_seq_n(ss, 0), t, true, result);

  /* General case */
  int loc;
  if (! tsequenceset_find_timestamp(ss, t, &loc))
    return false;
  return tsequence_value_at_timestamp(tsequenceset_seq_n(ss, loc), t, true, result);
}

/*****************************************************************************
 * Cast functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_cast
 * @brief Cast a temporal sequence set integer to a temporal sequence set float.
 * @sqlop @p ::
 */
TSequenceSet *
tintseqset_to_tfloatseqset(const TSequenceSet *ss)
{
  TSequenceSet *result = tsequenceset_copy(ss);
  result->temptype = T_TFLOAT;
  MOBDB_FLAGS_SET_CONTINUOUS(result->flags, true);
  MOBDB_FLAGS_SET_INTERP(result->flags, STEPWISE);
  for (int i = 0; i < ss->count; i++)
  {
    TSequence *seq = (TSequence *) tsequenceset_seq_n(result, i);
    seq->temptype = T_TFLOAT;
    for (int j = 0; j < seq->count; j++)
    {
      TInstant *inst = (TInstant *) tsequence_inst_n(seq, j);
      inst->temptype = T_TFLOAT;
      inst->value = Float8GetDatum((double)DatumGetInt32(tinstant_value(inst)));
    }
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_cast
 * @brief Cast a temporal sequence set float to a temporal sequence set integer.
 * @sqlop @p ::
 */
TSequenceSet *
tfloatseqset_to_tintseqset(const TSequenceSet *ss)
{
  if (MOBDB_FLAGS_GET_LINEAR(ss->flags))
    elog(ERROR, "Cannot cast temporal float with linear interpolation to temporal integer");
  TSequenceSet *result = tsequenceset_copy(ss);
  result->temptype = T_TINT;
  MOBDB_FLAGS_SET_CONTINUOUS(result->flags, false);
  MOBDB_FLAGS_SET_INTERP(result->flags, STEPWISE);
  for (int i = 0; i < ss->count; i++)
  {
    TSequence *seq = (TSequence *) tsequenceset_seq_n(result, i);
    seq->temptype = T_TINT;
    for (int j = 0; j < seq->count; j++)
    {
      TInstant *inst = (TInstant *) tsequence_inst_n(seq, j);
      inst->temptype = T_TINT;
      inst->value = Int32GetDatum((double)DatumGetFloat8(tinstant_value(inst)));
    }
  }
  return result;
}

/*****************************************************************************
 * Transformation functions
 *****************************************************************************/

#if MEOS
/**
 * @ingroup libmeos_internal_temporal_constructor
 * @brief Return a copy of a temporal sequence set without any extra space.
 * @note We cannot simply test whether ss->count == ss->maxcount since there
 * could be extra space allocated for the (variable-length) sequences
 */
TSequenceSet *
tsequenceset_compact(const TSequenceSet *ss)
{
  TSequence **sequences = palloc0(sizeof(TSequence *) * ss->count);
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    sequences[i] = tsequence_compact(seq);
  }
  return tsequenceset_make_free(sequences, ss->count, NORMALIZE_NO);
}
#endif /* MEOS */

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Return a temporal instant transformed into a temporal sequence set.
 * @sqlfunc tbool_seqset(), tint_seqset(), tfloat_seqset(), ttext_seqset(), etc.
 */
TSequenceSet *
tinstant_to_tsequenceset(const TInstant *inst, interpType interp)
{
  assert(interp == STEPWISE || interp == LINEAR);
  TSequence *seq = tinstant_to_tsequence(inst, interp);
  TSequenceSet *result = tsequence_to_tsequenceset(seq);
  pfree(seq);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Return a temporal discrete sequence transformed into a temporal
 * sequence set.
 * @sqlfunc tbool_seqset(), tint_seqset(), tfloat_seqset(), ttext_seqset(), etc.
 */
TSequenceSet *
tdiscseq_to_tsequenceset(const TSequence *seq, interpType interp)
{
  assert(interp == STEPWISE || interp == LINEAR);
  TSequence **sequences = palloc(sizeof(TSequence *) * seq->count);
  for (int i = 0; i < seq->count; i++)
  {
    const TInstant *inst = tsequence_inst_n(seq, i);
    sequences[i] = tinstant_to_tsequence(inst, interp);
  }
  TSequenceSet *result = tsequenceset_make((const TSequence **) sequences,
    seq->count, NORMALIZE_NO);
  pfree(sequences);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Return a temporal sequence set transformed into discrete interpolation.
 * @return Return an error if any of the composing temporal sequences has
 * more than one instant
 * @sqlfunc tbool_discseq(), tint_discseq(), tfloat_discseq(), ttext_discseq(),
 * etc.
 */
TSequence *
tsequenceset_to_tdiscseq(const TSequenceSet *ss)
{
  const TSequence *seq;
  for (int i = 0; i < ss->count; i++)
  {
    seq = tsequenceset_seq_n(ss, i);
    if (seq->count != 1)
      elog(ERROR, "Cannot transform input to a temporal discrete sequence");
  }

  const TInstant **instants = palloc(sizeof(TInstant *) * ss->count);
  for (int i = 0; i < ss->count; i++)
  {
    seq = tsequenceset_seq_n(ss, i);
    instants[i] = tsequence_inst_n(seq, 0);
  }
  TSequence *result = tsequence_make(instants, ss->count, true, true, DISCRETE,
    NORMALIZE_NO);
  pfree(instants);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Return a temporal sequence transformed into a temporal sequence set.
 * @sqlfunc tbool_seqset(), tint_seqset(), tfloat_seqset(), ttext_seqset(), etc.
 */
TSequenceSet *
tsequence_to_tsequenceset(const TSequence *seq)
{
  assert(seq);
  if (MOBDB_FLAGS_GET_DISCRETE(seq->flags))
  {
    interpType interp = MOBDB_FLAGS_GET_CONTINUOUS(seq->flags) ? LINEAR : STEPWISE;
    return tdiscseq_to_tsequenceset(seq, interp);
  }
  return tsequenceset_make(&seq, 1, NORMALIZE_NO);
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Return a temporal sequence set with continuous base type from
 * stepwise to linear interpolation.
 * @sqlfunc toLinear()
 */
TSequenceSet *
tstepseqset_to_linear(const TSequenceSet *ss)
{
  /* Singleton sequence set */
  if (ss->count == 1)
    return tstepseq_to_linear(tsequenceset_seq_n(ss, 0));

  /* General case */
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->totalcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tstepseq_to_linear1(seq, &sequences[k]);
  }
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Return a temporal sequence set shifted and/or scaled by the intervals.
 * @pre The duration is greater than 0 if it is not NULL
 * @sqlfunc shift(), tscale(), shiftTscale().
 */
TSequenceSet *
tsequenceset_shift_tscale(const TSequenceSet *ss, const Interval *shift,
  const Interval *duration)
{
  assert(shift != NULL || duration != NULL);

  /* Copy the input sequence set to the result */
  TSequenceSet *result = tsequenceset_copy(ss);

  /* Determine the shift and/or the scale values */
  Span p1, p2;
  const TSequence *seq1 = tsequenceset_seq_n(ss, 0);
  const TSequence *seq2 = tsequenceset_seq_n(ss, ss->count - 1);
  const TInstant *inst1 = tsequence_inst_n(seq1, 0);
  const TInstant *inst2 = tsequence_inst_n(seq2, seq2->count - 1);
  span_set(TimestampTzGetDatum(inst1->t), TimestampTzGetDatum(inst2->t),
    seq1->period.lower_inc, seq2->period.upper_inc, T_TIMESTAMPTZ, &p1);
  span_set(p1.lower, p1.upper, p1.lower_inc, p1.upper_inc, T_TIMESTAMPTZ, &p2);
  period_shift_tscale(&p2, shift, duration);
  TimestampTz delta;
  if (shift != NULL)
    delta = p2.lower - p1.lower;
  double scale;
  bool instant = (p2.lower == p2.upper);
  /* If the sequence set is instantaneous we cannot scale */
  if (duration != NULL && ! instant)
    scale = (double) (p2.upper - p2.lower) / (double) (p1.upper - p1.lower);

  /* Shift and/or scale each composing sequence */
  for (int i = 0; i < ss->count; i++)
  {
    TSequence *seq = (TSequence *) tsequenceset_seq_n(result, i);
    /* Shift and/or scale the bounding period of the sequence */
    if (shift != NULL && (duration == NULL || seq->count == 1))
    {
      seq->period.lower += delta;
      seq->period.upper += delta;
    }
    /* If the sequence is instantaneous we cannot scale */
    if (duration != NULL && seq->count > 1)
    {
      seq->period.lower = p2.lower + (seq->period.lower - p1.lower) * scale;
      seq->period.upper = p2.lower + (seq->period.upper - p1.lower) * scale;
    }
    /* Shift and/or scale each composing instant */
    for (int j = 0; j < seq->count; j++)
    {
      TInstant *inst = (TInstant *) tsequence_inst_n(seq, j);
      /* Shift and/or scale the bounding period of the sequence */
      if (shift != NULL)
        inst->t += delta;
      /* If the sequence is instantaneous we cannot scale */
      if (duration != NULL && seq->count > 1)
        inst->t = p2.lower + (inst->t - p2.lower) * scale;
    }
  }
  return result;
}

/*****************************************************************************
 * Ever/always functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_ever
 * @brief Return true if a temporal sequence set is ever equal to a base value.
 * @sqlop @p ?=
 */
bool
tsequenceset_ever_eq(const TSequenceSet *ss, Datum value)
{
  /* Bounding box test */
  if (! temporal_bbox_ev_al_eq((Temporal *)ss, value, EVER))
    return false;

  for (int i = 0; i < ss->count; i++)
    if (tsequence_ever_eq(tsequenceset_seq_n(ss, i), value))
      return true;
  return false;
}

/**
 * @ingroup libmeos_internal_temporal_ever
 * @brief Return true if a temporal sequence set is always equal to a base value.
 * @sqlop @p %=
 */
bool
tsequenceset_always_eq(const TSequenceSet *ss, Datum value)
{
  /* Bounding box test */
  if (! temporal_bbox_ev_al_eq((Temporal *)ss, value, ALWAYS))
    return false;

  /* The bounding box test above is enough to compute the answer for
   * temporal numbers */
  if (tnumber_type(ss->temptype))
    return true;

  for (int i = 0; i < ss->count; i++)
    if (! tsequence_always_eq(tsequenceset_seq_n(ss, i), value))
      return false;
  return true;
}

/*****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_ever
 * @brief Return true if a temporal sequence set is ever less than a base value.
 * @sqlop @p ?<
 */
bool
tsequenceset_ever_lt(const TSequenceSet *ss, Datum value)
{
  /* Bounding box test */
  if (! temporal_bbox_ev_al_lt_le((Temporal *)ss, value, EVER))
    return false;

  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    if (tsequence_ever_lt(seq, value))
      return true;
  }
  return false;
}

/**
 * @ingroup libmeos_internal_temporal_ever
 * @brief Return true if a temporal sequence set is ever less than or equal
 * to a base value.
 * @sqlop @p ?<=
 */
bool
tsequenceset_ever_le(const TSequenceSet *ss, Datum value)
{
  /* Bounding box test */
  if (! temporal_bbox_ev_al_lt_le((Temporal *)ss, value, EVER))
    return false;

  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    if (tsequence_ever_le(seq, value))
      return true;
  }
  return false;
}

/**
 * @ingroup libmeos_internal_temporal_ever
 * @brief Return true if a temporal sequence set is always less than a base value.
 * @sqlop @p %<
 */
bool
tsequenceset_always_lt(const TSequenceSet *ss, Datum value)
{
  /* Bounding box test */
  if (! temporal_bbox_ev_al_lt_le((Temporal *)ss, value, ALWAYS))
    return false;

  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    if (! tsequence_always_lt(seq, value))
      return false;
  }
  return true;
}

/**
 * @ingroup libmeos_internal_temporal_ever
 * @brief Return true if a temporal sequence set is always less than or equal
 * to a base value.
 * @sqlop @p %<=
 */
bool
tsequenceset_always_le(const TSequenceSet *ss, Datum value)
{
  /* Bounding box test */
  if (! temporal_bbox_ev_al_lt_le((Temporal *)ss, value, ALWAYS))
    return false;

  /* The bounding box test above is enough to compute
   * the answer for temporal numbers */
  if (tnumber_type(ss->temptype))
    return true;

  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    if (! tsequence_always_le(seq, value))
      return false;
  }
  return true;
}

/*****************************************************************************
 * Restriction Functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal sequence set to (the complement of) a base value.
 *
 * @note There is no bounding box test in this function, it is done in the
 * dispatch function for all temporal types.
 * @sqlfunc atValue(), minusValue()
 */
TSequenceSet *
tsequenceset_restrict_value(const TSequenceSet *ss, Datum value, bool atfunc)
{
  /* Singleton sequence set */
  if (ss->count == 1)
    return tcontseq_restrict_value(tsequenceset_seq_n(ss, 0), value, atfunc);

  /* General case */
  int count = ss->totalcount;
  /* For minus and linear interpolation we need the double of the count */
  if (! atfunc && MOBDB_FLAGS_GET_LINEAR(ss->flags))
    count *= 2;
  TSequence **sequences = palloc(sizeof(TSequence *) * count);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tcontseq_restrict_value1(seq, value, atfunc, &sequences[k]);
  }
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal sequence set to (the complement of) an array of
 * base values.
 *
 * @param[in] ss Temporal sequence set
 * @param[in] set Set of base values
 * @param[in] atfunc True if the restriction is at, false for minus
 * @pre There are no duplicates values in the array
 * @sqlfunc atValues(), minusValues()
 */
TSequenceSet *
tsequenceset_restrict_values(const TSequenceSet *ss, const Set *set,
  bool atfunc)
{
  /* Singleton sequence set */
  if (ss->count == 1)
    return tcontseq_restrict_values(tsequenceset_seq_n(ss, 0), set, atfunc);

  /* General case
   * Compute the AT function */
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->totalcount *
     set->count);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tsequence_at_values1(seq, set, &sequences[k]);
  }
  TSequenceSet *atresult = tsequenceset_make_free(sequences, k, NORMALIZE);
  if (atfunc)
    return atresult;

  /*
   * MINUS function
   * Compute the complement of the previous value.
   */
  if (k == 0)
    return tsequenceset_copy(ss);

  SpanSet *ps1 = tsequenceset_time(ss);
  SpanSet *ps2 = tsequenceset_time(atresult);
  SpanSet *ps = minus_spanset_spanset(ps1, ps2);
  TSequenceSet *result = NULL;
  if (ps != NULL)
  {
    result = tsequenceset_restrict_periodset(ss, ps, REST_AT);
    pfree(ps);
  }
  pfree(atresult); pfree(ps1); pfree(ps2);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal number to a span of base values.
 *
 * @note It is supposed that a bounding box test has been done in the dispatch
 * function.
 * @sqlfunc atSpan(), minusSpan()
 */
TSequenceSet *
tnumberseqset_restrict_span(const TSequenceSet *ss, const Span *span,
  bool atfunc)
{
  /* Singleton sequence set */
  if (ss->count == 1)
    return tnumbercontseq_restrict_span(tsequenceset_seq_n(ss, 0), span, atfunc);

  /* General case */
  int count = ss->totalcount;
  /* For minus and linear interpolation we need the double of the count */
  if (! atfunc && MOBDB_FLAGS_GET_LINEAR(ss->flags))
    count *= 2;
  TSequence **sequences = palloc(sizeof(TSequence *) * count);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tnumbercontseq_restrict_span2(seq, span, atfunc, &sequences[k]);
  }
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal number to (the complement of) an array of
 * spans of base values
 *
 * @param[in] ss Temporal number
 * @param[in] spanset Span set
 * @param[in] atfunc True if the restriction is at, false for minus
 * @return Resulting temporal number value
 * @sqlfunc atSpanset(), minusSpanset()
 */
TSequenceSet *
tnumberseqset_restrict_spanset(const TSequenceSet *ss, const SpanSet *spanset,
  bool atfunc)
{
  /* Singleton sequence set */
  if (ss->count == 1)
    return tnumbercontseq_restrict_spanset(tsequenceset_seq_n(ss, 0),
      spanset, atfunc);

  /* General case */
  int maxcount = ss->totalcount * spanset->count;
  /* For minus and linear interpolation we need the double of the count */
  if (! atfunc && MOBDB_FLAGS_GET_LINEAR(ss->flags))
    maxcount *= 2;
  TSequence **sequences = palloc(sizeof(TSequence *) * maxcount);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    k += tnumbercontseq_restrict_spanset1(seq, spanset, atfunc,
      &sequences[k]);
  }
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal sequence set to (the complement of) its
 * minimum/maximum base value
 *
 * @param[in] ss Temporal sequence set
 * @param[in] min True if restricted to the minumum value, false for the
 * maximum value
 * @param[in] atfunc True if the restriction is at, false for minus
 * @sqlfunc atMin(), atMax(), minusMin(), minusMax()
 */
TSequenceSet *
tsequenceset_restrict_minmax(const TSequenceSet *ss, bool min, bool atfunc)
{
  Datum minmax = min ? tsequenceset_min_value(ss) : tsequenceset_max_value(ss);
  return tsequenceset_restrict_value(ss, minmax, atfunc);
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal sequence set to (the complement of) a timestamp.
 * @sqlfunc atTimestamp(), minusTimestamp()
 */
Temporal *
tsequenceset_restrict_timestamp(const TSequenceSet *ss, TimestampTz t,
  bool atfunc)
{
  /* Bounding box test */
  if (! contains_period_timestamp(&ss->period, t))
    return atfunc ? NULL : (Temporal *) tsequenceset_copy(ss);

  /* Singleton sequence set */
  if (ss->count == 1)
    return atfunc ?
      (Temporal *) tcontseq_at_timestamp(tsequenceset_seq_n(ss, 0), t) :
      (Temporal *) tcontseq_minus_timestamp(tsequenceset_seq_n(ss, 0), t);

  /* General case */
  const TSequence *seq;
  if (atfunc)
  {
    int loc;
    if (! tsequenceset_find_timestamp(ss, t, &loc))
      return NULL;
    seq = tsequenceset_seq_n(ss, loc);
    return (Temporal *) tsequence_at_timestamp(seq, t);
  }
  else
  {
    /* At most one composing sequence can be split into two */
    TSequence **sequences = palloc(sizeof(TSequence *) * (ss->count + 1));
    int k = 0;
    int i;
    for (i = 0; i < ss->count; i++)
    {
      seq = tsequenceset_seq_n(ss, i);
      k += tcontseq_minus_timestamp1(seq, t, &sequences[k]);
      if (t < (TimestampTz) seq->period.upper)
      {
        i++;
        break;
      }
    }
    /* Copy the remaining sequences if went out of the for loop with the break */
    for (int j = i; j < ss->count; j++)
      sequences[k++] = tsequence_copy(tsequenceset_seq_n(ss, j));
    /* k is never equal to 0 since in that case it is a singleton sequence set
       and it has been dealt by tcontseq_minus_timestamp above */
    return (Temporal *) tsequenceset_make_free(sequences, k, NORMALIZE_NO);
  }
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal sequence set to (the complement of) a timestamp set.
 * @sqlfunc atTstzSet(), minusTstzSet()
 */
Temporal *
tsequenceset_restrict_timestampset(const TSequenceSet *ss, const Set *ts,
  bool atfunc)
{
  /* Singleton timestamp set */
  if (ts->count == 1)
  {
    Temporal *temp = tsequenceset_restrict_timestamp(ss,
      DatumGetTimestampTz(set_val_n(ts, 0)), atfunc);
    if (atfunc && temp != NULL)
    {
      Temporal *result = (Temporal *) tinstant_to_tsequence(
        (const TInstant *) temp, DISCRETE);
      pfree(temp);
      return result;
    }
    return temp;
  }

  /* Bounding box test */
  Span s;
  set_set_span(ts, &s);
  if (! overlaps_span_span(&ss->period, &s))
    return atfunc ? NULL : (Temporal *) tsequenceset_copy(ss);

  /* Singleton sequence set */
  if (ss->count == 1)
    return atfunc ?
      (Temporal *) tcontseq_at_timestampset(tsequenceset_seq_n(ss, 0), ts) :
      (Temporal *) tcontseq_minus_timestampset(tsequenceset_seq_n(ss, 0), ts);

  /* General case */
  const TSequence *seq;
  if (atfunc)
  {
    TInstant **instants = palloc(sizeof(TInstant *) * ts->count);
    int count = 0;
    int i = 0, j = 0;
    while (i < ts->count && j < ss->count)
    {
      seq = tsequenceset_seq_n(ss, j);
      TimestampTz t = DatumGetTimestampTz(set_val_n(ts, i));
      if (contains_period_timestamp(&seq->period, t))
      {
        instants[count++] = tsequence_at_timestamp(seq, t);
        i++;
      }
      else
      {
        if (t <= (TimestampTz) seq->period.lower)
          i++;
        if (t >= (TimestampTz) seq->period.upper)
          j++;
      }
    }
    return (Temporal *) tsequence_make_free(instants, count, true, true,
      DISCRETE, NORMALIZE_NO);
  }
  else
  {
    /* For the minus case each timestamp will split at most one
     * composing sequence into two */
    TSequence **sequences = palloc(sizeof(TSequence *) *
      (ss->count + ts->count + 1));
    int k = 0;
    for (int i = 0; i < ss->count; i++)
    {
      seq = tsequenceset_seq_n(ss, i);
      k += tcontseq_minus_timestampset1(seq, ts, &sequences[k]);

    }
    return (Temporal *) tsequenceset_make_free(sequences, k, NORMALIZE);
  }
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal sequence set to (the complement of) a period.
 * @sqlfunc atTime(), minusTime()
 */
TSequenceSet *
tsequenceset_restrict_period(const TSequenceSet *ss, const Span *p,
  bool atfunc)
{
  /* Bounding box test */
  if (! overlaps_span_span(&ss->period, p))
    return atfunc ? NULL : tsequenceset_copy(ss);

  TSequence *seq;
  TSequenceSet *result;

  /* Singleton sequence set */
  if (ss->count == 1)
  {
    if (atfunc)
    {
      seq = tcontseq_at_period(tsequenceset_seq_n(ss, 0), p);
      result = tsequence_to_tsequenceset(seq);
      pfree(seq);
      return result;
    }
    else
      return tcontseq_minus_period(tsequenceset_seq_n(ss, 0), p);
  }

  /* General case */
  if (atfunc)
  {
    /* AT */
    int loc;
    tsequenceset_find_timestamp(ss, p->lower, &loc);
    /* We are sure that loc < ss->count due to the bounding period test above */
    TSequence **sequences = palloc(sizeof(TSequence *) * (ss->count - loc));
    TSequence *tofree[2];
    int k = 0, l = 0;
    for (int i = loc; i < ss->count; i++)
    {
      seq = (TSequence *) tsequenceset_seq_n(ss, i);
      if (contains_span_span(p, &seq->period))
        sequences[k++] = seq;
      else if (overlaps_span_span(p, &seq->period))
      {
        TSequence *newseq = tcontseq_at_period(seq, p);
        sequences[k++] = tofree[l++] = newseq;
      }
      int cmp = timestamptz_cmp_internal(DatumGetTimestampTz(p->upper),
        DatumGetTimestampTz(seq->period.upper));
      if (cmp < 0 || (cmp == 0 && seq->period.upper_inc))
        break;
    }
    if (k == 0)
    {
      pfree(sequences);
      return NULL;
    }
    /* Since both the tsequenceset and the period are normalized it is not
     * necessary to normalize the result of the projection */
    result = tsequenceset_make((const TSequence **) sequences, k, NORMALIZE_NO);
    for (int i = 0; i < l; i++)
      pfree(tofree[i]);
    pfree(sequences);
    return result;
  }
  else
  {
    /* MINUS */
    SpanSet *ps = tsequenceset_time(ss);
    SpanSet *resultps = minus_spanset_span(ps, p);
    result = NULL;
    if (resultps != NULL)
    {
      result = tsequenceset_restrict_periodset(ss, resultps, REST_AT);
      pfree(resultps);
    }
    pfree(ps);
    return result;
  }
}

/**
 * @ingroup libmeos_internal_temporal_restrict
 * @brief Restrict a temporal sequence set to (the complement of) a period set.
 * @sqlfunc atTime(), minusTime()
 */
TSequenceSet *
tsequenceset_restrict_periodset(const TSequenceSet *ss, const SpanSet *ps,
  bool atfunc)
{
  /* Singleton period set */
  if (ps->count == 1)
    return tsequenceset_restrict_period(ss, spanset_sp_n(ps, 0), atfunc);

  /* Bounding box test */
  if (! overlaps_span_span(&ss->period, &ps->span))
    return atfunc ? NULL : tsequenceset_copy(ss);

  /* Singleton sequence set */
  if (ss->count == 1)
    return tcontseq_restrict_periodset(tsequenceset_seq_n(ss, 0), ps, atfunc);

  /* General case */
  TSequence **sequences;
  int i = 0, j = 0, k = 0;
  if (atfunc)
  {
    TimestampTz t = Max(DatumGetTimestampTz(ss->period.lower),
      DatumGetTimestampTz(ps->span.lower));
    tsequenceset_find_timestamp(ss, t, &i);
    spanset_find_value(ps, DatumGetTimestampTz(t), &j);
    sequences = palloc(sizeof(TSequence *) * (ss->count + ps->count - i - j));
  }
  else
    sequences = palloc(sizeof(TSequence *) * (ss->count + ps->count));
  while (i < ss->count && j < ps->count)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    const Span *p = spanset_sp_n(ps, j);
    /* The sequence and the period do not overlap */
    if (left_span_span(&seq->period, p))
    {
      if (! atfunc)
        /* Copy the sequence */
        sequences[k++] = tsequence_copy(seq);
      i++;
    }
    else if (overlaps_span_span(&seq->period, p))
    {
      if (atfunc)
      {
        /* Compute the restriction of the sequence and the period */
        TSequence *seq1 = tcontseq_at_period(seq, p);
        if (seq1 != NULL)
          sequences[k++] = seq1;
        int cmp = timestamptz_cmp_internal(DatumGetTimestampTz(seq->period.upper),
          DatumGetTimestampTz(p->upper));
        if (cmp == 0 && seq->period.upper_inc == p->upper_inc)
        {
          i++; j++;
        }
        else if (cmp < 0 ||
          (cmp == 0 && ! seq->period.upper_inc && p->upper_inc))
          i++;
        else
          j++;
      }
      else
      {
        /* Compute the difference of the sequence and the FULL periodset.
         * Notice that we cannot compute the difference with the
         * current period without replicating the functionality in
         * tcontseq_minus_periodset1 */
        k += tcontseq_minus_periodset1(seq, ps, j, &sequences[k]);
        i++;
      }
    }
    else
      j++;
  }
  if (! atfunc)
  {
    /* For minus copy the sequences after the period set */
    while (i < ss->count)
      sequences[k++] = tsequence_copy(tsequenceset_seq_n(ss, i++));
  }
  /* It is necessary to normalize despite the fact that both the tsequenceset
  * and the periodset are normalized */
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/*****************************************************************************
 * Append and merge functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Append an instant to a temporal sequence set.
 * @param[in,out] ss Temporal sequence set
 * @param[in] inst Temporal instant
 * @param[in] expand True when reserving space for additional instants
 * @sqlfunc appendInstant()
 */
TSequenceSet *
tsequenceset_append_tinstant(TSequenceSet *ss, const TInstant *inst, bool expand)
{
  assert(ss->temptype == inst->temptype);
  TSequence *seq = (TSequence *) tsequenceset_seq_n(ss, ss->count - 1);
  Temporal *temp = tsequence_append_tinstant(seq, inst, expand);
  TSequenceSet *ss1;

#if MEOS
  /* Account for expandable structures
   * A while is used instead of an if to enable to break the loop if there is
   * no more available space */
  int count = (temp->subtype == TSEQUENCE) ? ss->count : ss->count + 1;
  while (expand && count <= ss->maxcount)
  {
    size_t size = 0;
    /* Append the new sequence(s) if there is enough space: only one if the
     * instant was appended to the last sequence, or the two sequences
     * composing the sequence set that results from appending the instant */
    const TSequence *newseq1, *newseq2 = NULL;
    if (count == ss->count)
      size += double_pad(VARSIZE(temp));
    else /* temp->subtype == TSEQUENCESET */
    {
      ss1 = (TSequenceSet *) temp;
      newseq1 = tsequenceset_seq_n(ss1, 0);
      newseq2 = tsequenceset_seq_n(ss1, 1);
      size += double_pad(VARSIZE(newseq1));
      size += double_pad(VARSIZE(newseq2));
    }
    /* Remove the size of the current last sequence */
    TSequence *last = (TSequence *) tsequenceset_seq_n(ss, ss->count - 1);
    size_t avail_size = ((char *) ss + VARSIZE(ss)) -
      ((char *) last + double_pad(VARSIZE(last)));
    if (size > avail_size)
      break;
    /* Update the offsets array and the counts when adding two sequences */
    if (count != ss->count)
    {
      (tsequenceset_offsets_ptr(ss))[count - 1] =
        (tsequenceset_offsets_ptr(ss))[count - 2] + double_pad(VARSIZE(newseq1));
      ss->count++;
      ss->totalcount++;
    }
    /* There is enough space to add the new sequence(s) */
    if (count == ss->count)
    {
      memset(last, 0, size);
      memcpy(last, temp, VARSIZE(temp));
    }
    else /* temp->subtype == TSEQUENCESET */
    {
      memset(last, 0, size);
      memcpy(last, newseq1, VARSIZE(newseq1));
      last = (TSequence *) ((char *) last + double_pad(VARSIZE(newseq1)));
      memcpy(last, newseq2, VARSIZE(newseq2));
    }
    /* Expand the bounding box and return */
    if (count == ss->count)
      tsequenceset_expand_bbox(ss, (TSequence *) temp);
    else /* temp->subtype == TSEQUENCESET */
    {
      tsequenceset_expand_bbox(ss, newseq1);
      tsequenceset_expand_bbox(ss, newseq2);
    }
    return ss;
  }
#endif /* MEOS */

  /* This is the first time we use an expandable structure or there is no more
   * free space */
  const TSequence **sequences = palloc(sizeof(TSequence *) * ss->count + 1);
  int k = 0;
  for (int i = 0; i < ss->count - 1; i++)
    sequences[k++] = tsequenceset_seq_n(ss, i);
  assert(temp->subtype == TSEQUENCE || temp->subtype == TSEQUENCESET);
  if (temp->subtype == TSEQUENCE)
    sequences[k++] = (const TSequence *) temp;
  else /* temp->subtype == TSEQUENCESET */
  {
    ss1 = (TSequenceSet *) temp;
    sequences[k++] = tsequenceset_seq_n(ss1, 0);
    sequences[k++] = tsequenceset_seq_n(ss1, 1);
  }
  TSequenceSet *result = tsequenceset_make(sequences, k, NORMALIZE_NO);
  pfree(sequences);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Append a sequence to a temporal sequence set.
 * @param[in,out] ss Temporal sequence set
 * @param[in] seq Temporal sequence
 * @param[in] expand True when reserving space for additional sequences
 * @sqlfunc appendSequence()
 * @note It is the responsibility of the calling function to free the memory,
 * that is, delete the old value of ss when it cannot be expanded and a new
 * sequence set is created.
 */
TSequenceSet *
tsequenceset_append_tsequence(TSequenceSet *ss, const TSequence *seq,
  bool expand)
{
  /* Ensure validity of the arguments */
  assert(ss->temptype == seq->temptype);
  /* The last sequence below may be modified with expandable structures */
  TSequence *last = (TSequence *) tsequenceset_seq_n(ss, ss->count - 1);
  const TInstant *inst1 = tsequence_inst_n(last, last->count - 1);
  const TInstant *inst2 = tsequence_inst_n(seq, 0);
  /* We cannot call ensure_increasing_timestamps since we must take into
   * account inclusive/exclusive bounds */
  char *t1;
  if (inst1->t > inst2->t)
  {
    t1 = pg_timestamptz_out(inst1->t);
    char *t2 = pg_timestamptz_out(inst2->t);
    elog(ERROR, "Timestamps for temporal value must be increasing: %s, %s",
      t1, t2);
  }
  else if (inst1->t == inst2->t && ss->period.upper_inc &&
    seq->period.lower_inc)
  {
    meosType basetype = temptype_basetype(ss->temptype);
    Datum value1 = tinstant_value(inst1);
    Datum value2 = tinstant_value(inst2);
    if (! datum_eq(value1, value2, basetype))
    {
      t1 = pg_timestamptz_out(inst1->t);
      elog(ERROR, "The temporal values have different value at their common timestamp %s", t1);
    }
  }

  bool removelast, removefirst;
  bool join = tsequence_join_test(last, seq, &removelast, &removefirst);
  /* We are sure that the result will be a SINGLE sequence */
  TSequence *newseq = (! join) ? NULL :
    (TSequence *) tsequence_append_tsequence(last, seq, expand);
  int count = (join) ? ss->count : ss->count + 1;

#if MEOS
  /* Account for expandable structures
   * A while is used instead of an if to be able to break the loop if there is
   * not enough available space to append the new sequence */
  while (expand && count <= ss->maxcount)
  {
    /* Determine whether there is enough available space */
    size_t size = 0;
    if (join)
      size += double_pad(VARSIZE(newseq)) - double_pad(VARSIZE(last));
    else
      size += double_pad(VARSIZE(seq));
    /* Remove the size of the current last sequence */
    size_t avail_size = ((char *) ss + VARSIZE(ss)) -
      ((char *) last + double_pad(VARSIZE(last)));
    if (size > avail_size)
      /* There is not enough available space */
      break;

    /* There is enough space to add the new sequence */
    if (! join)
    {
      /* Update the offsets array and the counts when adding one sequence */
      (tsequenceset_offsets_ptr(ss))[count - 1] =
        (tsequenceset_offsets_ptr(ss))[count - 2] + double_pad(VARSIZE(last));
      ss->count++;
      ss->totalcount += seq->count;
    }
    if (join)
    {
      /* Set to 0 in case the new sequence is smaller than the current one */
      memset(last, 0, VARSIZE(last));
      memcpy(last, newseq, VARSIZE(newseq));
    }
    else
    {
      last = (TSequence *) ((char *) last + double_pad(VARSIZE(last)));
      memcpy(last, newseq, VARSIZE(newseq));
    }
    /* Expand the bounding box and return */
    tsequenceset_expand_bbox(ss, seq);
    return ss;
  }
#endif /* MEOS */

  /* This is the first time we use an expandable structure or there is not
   * enough available space */
  const TSequence **sequences = palloc(sizeof(TSequence *) * count);
  int k = 0;
  for (int i = 0; i < ss->count - 1; i++)
    sequences[k++] = tsequenceset_seq_n(ss, i);
  if (join)
    sequences[k++] = newseq;
  else
  {
    sequences[k++] = tsequenceset_seq_n(ss, ss->count - 1);
    sequences[k++] = seq;
  }
  int maxcount = count;
  if (expand && count > ss->maxcount)
  {
    maxcount = ss->maxcount * 2;
#ifdef DEBUG_BUILD
    printf(" seqset -> %d\n", maxcount);
#endif /* DEBUG_BUILD */
  }
  TSequenceSet *result = tsequenceset_make1_exp(sequences, count, maxcount,
    NORMALIZE_NO);
  pfree(sequences);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Merge two temporal sequence sets
 * @sqlfunc merge()
 */
TSequenceSet *
tsequenceset_merge(const TSequenceSet *ss1, const TSequenceSet *ss2)
{
  const TSequenceSet *seqsets[] = {ss1, ss2};
  return tsequenceset_merge_array(seqsets, 2);
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Merge an array of temporal sequence sets.
 *
 * The values in the array may overlap in a single instant.
 * @param[in] seqsets Array of sequence sets
 * @param[in] count Number of elements in the array
 * @sqlfunc merge()
 */
TSequenceSet *
tsequenceset_merge_array(const TSequenceSet **seqsets, int count)
{
  /* Validity test will be done in tsequence_merge_array */
  /* Collect the composing sequences */
  int totalcount = 0;
  for (int i = 0; i < count; i++)
    totalcount += seqsets[i]->count;
  const TSequence **sequences = palloc0(sizeof(TSequence *) * totalcount);
  int k = 0;
  for (int i = 0; i < count; i++)
  {
    for (int j = 0; j < seqsets[i]->count; j++)
      sequences[k++] = tsequenceset_seq_n(seqsets[i], j);
  }
  /* We cannot call directly tsequence_merge_array since the result must always
   * be of subtype TSEQUENCESET */
  int newcount;
  TSequence **newseqs = tsequence_merge_array1(sequences, totalcount, &newcount);
  return tsequenceset_make_free(newseqs, newcount, NORMALIZE);
}

/*****************************************************************************
 * Synchronize functions
 *****************************************************************************/

/**
 * @brief Temporally intersect or synchronize a temporal sequence set and a temporal
 * sequence
 *
 * The resulting values are composed of denormalized sequences
 * covering the intersection of their time spans
 *
 * @param[in] ss,seq Input values
 * @param[in] mode Enumeration for either intersect or synchronize
 * @param[out] inter1, inter2 Output values
 * @result Return false if the input values do not overlap on time
 */
bool
synchronize_tsequenceset_tsequence(const TSequenceSet *ss, const TSequence *seq,
  SyncMode mode, TSequenceSet **inter1, TSequenceSet **inter2)
{
  /* Bounding period test */
  if (! overlaps_span_span(&ss->period, &seq->period))
    return false;

  int loc;
  tsequenceset_find_timestamp(ss, seq->period.lower, &loc);
  /* We are sure that loc < ss->count due to the bounding period test above */
  TSequence **sequences1 = palloc(sizeof(TSequence *) * ss->count - loc);
  TSequence **sequences2 = palloc(sizeof(TSequence *) * ss->count - loc);
  int k = 0;
  for (int i = loc; i < ss->count; i++)
  {
    const TSequence *seq1 = tsequenceset_seq_n(ss, i);
    TSequence *interseq1, *interseq2;
    bool hasinter;
    /* mode == SYNCHRONIZE or SYNCHRONIZE_CROSS */
    hasinter = synchronize_tsequence_tsequence(seq, seq1,
      &interseq1, &interseq2, mode == SYNCHRONIZE_CROSS);
    if (hasinter)
    {
      sequences1[k] = interseq1;
      sequences2[k++] = interseq2;
    }
    int cmp = timestamptz_cmp_internal(DatumGetTimestampTz(seq->period.upper),
      DatumGetTimestampTz(seq1->period.upper));
    if (cmp < 0 ||
      (cmp == 0 && (!seq->period.upper_inc || seq1->period.upper_inc)))
      break;
  }
  if (k == 0)
  {
    pfree(sequences1); pfree(sequences2);
    return false;
  }

  *inter1 = tsequenceset_make_free(sequences1, k, NORMALIZE_NO);
  *inter2 = tsequenceset_make_free(sequences2, k, NORMALIZE_NO);
  return true;
}

/**
 * @brief Temporally intersect or synchronize two temporal sequence sets
 *
 * @param[in] ss1,ss2 Input values
 * @param[in] mode Intersection or synchronization (with or without adding crossings)
 * @param[out] inter1, inter2 Output values
 * @result Return false if the input values do not overlap on time
 */
bool
synchronize_tsequenceset_tsequenceset(const TSequenceSet *ss1,
  const TSequenceSet *ss2, SyncMode mode, TSequenceSet **inter1,
  TSequenceSet **inter2)
{
  /* Bounding period test */
  if (! overlaps_span_span(&ss1->period, &ss2->period))
    return false;

  int count = ss1->count + ss2->count;
  TSequence **sequences1 = palloc(sizeof(TSequence *) * count);
  TSequence **sequences2 = palloc(sizeof(TSequence *) * count);
  int i = 0, j = 0, k = 0;
  while (i < ss1->count && j < ss2->count)
  {
    const TSequence *seq1 = tsequenceset_seq_n(ss1, i);
    const TSequence *seq2 = tsequenceset_seq_n(ss2, j);
    TSequence *interseq1, *interseq2;
    bool hasinter;
    /* mode == SYNCHRONIZE or SYNCHRONIZE_CROSS */
    hasinter = synchronize_tsequence_tsequence(seq1, seq2,
      &interseq1, &interseq2, mode == SYNCHRONIZE_CROSS);
    if (hasinter)
    {
      sequences1[k] = interseq1;
      sequences2[k++] = interseq2;
    }
    int cmp = timestamptz_cmp_internal(DatumGetTimestampTz(seq1->period.upper),
      DatumGetTimestampTz(seq2->period.upper));
    if (cmp == 0 && seq1->period.upper_inc == seq2->period.upper_inc)
    {
      i++; j++;
    }
    else if (cmp < 0 ||
      (cmp == 0 && ! seq1->period.upper_inc && seq2->period.upper_inc))
      i++;
    else
      j++;
  }
  if (k == 0)
  {
    pfree(sequences1); pfree(sequences2);
    return false;
  }

  *inter1 = tsequenceset_make_free(sequences1, k, NORMALIZE_NO);
  *inter2 = tsequenceset_make_free(sequences2, k, NORMALIZE_NO);
  return true;
}

/*****************************************************************************
 * Intersection functions
 *****************************************************************************/

/**
 * @brief Temporally intersect two temporal values
 *
 * @param[in] ss,inst Input values
 * @param[out] inter1, inter2 Output values
 * @result Return false if the input values do not overlap on time
 */
bool
intersection_tsequenceset_tinstant(const TSequenceSet *ss, const TInstant *inst,
  TInstant **inter1, TInstant **inter2)
{
  TInstant *inst1 = (TInstant *)
    tsequenceset_restrict_timestamp(ss, inst->t, REST_AT);
  if (inst1 == NULL)
    return false;

  *inter1 = inst1;
  *inter2 = tinstant_copy(inst);
  return true;
}

/**
 * @brief Temporally intersect two temporal values
 *
 * @param[in] inst,ss Input values
 * @param[out] inter1, inter2 Output values
 * @result Return false if the input values do not overlap on time
 */
bool
intersection_tinstant_tsequenceset(const TInstant *inst, const TSequenceSet *ss,
  TInstant **inter1, TInstant **inter2)
{
  return intersection_tsequenceset_tinstant(ss, inst, inter2, inter1);
}

/**
 * @brief Temporally intersect two temporal values
 *
 * @param[in] ss,seq Input values
 * @param[out] inter1, inter2 Output values
 * @result Return false if the input values do not overlap on time
 */
bool
intersection_tsequenceset_tdiscseq(const TSequenceSet *ss,
  const TSequence *seq, TSequence **inter1, TSequence **inter2)
{
  /* Bounding period test */
  if (! overlaps_span_span(&ss->period, &seq->period))
    return false;

  TInstant **instants1 = palloc(sizeof(TInstant *) * seq->count);
  const TInstant **instants2 = palloc(sizeof(TInstant *) * seq->count);
  int i = 0, j = 0, k = 0;
  while (i < ss->count && j < seq->count)
  {
    const TSequence *seq1 = tsequenceset_seq_n(ss, i);
    const TInstant *inst = tsequence_inst_n(seq, j);
    if (contains_period_timestamp(&seq1->period, inst->t))
    {
      instants1[k] = tsequence_at_timestamp(seq1, inst->t);
      instants2[k++] = inst;
    }
    int cmp = timestamptz_cmp_internal(DatumGetTimestampTz(seq1->period.upper),
      inst->t);
    if (cmp == 0)
    {
      i++; j++;
    }
    else if (cmp < 0)
      i++;
    else
      j++;
  }
  if (k == 0)
  {
    pfree(instants1); pfree(instants2);
    return false;
  }

  *inter1 = tsequence_make_free(instants1, k, true, true, DISCRETE,
    NORMALIZE_NO);
  *inter2 = tsequence_make(instants2, k, true, true, DISCRETE,
    NORMALIZE_NO);
  pfree(instants2);
  return true;
}

/**
 * @brief Temporally intersect two temporal values
 *
 * @param[in] seq,ss Input values
 * @param[out] inter1,inter2 Output values
 * @result Return false if the input values do not overlap on time
 */
bool
intersection_tdiscseq_tsequenceset(const TSequence *seq,
  const TSequenceSet *ss, TSequence **inter1, TSequence **inter2)
{
  return intersection_tsequenceset_tdiscseq(ss, seq, inter2, inter1);
}

/**
 * @brief Temporally intersect or synchronize two temporal values
 *
 * @param[in] seq,ss Input values
 * @param[in] mode Enumeration for either intersect or synchronize
 * @param[out] inter1, inter2 Output values
 * @result Return false if the input values do not overlap on times
 */
bool
intersection_tsequence_tsequenceset(const TSequence *seq, const TSequenceSet *ss,
  SyncMode mode, TSequenceSet **inter1, TSequenceSet **inter2)
{
  return synchronize_tsequenceset_tsequence(ss, seq, mode, inter2, inter1);
}

/*****************************************************************************
 * Input/output functions
 *****************************************************************************/

#if MEOS
/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return a temporal sequence set from its Well-Known Text (WKT) representation.
 *
 * @param[in] str String
 * @param[in] temptype Temporal type
 * @param[in] interp Interpolation
 */
TSequenceSet *
tsequenceset_in(const char *str, meosType temptype, interpType interp)
{
  return tsequenceset_parse(&str, temptype, interp);
}
/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return a temporal sequence set boolean from its Well-Known Text (WKT)
 * representation.
 */
TSequenceSet *
tboolseqset_in(const char *str)
{
  return tsequenceset_parse(&str, T_TBOOL, true);
}

/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return a temporal sequence set integer from its Well-Known Text (WKT)
 * representation.
 */
TSequenceSet *
tintseqset_in(const char *str)
{
  return tsequenceset_parse(&str, T_TINT, true);
}

/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return a temporal sequence set float from its Well-Known Text (WKT)
 * representation.
 */
TSequenceSet *
tfloatseqset_in(const char *str)
{
  /* Call the superclass function to read the interpolation at the beginning (if any) */
  Temporal *temp = temporal_parse(&str, T_TFLOAT);
  assert (temp->subtype == TSEQUENCE);
  return (TSequenceSet *) temp;
}

/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return a temporal sequence set text from its Well-Known Text (WKT)
 * representation.
 */
TSequenceSet *
ttextseqset_in(const char *str)
{
  return tsequenceset_parse(&str, T_TTEXT, true);
}

/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return a temporal sequence set geometric point from its Well-Known Text
 * (WKT) representation.
 */
TSequenceSet *
tgeompointseqset_in(const char *str)
{
  /* Call the superclass function to read the SRID at the beginning (if any) */
  Temporal *temp = tpoint_parse(&str, T_TGEOMPOINT);
  assert (temp->subtype == TSEQUENCESET);
  return (TSequenceSet *) temp;
}

/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return a temporal sequence set geographic point from its Well-Known Text
 * (WKT) representation.
 */
TSequenceSet *
tgeogpointseqset_in(const char *str)
{
  /* Call the superclass function to read the SRID at the beginning (if any) */
  Temporal *temp = tpoint_parse(&str, T_TGEOGPOINT);
  assert (temp->subtype == TSEQUENCESET);
  return (TSequenceSet *) temp;
}
#endif

/**
 * @brief Return the Well-Known Text (WKT) representation of a temporal sequence set.
 *
 * @param[in] ss Temporal sequence set
 * @param[in] maxdd Maximum number of decimal digits to output for floating point
 * values
 * @param[in] value_out Function called to output the base value
 */
char *
tsequenceset_to_string(const TSequenceSet *ss, int maxdd, outfunc value_out)
{
  char **strings = palloc(sizeof(char *) * ss->count);
  size_t outlen = 0;
  char prefix[20];
  if (MOBDB_FLAGS_GET_CONTINUOUS(ss->flags) &&
      ! MOBDB_FLAGS_GET_LINEAR(ss->flags))
    sprintf(prefix, "Interp=Stepwise;");
  else
    prefix[0] = '\0';
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    strings[i] = tsequence_to_string(seq, maxdd, true, value_out);
    outlen += strlen(strings[i]) + 1;
  }
  return stringarr_to_string(strings, ss->count, outlen, prefix, '{', '}',
    QUOTES_NO, SPACES);
}

/**
 * @ingroup libmeos_internal_temporal_inout
 * @brief Return the Well-Known Text (WKT) representation of a temporal sequence set.
 */
char *
tsequenceset_out(const TSequenceSet *ss, int maxdd)
{
  return tsequenceset_to_string(ss, maxdd, &basetype_out);
}

/*****************************************************************************
 * Modification functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_modif
 * @brief Insert the second temporal value into the first one.
 */
TSequenceSet *
tsequenceset_insert(const TSequenceSet *ss1, const TSequenceSet *ss2)
{
  TSequenceSet *result;
  const TInstant *instants[2] = {0};
  interpType interp = MOBDB_FLAGS_GET_INTERP(ss1->flags);
  int count;

  /* Order the two sequence sets */
  const TSequence *seq1 = tsequenceset_seq_n(ss1, 0);
  const TSequence *seq2 = tsequenceset_seq_n(ss2, 0);
  const TSequenceSet *ss; /* for swaping */
  if (left_span_span(&seq2->period, &seq1->period))
  {
    ss = ss1; ss1 = ss2; ss2 = ss;
  }

  /* Singleton sequence sets */
  if (ss1->count == 1 && ss2->count == 1)
  {
    Temporal *temp = tcontseq_insert(seq1, seq2);
    if (temp->subtype == TSEQUENCESET)
      return (TSequenceSet *) temp;
    result = tsequence_to_tsequenceset((TSequence *) temp);
    pfree(temp);
    return result;
  }

  /* If one sequence set is before the other one add the potential gap between
   * the two and call directly the merge function */
  if (left_span_span(&ss1->period, &ss2->period))
  {
    if (ss1->period.upper_inc && ss2->period.lower_inc)
    {
      seq1 = tsequenceset_seq_n(ss1, ss1->count - 1);
      seq2 = tsequenceset_seq_n(ss2, 0);
      instants[0] = tsequence_inst_n(seq1, seq1->count - 1);
      instants[1] = tsequence_inst_n(seq2, 0);
      count = (timestamptz_cmp_internal(instants[0]->t, instants[1]->t) == 0) ?
        1 : 2;
      TSequence *seq = tsequence_make(instants, count, true, true, interp,
        NORMALIZE_NO);
      TSequenceSet *gap = tsequence_to_tsequenceset(seq);
      pfree(seq);
      const TSequenceSet *seqsets[] = {ss1, gap, ss2};
      return tsequenceset_merge_array(seqsets, 3);
    }
    else
    {
      const TSequenceSet *seqsets[] = {ss1, ss2};
      return tsequenceset_merge_array(seqsets, 2);
    }
  }

  /*
   * ss1   |---|         |---|         |---|
   * ss2          |---|         |---|
   * additional sequences
   *           |--|   |--|   |--|   |--|
   */
  count = ss1->count + ss2->count + Min(ss1->count, ss2->count) * 2;
  const TSequence **sequences = palloc(sizeof(TSequence *) * count);
  TSequence **tofree = palloc(sizeof(TSequence *) *
    Min(ss1->count, ss2->count) * 2);
  meosType basetype = temptype_basetype(ss1->temptype);
  /* Add the first sequence of ss1 to the result */
  sequences[0] = tsequenceset_seq_n(ss1, 0);
  int i = 1, /* counter for the first sequence */
    j = 0,   /* counter for the second sequence */
    k = 1,   /* counter for the sequences in the result */
    l = 0;   /* counter for the new sequences to be freed */
  while (i < ss1->count && j < ss2->count)
  {
    seq1 = tsequenceset_seq_n(ss1, i);
    seq2 = tsequenceset_seq_n(ss2, j);
    int cmp1 = timestamptz_cmp_internal(sequences[k - 1]->period.upper,
      seq2->period.lower);
    int cmp2 = timestamptz_cmp_internal(seq2->period.upper, seq1->period.lower);
    /* If seq2 is between the last sequence added and seq1 */
    if (cmp1 <= 0 && cmp2 <= 0)
    {
      /* Verify that the two sequences have the same value at common instants */
      const TInstant *inst1, *inst2;
      if (cmp1 == 0 && sequences[k - 1]->period.upper_inc &&
          seq2->period.lower_inc)
      {
        inst1 = tsequence_inst_n(sequences[k - 1], sequences[k - 1]->count - 1);
        inst2 = tsequence_inst_n(seq2, 0);
        if (! datum_eq(tinstant_value(inst1), tinstant_value(inst2), basetype))
        {
          char *str = pg_timestamptz_out(inst1->t);
          elog(ERROR, "The temporal values have different value at their common instant %s", str);
        }
      }
      if (cmp2 == 0 && seq2->period.upper_inc && seq1->period.lower_inc)
      {
        inst1 = tsequence_inst_n(seq2, seq2->count - 1);
        inst2 = tsequence_inst_n(seq1, 0);
        if (! datum_eq(tinstant_value(inst1), tinstant_value(inst2), basetype))
        {
          char *str = pg_timestamptz_out(inst1->t);
          elog(ERROR, "The temporal values have different value at their common instant %s", str);
        }
      }
      /* Fill the gap between the last sequence added and seq2 */
      if (sequences[k - 1]->period.upper_inc && seq2->period.lower_inc)
      {
        instants[0] = tsequence_inst_n(sequences[k - 1],
          sequences[k - 1]->count - 1);
        instants[1] = tsequence_inst_n(seq2, 0);
        count = (timestamptz_cmp_internal(instants[0]->t, instants[1]->t) == 0) ?
          1 : 2;
        /* We put true so that it works with stepwise interpolation */
        tofree[l] = tsequence_make(instants, count, true, true, interp,
          NORMALIZE_NO);
        sequences[k++] = (const TSequence *) tofree[l++];
      }
      /* Add seq2 */
      sequences[k++] = seq2;
      /* Fill the gap between the seq2 and seq1 */
      if (seq2->period.upper_inc && seq1->period.lower_inc)
      {
        instants[0] = tsequence_inst_n(seq2, seq2->count - 1);
        instants[1] = tsequence_inst_n(seq1, 0);
        count = (timestamptz_cmp_internal(instants[0]->t, instants[1]->t) == 0) ?
          1 : 2;
        /* We put true so that it works with stepwise interpolation */
        tofree[l] = tsequence_make(instants, count, true, true, interp,
          NORMALIZE_NO);
        sequences[k++] = (const TSequence *) tofree[l++];
      }
      i++;
      j++;
    }
    else /* consume seq1 and advance i */
    {
      sequences[k++] = seq1;
      i++;
    }
  }
  /* Add the remaining sequences */
  while (i < ss1->count)
    sequences[k++] = tsequenceset_seq_n(ss1, i++);
  while (j < ss2->count)
    sequences[k++] = tsequenceset_seq_n(ss2, j++);
  /* Construct the result */
  int newcount;
  TSequence **normseqs = tseqarr_normalize(sequences, k, &newcount);
  result = tsequenceset_make_free(normseqs, newcount, NORMALIZE_NO);
  pfree_array((void **) tofree, l);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_modif
 * @brief Update the first temporal value with the second one.
 */
TSequenceSet *
tsequenceset_update(const TSequenceSet *ss1, const TSequenceSet *ss2)
{ // TODO BBOX TEST
  SpanSet *ps = tsequenceset_time(ss2);
  TSequenceSet *rest = tsequenceset_restrict_periodset(ss1, ps, REST_MINUS);
  pfree(ps);
  if (! rest)
    return tsequenceset_copy(ss2);
  TSequenceSet *result = tsequenceset_insert(rest, ss2);
  pfree(rest);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Delete a timestamp from a temporal sequence set.
 *
 * @param[in] ss Temporal sequence set
 * @param[in] t Timestamp
 * @sqlfunc atTime(), minusTime()
 */
TSequenceSet *
tsequenceset_delete_timestamp(const TSequenceSet *ss, TimestampTz t)
{
  /* Bounding box test */
  if (! contains_period_timestamp(&ss->period, t))
    return tsequenceset_copy(ss);

  TSequence *seq1;

  /* Singleton sequence set */
  if (ss->count == 1)
  {
    TSequenceSet *result = NULL;
    seq1 = tcontseq_delete_timestamp(tsequenceset_seq_n(ss, 0), t);
    if (seq1)
    {
      result = tsequence_to_tsequenceset(seq1);
      pfree(seq1);
    }
    return result;
  }

  /* General case */
  TSequence **sequences = palloc(sizeof(TSequence *) * (ss->count));
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    seq1 = tcontseq_delete_timestamp(seq, t);
    if (seq1)
      sequences[k++] = seq1;
  }
  return tsequenceset_make_free(sequences, k, NORMALIZE_NO);
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Restrict a temporal sequence set to (the complement of) a timestamp set.
 * @sqlfunc atTime(), minusTime()
 */
TSequenceSet *
tsequenceset_delete_timestampset(const TSequenceSet *ss,
  const Set *ts)
{
  /* Singleton timestamp set */
  if (ts->count == 1)
    return tsequenceset_delete_timestamp(ss,
      DatumGetTimestampTz(set_val_n(ts, 0)));

  /* Bounding box test */
  Span s;
  set_set_span(ts, &s);
  if (! overlaps_span_span(&ss->period, &s))
    return tsequenceset_copy(ss);

  TSequence *seq1;

  /* Singleton sequence set */
  if (ss->count == 1)
  {
    TSequenceSet *result = NULL;
    seq1 = tcontseq_delete_timestampset(tsequenceset_seq_n(ss, 0), ts);
    if (seq1)
    {
      result = tsequence_to_tsequenceset(seq1);
      pfree(seq1);
    }
    return result;
  }

  /* General case */
  TSequence **sequences = palloc(sizeof(TSequence *) * ss->count);
  int k = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    seq1 = tcontseq_delete_timestampset(seq, ts);
    if (seq1)
      sequences[k++] = seq1;
  }
  return tsequenceset_make_free(sequences, k, NORMALIZE);
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Delete a period from a temporal sequence set.
 * @sqlfunc deleteTime()
 */
TSequenceSet *
tsequenceset_delete_period(const TSequenceSet *ss, const Span *p)
{
  SpanSet *ps = span_to_spanset(p);
  TSequenceSet *result = tsequenceset_delete_periodset(ss, ps);
  pfree(ps);
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_transf
 * @brief Delete a period from a temporal sequence set.
 * @sqlfunc deleteTime()
 */
TSequenceSet *
tsequenceset_delete_periodset(const TSequenceSet *ss, const SpanSet *ps)
{
  /* Bounding box test */
  if (! overlaps_span_span(&ss->period, &ps->span))
    return tsequenceset_copy(ss);

  TSequence *seq;
  TSequenceSet *result = NULL;

  /* Singleton sequence set */
  if (ss->count == 1)
  {
    seq = tcontseq_delete_periodset(tsequenceset_seq_n(ss, 0), ps);
    if (seq)
    {
      result = tsequence_to_tsequenceset(seq);
      pfree(seq);
    }
    return result;
  }

  /* General case */
  TSequenceSet *minus = tsequenceset_restrict_periodset(ss, ps, REST_MINUS);
  /* The are minus->count - 1 holes that may be filled */
  TSequence **sequences = palloc(sizeof(TSequence *) * (minus->count * 2 - 1));
  TSequence **tofree = palloc(sizeof(TSequence *) * (minus->count - 1));
  const TInstant *instants[2] = {0};
  interpType interp = MOBDB_FLAGS_GET_INTERP(ss->flags);
  sequences[0] = seq = (TSequence *) tsequenceset_seq_n(minus, 0);
  const Span *p = spanset_sp_n(ps, 0);
  int i = 1, /* current composing sequence */
    j = 0,   /* current composing period */
    k = 1,   /* number of sequences in the currently constructed sequence */
    l = 0;   /* number of sequences to be freed */  /* Fill the gaps */
  /* Skip all composing periods that are before or adjacent to seq */
  while (j < ps->count)
  {
    if (timestamptz_cmp_internal(DatumGetTimestampTz(p->upper),
          DatumGetTimestampTz(seq->period.lower)) > 0)
      break;
    p = spanset_sp_n(ps, ++j);
  }
  seq = (TSequence *) tsequenceset_seq_n(minus, 1);
  while (i < ss->count && j < ps->count)
  {
    if (timestamptz_cmp_internal(DatumGetTimestampTz(p->upper),
          DatumGetTimestampTz(seq->period.lower) <= 0))
    {
      instants[0] = tsequence_inst_n(sequences[k - 1],
        sequences[k - 1]->count - 1);
      instants[1] = tsequence_inst_n(seq, 0);
      int count = (timestamptz_cmp_internal(instants[0]->t, instants[1]->t) == 0) ?
        1 : 2;
      /* We put true so that it works with stepwise interpolation */
      tofree[l] = tsequence_make(instants, count, true, true, interp,
        NORMALIZE_NO);
      sequences[k++] = tofree[l++];
    }
    sequences[k++] = seq;
    seq = (TSequence *) tsequenceset_seq_n(minus, ++i);
    p = spanset_sp_n(ps, ++j);
  }
  /* Add remaining sequences to the result */
  while (i < ss->count)
    sequences[k++] = (TSequence *) tsequenceset_seq_n(minus, i++);
  /* Construct the result */
  int newcount;
  TSequence **normseqs = tseqarr_normalize((const TSequence **) sequences, k,
    &newcount);
  result = tsequenceset_make_free(normseqs, newcount, NORMALIZE_NO);
  pfree_array((void **) tofree, l);
  return result;
}

/*****************************************************************************
 * Intersects functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_time
 * @brief Return true if a temporal sequence set intersects a timestamp.
 * @sqlfunc intersectsTimestamp()
 */
bool
tsequenceset_overlaps_timestamp(const TSequenceSet *ss, TimestampTz t)
{
  int loc;
  if (tsequenceset_find_timestamp(ss, t, &loc))
    return true;
  return false;
}

/**
 * @ingroup libmeos_internal_temporal_time
 * @brief Return true if a temporal sequence set intersects a timestamp set.
 * @sqlfunc intersectsTstzSet()
 */
bool
tsequenceset_overlaps_timestampset(const TSequenceSet *ss,
  const Set *ss1)
{
  for (int i = 0; i < ss1->count; i++)
    if (tsequenceset_overlaps_timestamp(ss,
        DatumGetTimestampTz(set_val_n(ss1, i))))
      return true;
  return false;
}

/**
 * @ingroup libmeos_internal_temporal_time
 * @brief Return true if a temporal sequence set intersects a period.
 * @sqlfunc intersectsPeriod()
 */
bool
tsequenceset_overlaps_period(const TSequenceSet *ss, const Span *p)
{
  /* Binary search of lower and upper bounds of period */
  int loc1, loc2;
  if (tsequenceset_find_timestamp(ss, p->lower, &loc1) ||
    tsequenceset_find_timestamp(ss, p->upper, &loc2))
    return true;

  for (int i = loc1; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    if (overlaps_span_span(&seq->period, p))
      return true;
    if (p->upper < seq->period.upper)
      break;
  }
  return false;
}

/**
 * @ingroup libmeos_internal_temporal_time
 * @brief Return true if a temporal sequence set intersects a period set.
 * @sqlfunc intersectsPeriod()
 */
bool
tsequenceset_overlaps_periodset(const TSequenceSet *ss, const SpanSet *ps)
{
  for (int i = 0; i < ps->count; i++)
    if (tsequenceset_overlaps_period(ss, spanset_sp_n(ps, i)))
      return true;
  return false;
}

/*****************************************************************************
 * Local aggregate functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_agg
 * @brief Return the integral (area under the curve) of a temporal number
 */
double
tnumberseqset_integral(const TSequenceSet *ss)
{
  double result = 0;
  for (int i = 0; i < ss->count; i++)
    result += tnumberseq_integral(tsequenceset_seq_n(ss, i));
  return result;
}

/**
 * @brief Return the duration of a temporal sequence set as a double
 */
static double
tsequenceset_interval_double(const TSequenceSet *ss)
{
  double result = 0;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    result += (double) (seq->period.upper - seq->period.lower);
  }
  return result;
}

/**
 * @ingroup libmeos_internal_temporal_agg
 * @brief Return the time-weighted average of a temporal number
 * @sqlfunc twAvg()
 */
double
tnumberseqset_twavg(const TSequenceSet *ss)
{
  double duration = tsequenceset_interval_double(ss);
  double result;
  if (duration == 0.0)
  {
    result = 0;
    for (int i = 0; i < ss->count; i++)
      result += tnumbercontseq_twavg(tsequenceset_seq_n(ss, i));
    return result / ss->count;
  }
  else
    result = tnumberseqset_integral(ss) / duration;
  return result;
}

/*****************************************************************************
 * Comparison functions
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_comp
 * @brief Return true if two temporal sequence sets are equal.
 *
 * @pre The arguments are of the same base type
 * @note The internal B-tree comparator is not used to increase efficiency
 * @sqlop @p =
 */
bool
tsequenceset_eq(const TSequenceSet *ss1, const TSequenceSet *ss2)
{
  assert(ss1->temptype == ss2->temptype);
  /* If number of sequences or flags are not equal */
  if (ss1->count != ss2->count || ss1->flags != ss2->flags)
    return false;

  /* If bounding boxes are not equal */
  if (! temporal_bbox_eq(TSEQUENCESET_BBOX_PTR(ss1),
      TSEQUENCESET_BBOX_PTR(ss2), ss1->temptype))
    return false;

  /* Compare the composing sequences */
  for (int i = 0; i < ss1->count; i++)
  {
    const TSequence *seq1 = tsequenceset_seq_n(ss1, i);
    const TSequence *seq2 = tsequenceset_seq_n(ss2, i);
    if (! tsequence_eq(seq1, seq2))
      return false;
  }
  return true;
}

/**
 * @ingroup libmeos_internal_temporal_comp
 * @brief Return -1, 0, or 1 depending on whether the first Temporal sequence set
 * is less than, equal, or greater than the second one.
 *
 * @pre The arguments are of the same base type
 * @note Period and bounding box comparison have been done by the calling
 * function @ref temporal_cmp
 * @sqlfunc tbool_cmp(), tint_cmp(), tfloat_cmp(), ttext_cmp(), etc.
 */
int
tsequenceset_cmp(const TSequenceSet *ss1, const TSequenceSet *ss2)
{
  assert(ss1->temptype == ss2->temptype);

  /* Compare composing instants */
  int count = Min(ss1->count, ss2->count);
  for (int i = 0; i < count; i++)
  {
    const TSequence *seq1 = tsequenceset_seq_n(ss1, i);
    const TSequence *seq2 = tsequenceset_seq_n(ss2, i);
    int result = tsequence_cmp(seq1, seq2);
    if (result)
      return result;
  }

  /* ss1->count == ss2->count because of the bounding box and the
   * composing sequence tests above */

  /* ss1->flags == ss2->flags since all the composing sequences are equal */

  /* The two values are equal */
  return 0;
}

/*****************************************************************************
 * Function for defining hash index
 * The function reuses the approach for array types for combining the hash of
 * the elements.
 *****************************************************************************/

/**
 * @ingroup libmeos_internal_temporal_accessor
 * @brief Return the 32-bit hash value of a temporal sequence set.
 * @sqlfunc tbool_hash(), tint_hash(), tfloat_hash(), ttext_hash(), etc.
 */
uint32
tsequenceset_hash(const TSequenceSet *ss)
{
  uint32 result = 1;
  for (int i = 0; i < ss->count; i++)
  {
    const TSequence *seq = tsequenceset_seq_n(ss, i);
    uint32 seq_hash = tsequence_hash(seq);
    result = (result << 5) - result + seq_hash;
  }
  return result;
}

/*****************************************************************************/
