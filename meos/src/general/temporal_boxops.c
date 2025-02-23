/*****************************************************************************
 *
 * This MobilityDB code is provided under The PostgreSQL License.
 * Copyright (c) 2016-2024, Université libre de Bruxelles and MobilityDB
 * contributors
 *
 * MobilityDB includes portions of PostGIS version 3 source code released
 * under the GNU General Public License (GPLv2 or later).
 * Copyright (c) 2001-2024, PostGIS contributors
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
 * @file
 * @brief Bounding box operators for temporal types
 *
 * The bounding box of temporal values are
 * - a @p Span for temporal Booleans
 * - a @p TBox for temporal integers and floats, where the @p x coordinate is
 *   for the value dimension and the @p t coordinate is for the time dimension.
 * The following operators are defined: @p overlaps, @p contains, @p contained,
 * @p same, and @p adjacent.
 *
 * The operators consider as many dimensions as they are shared in both
 * arguments: only the value dimension, only the time dimension, or both
 * the value and the time dimensions.
 */

#include "general/temporal_boxops.h"

/* C */
#include <assert.h>
#include <limits.h>
/* PostgreSQL */
#include <utils/timestamp.h>
/* MEOS */
#include <meos.h>
#include <meos_internal.h>
#include "general/span.h"
#include "general/type_util.h"
#include "general/type_util.h"
#include "point/tpoint_boxops.h"
#if NPOINT
  #include "npoint/tnpoint_boxops.h"
#endif

/*****************************************************************************
 * Functions on generic bounding boxes of temporal types
 *****************************************************************************/

/**
 * @brief Return true if the type is a bounding box type
 */
bool
bbox_type(meosType bboxtype)
{
  if (bboxtype == T_TSTZSPAN || bboxtype == T_TBOX || bboxtype == T_STBOX)
    return true;
  return false;
}

/**
 * @brief Return the size of a bounding box type
 */
size_t
bbox_get_size(meosType bboxtype)
{
  assert(bbox_type(bboxtype));
  if (bboxtype == T_TSTZSPAN)
    return sizeof(Span);
  if (bboxtype == T_TBOX)
    return sizeof(TBox);
  else /* bboxtype == T_STBOX */
    return sizeof(STBox);
}

/**
 * @brief Return the maximum number of dimensions of a bounding box type
 */
int
bbox_max_dims(meosType bboxtype)
{
  assert(bbox_type(bboxtype));
  if (bboxtype == T_TSTZSPAN)
    return 1;
  if (bboxtype == T_TBOX)
    return 2;
  else /* bboxtype == T_STBOX */
    return 4;
}

/**
 * @brief Return true if two bounding boxes are equal
 * @param[in] box1,box2 Bounding boxes
 * @param[in] temptype Temporal type
 */
bool
temporal_bbox_eq(const void *box1, const void *box2, meosType temptype)
{
  assert(temporal_type(temptype));
  if (talpha_type(temptype))
    return span_eq_int((Span *) box1, (Span *) box2);
  if (tnumber_type(temptype))
    return tbox_eq((TBox *) box1, (TBox *) box2);
  if (tspatial_type(temptype))
    // TODO Due to floating point precision the current statement
    // is not equal to the next one.
    // return stbox_eq((STBox *) box1, (STBox *) box2);
    // Problem raised in the test file 51_tpoint_tbl.test.out
    // Look for temp != merge in that file for 2 other cases where
    // a problem still remains (result != 0) even with the _cmp function
    return stbox_cmp((STBox *) box1, (STBox *) box2) == 0;
  meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
    "Unknown temporal type for bounding box function: %d", temptype);
  return false;
}

/**
 * @brief Return -1, 0, or 1 depending on whether the first bounding box
 * is less than, equal, or greater than the second one
 * @param[in] box1,box2 Bounding boxes
 * @param[in] temptype Temporal type
 * @return On error return @p INT_MAX
 */
int
temporal_bbox_cmp(const void *box1, const void *box2, meosType temptype)
{
  assert(temporal_type(temptype));
  if (talpha_type(temptype))
    return span_cmp_int((Span *) box1, (Span *) box2);
  if (tnumber_type(temptype))
    return tbox_cmp((TBox *) box1, (TBox *) box2);
  if (tspatial_type(temptype))
    return stbox_cmp((STBox *) box1, (STBox *) box2);
  meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
    "Unknown temporal type for bounding box function: %d", temptype);
  return INT_MAX;
}

/*****************************************************************************
 * Compute the bounding box at the creation of temporal values
 *****************************************************************************/

/**
 * @brief Return the size of a bounding box of a temporal type
 * @return On error return SIZE_MAX
 */
size_t
temporal_bbox_size(meosType temptype)
{
  if (talpha_type(temptype))
    return sizeof(Span);
  if (tnumber_type(temptype))
    return sizeof(TBox);
  if (tspatial_type(temptype))
    return sizeof(STBox);
  meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
    "Unknown temporal type for bounding box function: %d", temptype);
  return SIZE_MAX; /* make compiler quiet */
}

/**
 * @ingroup meos_internal_temporal_accessor
 * @brief Return the last argument initialized with the bounding box of a
 * temporal instant
 * @param[in] inst Temporal value
 * @param[out] box Result
 */
void
tnumberinst_set_tbox(const TInstant *inst, TBox *box)
{
  assert(inst); assert(temporal_type(inst->temptype)); assert(box);
  assert(tnumber_type(inst->temptype));
  meosType basetype = temptype_basetype(inst->temptype);
  meosType spantype = basetype_spantype(basetype);
    Datum value = tinstant_val(inst);
  Datum time = TimestampTzGetDatum(inst->t);
  TBox *tbox = (TBox *) box;
  memset(tbox, 0, sizeof(TBox));
  span_set(value, value, true, true, basetype, spantype, &tbox->span);
  span_set(time, time, true, true, T_TIMESTAMPTZ, T_TSTZSPAN, &tbox->period);
  MEOS_FLAGS_SET_X(tbox->flags, true);
  MEOS_FLAGS_SET_T(tbox->flags, true);
  return;
}

/**
 * @ingroup meos_internal_temporal_accessor
 * @brief Return the last argument initialized with the bounding box of a
 * temporal instant
 * @param[in] inst Temporal value
 * @param[out] box Result
 */
void
tinstant_set_bbox(const TInstant *inst, void *box)
{
  assert(inst); assert(temporal_type(inst->temptype)); assert(box);
  if (talpha_type(inst->temptype))
    span_set(TimestampTzGetDatum(inst->t), TimestampTzGetDatum(inst->t),
      true, true, T_TIMESTAMPTZ, T_TSTZSPAN, (Span *) box);
  else if (tnumber_type(inst->temptype))
    tnumberinst_set_tbox(inst, (TBox *) box);
  else if (tgeo_type(inst->temptype))
    tpointinst_set_stbox(inst, (STBox *) box);
#if NPOINT
  else if (inst->temptype == T_TNPOINT)
    tnpointinst_set_stbox(inst, (STBox *) box);
#endif
  else
    meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
      "Unknown temporal type for bounding box function: %d", inst->temptype);
  return;
}

/**
 * @ingroup meos_internal_temporal_accessor
 * @brief Return the last argument initialized with the temporal box of a
 * temporal sequence number
 * @param[in] seq Temporal sequence
 * @param[out] box Temporal box
 */
void
tnumberseq_set_tbox(const TSequence *seq, TBox *box)
{
  assert(seq); assert(box); assert(tnumber_type(seq->temptype));
  memcpy(box, TSEQUENCE_BBOX_PTR(seq), sizeof(TBox));
  return;
}

/**
 * @ingroup meos_internal_temporal_bbox
 * @brief Return the last argument initialized with the bounding box of a
 * temporal sequence
 * @param[in] seq Temporal sequence
 * @param[out] box Bounding box
 */
void
tsequence_set_bbox(const TSequence *seq, void *box)
{
  assert(seq); assert(box);
  memset(box, 0, seq->bboxsize);
  memcpy(box, TSEQUENCE_BBOX_PTR(seq), seq->bboxsize);
  return;
}

/**
 * @ingroup meos_internal_temporal_accessor
 * @brief Return the last argument initialized with the temporal box of a
 * temporal sequence number
 * @param[in] ss Temporal sequence set
 * @param[out] box Temporal box
 */
void
tnumberseqset_set_tbox(const TSequenceSet *ss, TBox *box)
{
  assert(ss); assert(box); assert(tnumber_type(ss->temptype));
  memcpy(box, TSEQUENCESET_BBOX_PTR(ss), sizeof(TBox));
  return;
}

/**
 * @ingroup meos_internal_temporal_bbox
 * @brief Return the last argument initialized with the bounding box of a
 * temporal sequence set
 * @param[in] ss Temporal sequence set
 * @param[out] box Bounding box
 */
void
tsequenceset_set_bbox(const TSequenceSet *ss, void *box)
{
  assert(ss); assert(box);
  memset(box, 0, ss->bboxsize);
  memcpy(box, TSEQUENCESET_BBOX_PTR(ss), ss->bboxsize);
  return;
}

/**
 * @ingroup meos_internal_temporal_accessor
 * @brief Return the last argument initialized with the temporal box of a
 * temporal number
 * @param[in] temp Temporal number
 * @param[out] box Temporal box
 */
void
tnumber_set_tbox(const Temporal *temp, TBox *box)
{
  assert(temp); assert(box); assert(tnumber_type(temp->temptype));
  assert(temptype_subtype(temp->subtype));
  switch (temp->subtype)
  {
    case TINSTANT:
      tnumberinst_set_tbox((TInstant *) temp, box);
      break;
    case TSEQUENCE:
      tnumberseq_set_tbox((TSequence *) temp, box);
      break;
    default: /* TSEQUENCESET */
      tnumberseqset_set_tbox((TSequenceSet *) temp, box);
  }
  return;
}

/*****************************************************************************/

/**
 * @brief Set a temporal box from an array of temporal number instants
 * @param[in] instants Temporal instants
 * @param[in] count Number of elements in the array
 * @param[in] lower_inc, upper_inc True when the corresponding bound is
 * inclusive, false otherwise
 * @param[in] interp Interpolation
 * @param[in] box Box
 */
static void
tnumberinstarr_set_tbox(const TInstant **instants, int count, bool lower_inc,
  bool upper_inc, interpType interp, TBox *box)
{
  assert(tnumber_type(instants[0]->temptype));
  meosType basetype = temptype_basetype(instants[0]->temptype);
  meosType spantype = basetype_spantype(basetype);
  /* For discrete or step interpolation the bounds are always inclusive */
  bool lower_inc1 = lower_inc;
  bool upper_inc1 = upper_inc;
  if (interp != LINEAR)
  {
    lower_inc1 = upper_inc1 = true;
  }
  /* Compute the value span */
  Datum min = tinstant_val(instants[0]);
  Datum max = min;
  bool min_inc = lower_inc1, max_inc = lower_inc1;
  for (int i = 1; i < count; i++)
  {
    Datum value = tinstant_val(instants[i]);
    int min_cmp = datum_cmp(value, min, basetype);
    int max_cmp = datum_cmp(value, max, basetype);
    if (min_cmp <= 0)
    {
      min = value;
      if (min_cmp == 0)
        min_inc |= (i < count - 1) ? true : upper_inc1;
      else
        min_inc = (i < count - 1) ? true : upper_inc1;
    }
    if (max_cmp >= 0)
    {
      max = value;
      if (max_cmp == 0)
        max_inc |= (i < count - 1) ? true : upper_inc1;
      else
        max_inc = (i < count - 1) ? true : upper_inc1;
    }
  }
  if (datum_eq(min, max, basetype))
  {
    min_inc = max_inc = true;
  }
  span_set(min, max, min_inc, max_inc, basetype, spantype, &box->span);
  /* Compute the time span */
  span_set(TimestampTzGetDatum(instants[0]->t),
    TimestampTzGetDatum(instants[count - 1]->t), lower_inc, upper_inc,
    T_TIMESTAMPTZ, T_TSTZSPAN, &box->period);
  /* Set the flags */
  MEOS_FLAGS_SET_X(box->flags, true);
  MEOS_FLAGS_SET_T(box->flags, true);
  return;
}

/**
 * @brief Set a bounding box from an array of temporal instant values
 * @param[in] instants Temporal instants
 * @param[in] count Number of elements in the array
 * @param[in] lower_inc,upper_inc Period bounds
 * @param[in] interp Interpolation
 * @param[out] box Bounding box
 */
void
tinstarr_compute_bbox(const TInstant **instants, int count, bool lower_inc,
  bool upper_inc, interpType interp, void *box)
{
  meosType temptype = instants[0]->temptype;
  assert(temporal_type(temptype));
  if (talpha_type(temptype))
    span_set(TimestampTzGetDatum(instants[0]->t),
      TimestampTzGetDatum(instants[count - 1]->t), lower_inc, upper_inc,
      T_TIMESTAMPTZ, T_TSTZSPAN, (Span *) box);
  else if (tnumber_type(temptype))
    tnumberinstarr_set_tbox(instants, count, lower_inc, upper_inc,
      interp, (TBox *) box);
  else if (tgeo_type(temptype))
    tpointinstarr_set_stbox(instants, count, (STBox *) box);
#if NPOINT
  else if (temptype == T_TNPOINT)
    tnpointinstarr_set_stbox(instants, count, interp, (STBox *) box);
#endif
  else
  {
    meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
      "Unknown temporal type for bounding box function: %d", temptype);
    return;
  }
  /* Set the lower_inc and upper_inc bounds of the period at the beginning
   * of the bounding box */
  Span *s = (Span *) box;
  s->lower_inc = lower_inc;
  s->upper_inc = upper_inc;
  return;
}

/**
 * @brief Expand the bounding box of a temporal number sequence with an instant
 * @param[inout] seq Temporal sequence
 * @param[in] inst Temporal instant
 */
static void
tnumberseq_expand_tbox(TSequence *seq, const TInstant *inst)
{
  TBox box;
  tinstant_set_bbox(inst, &box);
  tbox_expand(&box, (TBox *) TSEQUENCE_BBOX_PTR(seq));
  return;
}

/**
 * @ingroup meos_internal_temporal_bbox
 * @brief Expand the bounding box of a temporal sequence with an additional
 * instant
 * @param[inout] seq Temporal sequence
 * @param[in] inst Temporal instant
 */
void
tsequence_expand_bbox(TSequence *seq, const TInstant *inst)
{
  assert(temporal_type(seq->temptype));
  if (talpha_type(seq->temptype))
    span_set(TimestampTzGetDatum(TSEQUENCE_INST_N(seq, 0)->t),
      TimestampTzGetDatum(inst->t), seq->period.lower_inc, true, T_TIMESTAMPTZ,
      T_TSTZSPAN, (Span *) TSEQUENCE_BBOX_PTR(seq));
  else if (tnumber_type(seq->temptype))
    tnumberseq_expand_tbox(seq, inst);
  else if (tgeo_type(seq->temptype))
    tpointseq_expand_stbox(seq, inst);
#if NPOINT
  else if (seq->temptype == T_TNPOINT)
    tnpointseq_expand_stbox(seq, inst);
#endif
  else
    meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
      "Unknown temporal type for bounding box function: %d", seq->temptype);
  return;
}

/**
 * @ingroup meos_internal_temporal_bbox
 * @brief Expand the bounding box of a temporal sequence set with an additional
 * sequence
 * @param[inout] ss Temporal sequence set
 * @param[in] seq Temporal sequence
 */
void
tsequenceset_expand_bbox(TSequenceSet *ss, const TSequence *seq)
{
  assert(temporal_type(ss->temptype));
  if (talpha_type(ss->temptype))
    span_expand(&seq->period, &ss->period);
  else if (tnumber_type(ss->temptype))
    tbox_expand((TBox *) TSEQUENCE_BBOX_PTR(seq),
      (TBox *) TSEQUENCE_BBOX_PTR(ss));
  // TODO Generalize as for tgeogpointseq_expand_stbox
  else if (tspatial_type(ss->temptype))
    stbox_expand((STBox *) TSEQUENCE_BBOX_PTR(seq),
      (STBox *) TSEQUENCE_BBOX_PTR(ss));
  else
    meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
      "Unknown temporal type for bounding box function: %d", ss->temptype);
  return;
}

/**
 * @brief Return the last argument initialized with the timestamptz span of an
 * array of temporal sequences
 * @param[in] sequences Temporal instants
 * @param[in] count Number of elements in the array
 * @param[out] s Result
 */
static void
tseqarr_set_tstzspan(const TSequence **sequences, int count, Span *s)
{
  const Span *first = &sequences[0]->period;
  const Span *last = &sequences[count - 1]->period;
  span_set(first->lower, last->upper, first->lower_inc, last->upper_inc,
    T_TIMESTAMPTZ, T_TSTZSPAN, s);
  return;
}

/**
 * @brief Return the last argument initialized with the temporal box of an
 * array of temporal number sequences
 * @param[in] box Box
 * @param[in] sequences Temporal instants
 * @param[in] count Number of elements in the array
 */
static void
tnumberseqarr_set_tbox(const TSequence **sequences, int count, TBox *box)
{
  memcpy(box, TSEQUENCE_BBOX_PTR(sequences[0]), sizeof(TBox));
  for (int i = 1; i < count; i++)
  {
    const TBox *box1 = TSEQUENCE_BBOX_PTR(sequences[i]);
    tbox_expand(box1, box);
  }
  return;
}

/**
 * @brief Return the last argument initialized with the bounding box from an
 * array of temporal sequences
 */
void
tseqarr_compute_bbox(const TSequence **sequences, int count, void *box)
{
  assert(temporal_type(sequences[0]->temptype));
  if (talpha_type(sequences[0]->temptype))
    tseqarr_set_tstzspan(sequences, count, (Span *) box);
  else if (tnumber_type(sequences[0]->temptype))
    tnumberseqarr_set_tbox(sequences, count, (TBox *) box);
  else if (tspatial_type(sequences[0]->temptype))
    tpointseqarr_set_stbox(sequences, count, (STBox *) box);
  else
    meos_error(ERROR, MEOS_ERR_INTERNAL_TYPE_ERROR,
      "Unknown temporal type for bounding box function: %d",
      sequences[0]->temptype);
  return;
}

/*****************************************************************************/

#if MEOS
/**
 * @brief Recompute the bounding box of a temporal sequence
 * @param[inout] seq Temporal sequence
 * @note This function is applied upon a restart
 */
void
tsequence_compute_bbox(TSequence *seq)
{
  const TInstant **instants = palloc(sizeof(TInstant *) * seq->count);
  for (int i = 0; i < seq->count; i++)
    instants[i] = TSEQUENCE_INST_N(seq, i);
  interpType interp = MEOS_FLAGS_GET_INTERP(seq->flags);
  tinstarr_compute_bbox(instants, seq->count, seq->period.lower_inc,
    seq->period.upper_inc, interp, TSEQUENCESET_BBOX_PTR(seq));
  pfree(instants);
  return;
}

/**
 * @brief (Re)compute the bounding box of a temporal sequence set
 * @param[inout] ss Temporal sequence set
 * @note This function is applied upon a restart
 */
void
tsequenceset_compute_bbox(TSequenceSet *ss)
{
  const TSequence **sequences = palloc(sizeof(TSequence *) * ss->count);
  for (int i = 0; i < ss->count; i++)
    sequences[i] = TSEQUENCESET_SEQ_N(ss, i);
  tseqarr_compute_bbox(sequences, ss->count, TSEQUENCESET_BBOX_PTR(ss));
  pfree(sequences);
  return;
}
#endif /* MEOS */

/*****************************************************************************
 * Boxes functions
 * These functions can be used for defining Multi-Entry Search Trees (a.k.a.
 * VODKA) indexes
 * https://www.pgcon.org/2014/schedule/events/696.en.html
 * https://github.com/MobilityDB/mest
 *****************************************************************************/

/**
 * @ingroup meos_internal_temporal_bbox
 * @brief Return an array of maximum n temporal boxes from a temporal number
 * instant
 * @param[in] inst Temporal value
 * @param[out] count Number of elements in the output array
 */
TBox *
tnumberinst_tboxes(const TInstant *inst, int *count)
{
  assert(inst); assert(tnumber_type(inst->temptype));
  TBox *result = palloc(sizeof(TBox));
  tnumberinst_set_tbox(inst, &result[0]);
  *count = 1;
  return result;
}

/**
 * @brief Return an array of maximum n temporal boxes from the instants of a
 * temporal number sequence with discrete interpolation (iterator function)
 * @param[in] seq Temporal value
 * @param[in] max_count Maximum number of elements in the output array
 * If the value is < 1, the result is one box per segment
 * @param[out] result Temporal box
 * @return Number of elements in the array
 */
static int
tnumberseq_disc_tboxes_iter(const TSequence *seq, int max_count, TBox *result)
{
  assert(! MEOS_FLAGS_LINEAR_INTERP(seq->flags)); assert(seq->count > 1);
  /* Temporal sequence has at least 2 instants */
  int nsegs = seq->count - 1;
  if (max_count < 1 || nsegs <= max_count)
  {
    /* One bounding box per instant */
    for (int i = 0; i < seq->count; i++)
      tnumberinst_set_tbox(TSEQUENCE_INST_N(seq, i), &result[i]);
    return nsegs;
  }
  else
  {
    /* One bounding box per several consecutive instants */
    /* Minimum number of input instants merged together in an output box */
    int size = nsegs / max_count;
    /* Number of output boxes that result from merging (size + 1) instants */
    int remainder = nsegs % max_count;
    int i = 0; /* Loop variable for input instants */
    int k = 0; /* Loop variable for output boxes */
    while (k < max_count)
    {
      int j = i + size;
      if (k < remainder)
        j++;
      assert(i < j);
      tnumberinst_set_tbox(TSEQUENCE_INST_N(seq, i), &result[k]);
      for (int l = i + 1; l <= j; l++)
      {
        const TInstant *inst = TSEQUENCE_INST_N(seq, l);
        TBox box;
        tnumberinst_set_tbox(inst, &box);
        tbox_expand(&box, &result[k]);
      }
      k++;
      i = j;
    }
    return max_count;
  }
}

/**
 * @brief Return an array of maximum n temporal boxes from the segments of a
 * temporal number sequence with continuous interpolation (iterator function)
 * @param[in] seq Temporal value
 * @param[in] max_count Maximum number of elements in the output array
 * If the value is < 1, the result is one box per segment
 * @param[out] result Temporal box
 * @return Number of elements in the array
 */
static int
tnumberseq_cont_tboxes_iter(const TSequence *seq, int max_count, TBox *result)
{
  assert(MEOS_FLAGS_GET_INTERP(seq->flags) != DISCRETE); assert(seq->count > 1);
  /* Temporal sequence has at least 2 instants */
  int nsegs = seq->count - 1;
  if (max_count < 1 || nsegs <= max_count)
  {
    /* One bounding box per segment */
    const TInstant *inst1 = TSEQUENCE_INST_N(seq, 0);
    for (int i = 0; i < seq->count - 1; i++)
    {
      tnumberinst_set_tbox(inst1, &result[i]);
      const TInstant *inst2 = TSEQUENCE_INST_N(seq, i + 1);
      TBox box;
      tnumberinst_set_tbox(inst2, &box);
      tbox_expand(&box, &result[i]);
      inst1 = inst2;
    }
    return nsegs;
  }
  else
  {
    /* One bounding box per several consecutive segments */
    /* Minimum number of input segments merged together in an output box */
    int size = nsegs / max_count;
    /* Number of output boxes that result from merging (size + 1) segments */
    int remainder = nsegs % max_count;
    int i = 0; /* Loop variable for input segments */
    int k = 0; /* Loop variable for output boxes */
    while (k < max_count)
    {
      int j = i + size;
      if (k < remainder)
        j++;
      assert(i < j);
      tnumberinst_set_tbox(TSEQUENCE_INST_N(seq, i), &result[k]);
      for (int l = i + 1; l <= j; l++)
      {
        const TInstant *inst = TSEQUENCE_INST_N(seq, l);
        TBox box;
        tnumberinst_set_tbox(inst, &box);
        tbox_expand(&box, &result[k]);
      }
      k++;
      i = j;
    }
    return max_count;
  }
}

/**
 * @brief Return an array of maximum n temporal boxes from the instants or
 * segments of a temporal number sequence (iterator function)
 * @param[in] seq Temporal value
 * @param[in] max_count Maximum number of elements in the output array
 * If the value is < 1, the result is one box per segment
 * @param[out] result Temporal box
 * @return Number of elements in the array
 */
static int
tnumberseq_tboxes_iter(const TSequence *seq, int max_count, TBox *result)
{
  /* Instantaneous sequence */
  if (seq->count == 1)
  {
    tnumberinst_set_tbox(TSEQUENCE_INST_N(seq, 0), &result[0]);
    return 1;
  }
  return (MEOS_FLAGS_GET_INTERP(seq->flags) == DISCRETE) ?
    tnumberseq_disc_tboxes_iter(seq, max_count, result) :
    tnumberseq_cont_tboxes_iter(seq, max_count, result);
}

/**
 * @ingroup meos_internal_temporal_bbox
 * @brief Return an array of maximum n temporal boxes from the segments
 * of a temporal number sequence
 * @param[in] seq Temporal sequence
 * @param[in] max_count Maximum number of elements in the output array
 * If the value is < 1, the result is one box per segment
 * @param[out] count Number of elements in the output array
 */
TBox *
tnumberseq_tboxes(const TSequence *seq, int max_count, int *count)
{
  assert(seq); assert(count); assert(tnumber_type(seq->temptype));
  int nboxes = (max_count < 1) ?
    ( seq->count == 1 ? 1 : seq->count - 1 ) : max_count;
  TBox *result = palloc(sizeof(TBox) * nboxes);
  *count = tnumberseq_tboxes_iter(seq, max_count, result);
  return result;
}

/**
 * @ingroup meos_internal_temporal_bbox
 * @brief Return an array of temporal boxes from the segments of a
 * temporal number sequence set
 * @param[in] ss Temporal sequence set
 * @param[in] max_count Maximum number of elements in the output array
 * If the value is < 1, the result is one box per segment
 * @param[out] count Number of elements in the output array
 */
TBox *
tnumberseqset_tboxes(const TSequenceSet *ss, int max_count, int *count)
{
  assert(ss); assert(count); assert(tnumber_type(ss->temptype));
  assert(MEOS_FLAGS_LINEAR_INTERP(ss->flags));
  int nboxes = (max_count < 1) ? ss->totalcount : max_count;
  TBox *result = palloc(sizeof(TBox) * nboxes);
  int nboxes1;
  if (max_count < 1 || ss->totalcount <= max_count)
  {
    /* One bounding box per segment */
    nboxes1 = 0;
    for (int i = 0; i < ss->count; i++)
      nboxes1 += tnumberseq_tboxes_iter(TSEQUENCESET_SEQ_N(ss, i),
        max_count, &result[nboxes1]);
    *count = nboxes1;
    return result;
  }
  else if (ss->count <= max_count)
  {
    /* Amount of bounding boxes per composing sequence determined from the
     * proportion of seq->count and ss->totalcount */
    nboxes1 = 0;
    for (int i = 0; i < ss->count; i++)
    {
      const TSequence *seq = TSEQUENCESET_SEQ_N(ss, i);
      int nboxes_seq = (int) (max_count * seq->count * 1.0 / ss->totalcount);
      if (! nboxes_seq)
        nboxes_seq = 1;
      nboxes1 += tnumberseq_tboxes_iter(seq, nboxes_seq,
        &result[nboxes1]);
    }
    *count = nboxes1;
    return result;
  }
  else
  {
    /* Merge consecutive sequences to reach the maximum number of boxes */
    /* Minimum number of sequences merged together in an output box */
    int size = ss->count / max_count;
    /* Number of output boxes that result from merging (size + 1) sequences */
    int remainder = ss->count % max_count;
    int i = 0; /* Loop variable for input sequences */
    int k = 0; /* Loop variable for output boxes */
    while (k < max_count)
    {
      int j = i + size - 1;
      if (k < remainder)
        j++;
      if (i < j)
      {
        tnumberseq_tboxes_iter(TSEQUENCESET_SEQ_N(ss, i), 1,
          &result[k]);
        for (int l = i + 1; l <= j; l++)
        {
          TBox box;
          tnumberseq_tboxes_iter(TSEQUENCESET_SEQ_N(ss, l), 1, &box);
          tbox_expand(&box, &result[k]);
        }
        i = j + 1;
        k++;
      }
      else
        tnumberseq_tboxes_iter(TSEQUENCESET_SEQ_N(ss, i++), 1,
          &result[k++]);
    }
    *count = max_count;
    return result;
  }
}

/**
 * @ingroup meos_temporal_bbox
 * @brief Return an array of temporal boxes from the segments of a
 * temporal number
 * @param[in] temp Temporal value
 * @param[in] max_count Maximum number of elements in the output array.
 * If the value is < 1, the result is one box per segment
 * @param[out] count Number of values of the output array
 * @return On error return @p NULL
 * @csqlfn #Tnumber_tboxes()
 */
TBox *
tnumber_tboxes(const Temporal *temp, int max_count, int *count)
{
  /* Ensure validity of the arguments */
  if (! ensure_not_null((void *) temp) || ! ensure_not_null((void *) count) ||
      ! ensure_tnumber_type(temp->temptype))
    return NULL;

  assert(temptype_subtype(temp->subtype));
  if (temp->subtype == TINSTANT)
    return tnumberinst_tboxes((TInstant *) temp, count);
  else if (temp->subtype == TSEQUENCE)
    return tnumberseq_tboxes((TSequence *) temp, max_count, count);
  else /* TSEQUENCESET */
    return tnumberseqset_tboxes((TSequenceSet *) temp, max_count, count);
}

/*****************************************************************************
 * Generic bounding box functions for temporal types
 * The inclusive/exclusive bounds are taken into account for the comparisons
 *****************************************************************************/

/**
 * @brief Generic bounding box function for a temporal value and a timestamptz
 * span
 */
bool
boxop_temporal_tstzspan(const Temporal *temp, const Span *s,
  bool (*func)(const Span *, const Span *), bool invert)
{
  Span s1;
  temporal_set_tstzspan(temp, &s1);
  return invert ? func(s, &s1) : func(&s1, s);

}

/**
 * @brief Generic bounding box function for two temporal values
 */
bool
boxop_temporal_temporal(const Temporal *temp1, const Temporal *temp2,
  bool (*func)(const Span *, const Span *))
{
  Span s1, s2;
  temporal_set_tstzspan(temp1, &s1);
  temporal_set_tstzspan(temp2, &s2);
  return func(&s1, &s2);
}

/*****************************************************************************
 * Generic bounding box functions for temporal number types
 *****************************************************************************/

/**
 * @brief Generic bounding box function for a temporal number and a span
 */
bool
boxop_tnumber_numspan(const Temporal *temp, const Span *s,
  bool (*func)(const Span *, const Span *), bool invert)
{
  Span s1;
  tnumber_set_span(temp, &s1);
  return invert ? func(s, &s1) : func(&s1, s);
}

/**
 * @brief Generic bounding box function for a temporal number and a temporal
 * box
 */
bool
boxop_tnumber_tbox(const Temporal *temp, const TBox *box,
  bool (*func)(const TBox *, const TBox *), bool invert)
{
  TBox box1;
  temporal_set_bbox(temp, &box1);
  return invert ? func(box, &box1) : func(&box1, box);
}

/**
 * @brief Generic bounding box function for two temporal numbers
 */
bool
boxop_tnumber_tnumber(const Temporal *temp1, const Temporal *temp2,
  bool (*func)(const TBox *, const TBox *))
{
  TBox box1, box2;
  temporal_set_bbox(temp1, &box1);
  temporal_set_bbox(temp2, &box2);
  return func(&box1, &box2);
}

/*****************************************************************************/
