/* ------------------------------------------------------------------------
 *
 * rangeset.c
 *		IndexRange functions
 *
 * Copyright (c) 2015-2016, Postgres Professional
 *
 * ------------------------------------------------------------------------
 */

#include "rangeset.h"


/* Check if two ranges intersect */
bool
iranges_intersect(IndexRange a, IndexRange b)
{
	return (irange_lower(a) <= irange_upper(b)) &&
		   (irange_lower(b) <= irange_upper(a));
}

/* Check if two ranges adjoin */
bool
iranges_adjoin(IndexRange a, IndexRange b)
{
	return (irange_upper(a) == irb_pred(irange_lower(b))) ||
		   (irange_upper(b) == irb_pred(irange_lower(a)));
}

/* Check if two ranges cover the same area */
bool
irange_eq_bounds(IndexRange a, IndexRange b)
{
	return (irange_lower(a) == irange_lower(b)) &&
		   (irange_upper(a) == irange_upper(b));
}

/* Comapre lossiness factor of two ranges */
ir_cmp_lossiness
irange_cmp_lossiness(IndexRange a, IndexRange b)
{
	if (is_irange_lossy(a) == is_irange_lossy(b))
		return IR_EQ_LOSSINESS;

	if (is_irange_lossy(a))
		return IR_A_LOSSY;

	if (is_irange_lossy(b))
		return IR_B_LOSSY;

	return IR_EQ_LOSSINESS;
}


/* Make union of two conjuncted ranges */
IndexRange
irange_union_simple(IndexRange a, IndexRange b)
{
	/* Ranges should be connected somehow */
	Assert(iranges_intersect(a, b) || iranges_adjoin(a, b));

	return make_irange(Min(irange_lower(a), irange_lower(b)),
					   Max(irange_upper(a), irange_upper(b)),
					   is_irange_lossy(a) && is_irange_lossy(b));
}

/* Get intersection of two conjuncted ranges */
IndexRange
irange_intersection_simple(IndexRange a, IndexRange b)
{
	/* Ranges should be connected somehow */
	Assert(iranges_intersect(a, b) || iranges_adjoin(a, b));

	return make_irange(Max(irange_lower(a), irange_lower(b)),
					   Min(irange_upper(a), irange_upper(b)),
					   is_irange_lossy(a) || is_irange_lossy(b));
}


/* Split covering IndexRange into several IndexRanges if needed */
static IndexRange
irange_handle_cover_internal(IndexRange ir_covering,
							 IndexRange ir_inner,
							 List **new_iranges)
{
	/* Equal lossiness should've been taken into cosideration earlier */
	Assert(is_irange_lossy(ir_covering) != is_irange_lossy(ir_inner));

	/* range 'ir_inner' is lossy */
	if (is_irange_lossy(ir_covering) == false)
		return ir_covering;

	/* range 'ir_covering' is lossy, 'ir_inner' is lossless! */
	else
	{
		IndexRange	ret; /* IndexRange to be returned */

		/* 'left_range_upper' should not be less than 'left_range_lower' */
		uint32		left_range_lower	= irange_lower(ir_covering),
					left_range_upper	= Max(irb_pred(irange_lower(ir_inner)),
											  left_range_lower);

		/* 'right_range_lower' should not be greater than 'right_range_upper' */
		uint32		right_range_upper	= irange_upper(ir_covering),
					right_range_lower	= Min(irb_succ(irange_upper(ir_inner)),
											  right_range_upper);

		/* We have to split the covering lossy IndexRange */
		Assert(is_irange_lossy(ir_covering) == true);

		/* 'ir_inner' should not cover leftmost IndexRange */
		if (irange_lower(ir_inner) > left_range_upper)
		{
			IndexRange	left_range;

			/* Leftmost IndexRange is lossy */
			left_range = make_irange(left_range_lower,
									 left_range_upper,
									 true);

			/* Append leftmost IndexRange ('left_range') to 'new_iranges' */
			*new_iranges = lappend_irange(*new_iranges, left_range);
		}

		/* 'ir_inner' should not cover rightmost IndexRange */
		if (right_range_lower > irange_upper(ir_inner))
		{
			IndexRange	right_range;

			/* Rightmost IndexRange is also lossy */
			right_range = make_irange(right_range_lower,
									  right_range_upper,
									  true);

			/* 'right_range' is indeed rightmost IndexRange */
			ret = right_range;

			/* Append medial IndexRange ('ir_inner') to 'new_iranges' */
			*new_iranges = lappend_irange(*new_iranges, ir_inner);
		}
		/* Else return 'ir_inner' as rightmost IndexRange */
		else ret = ir_inner;

		/* Return rightmost IndexRange (right_range | ir_inner) */
		return ret;
	}
}

/* Calculate union of two IndexRanges, return rightmost IndexRange */
static IndexRange
irange_union_internal(IndexRange first,
					  IndexRange second,
					  List **new_iranges)
{
	/* Swap 'first' and 'second' if order is incorrect */
	if (irange_lower(first) > irange_lower(second))
	{
		IndexRange temp;

		temp = first;
		first = second;
		second = temp;
	}

	/* IndexRanges intersect */
	if (iranges_intersect(first, second))
	{
		/* Calculate the intersection of 'first' and 'second' */
		IndexRange ir_union = irange_union_simple(first, second);

		/* if lossiness is the same, unite them and skip */
		if (is_irange_lossy(first) == is_irange_lossy(second))
			return ir_union;

		/* range 'first' covers 'second' */
		if (irange_eq_bounds(ir_union, first))
		{
			/* Save rightmost IndexRange to 'ret' */
			return irange_handle_cover_internal(first, second, new_iranges);
		}
		/* range 'second' covers 'first' */
		else if (irange_eq_bounds(ir_union, second))
		{
			/* Save rightmost IndexRange to 'ret' */
			return irange_handle_cover_internal(second, first, new_iranges);
		}
		/* No obvious leader, lossiness differs */
		else
		{
			/* range 'second' is lossy */
			if (is_irange_lossy(first) == false)
			{
				IndexRange	ret;

				/* Set new current IndexRange */
				ret = make_irange(irb_succ(irange_upper(first)),
								  irange_upper(second),
								  is_irange_lossy(second));

				/* Append lower part to 'new_iranges' */
				*new_iranges = lappend_irange(*new_iranges, first);

				/* Return a part of 'second' */
				return ret;
			}
			/* range 'first' is lossy */
			else
			{
				IndexRange	new_irange;

				new_irange = make_irange(irange_lower(first),
										 irb_pred(irange_lower(second)),
										 is_irange_lossy(first));

				/* Append lower part to 'new_iranges' */
				*new_iranges = lappend_irange(*new_iranges, new_irange);

				/* Return 'second' */
				return second;
			}
		}
	}
	/* IndexRanges do not intersect */
	else
	{
		/* Try to unite these IndexRanges if it's possible */
		if (irange_cmp_lossiness(first, second) == IR_EQ_LOSSINESS &&
			iranges_adjoin(first, second))
		{
			/* Return united IndexRange */
			return irange_union_simple(first, second);
		}
		/* IndexRanges are not adjoint */
		else
		{
			/* add 'first' to 'new_iranges' */
			*new_iranges = lappend_irange(*new_iranges, first);

			/* Return 'second' */
			return second;
		}
	}
}

/*
 * Make union of two index rage lists.
 */
List *
irange_list_union(List *a, List *b)
{
	ListCell   *ca,							/* iterator of A */
			   *cb;							/* iterator of B */
	List	   *result = NIL;				/* list of IndexRanges */
	IndexRange	cur = InvalidIndexRange;	/* current irange */

	/* Initialize iterators */
	ca = list_head(a);
	cb = list_head(b);

	/* Loop until we have no cells */
	while (ca || cb)
	{
		IndexRange next = InvalidIndexRange;

		/* Fetch next irange with lesser lower bound */
		if (ca && cb)
		{
			if (irange_lower(lfirst_irange(ca)) <= irange_lower(lfirst_irange(cb)))
			{
				next = lfirst_irange(ca);
				ca = lnext(ca); /* move to next cell */
			}
			else
			{
				next = lfirst_irange(cb);
				cb = lnext(cb); /* move to next cell */
			}
		}
		/* Fetch next irange from A */
		else if (ca)
		{
			next = lfirst_irange(ca);
			ca = lnext(ca); /* move to next cell */
		}
		/* Fetch next irange from B */
		else if (cb)
		{
			next = lfirst_irange(cb);
			cb = lnext(cb); /* move to next cell */
		}

		/* Put this irange to 'cur' if don't have it yet */
		if (!is_irange_valid(cur))
		{
			cur = next;
			continue; /* skip this iteration */
		}

		/* Unite 'cur' and 'next' in an appropriate way */
		cur = irange_union_internal(cur, next, &result);
	}

	/* Put current value into result list if any */
	if (is_irange_valid(cur))
		result = lappend_irange(result, cur);

	return result;
}

/*
 * Find intersection of two range lists.
 */
List *
irange_list_intersection(List *a, List *b)
{
	ListCell   *ca,				/* iterator of A */
			   *cb;				/* iterator of B */
	List	   *result = NIL;	/* list of IndexRanges */

	/* Initialize iterators */
	ca = list_head(a);
	cb = list_head(b);

	/* Loop until we have no cells */
	while (ca && cb)
	{
		IndexRange	ra = lfirst_irange(ca),
					rb = lfirst_irange(cb);

		/* Only care about intersecting ranges */
		if (iranges_intersect(ra, rb))
		{
			IndexRange	intersect, last;

			/*
			 * Get intersection and try to "glue" it to
			 * previous range, put it separately otherwise.
			 */
			intersect = irange_intersection_simple(ra, rb);
			if (result != NIL)
			{
				last = llast_irange(result);
				if (iranges_adjoin(last, intersect) &&
					is_irange_lossy(last) == is_irange_lossy(intersect))
				{
					llast(result) = alloc_irange(irange_union_simple(last, intersect));
				}
				else
				{
					result = lappend_irange(result, intersect);
				}
			}
			else
			{
				result = lappend_irange(result, intersect);
			}
		}

		/*
		 * Fetch next ranges. We use upper bound of current range to determine
		 * which lists to fetch, since lower bound of next range is greater (or
		 * equal) to upper bound of current.
		 */
		if (irange_upper(ra) <= irange_upper(rb))
			ca = lnext(ca);
		if (irange_upper(ra) >= irange_upper(rb))
			cb = lnext(cb);
	}
	return result;
}

/* Get total number of elements in range list */
int
irange_list_length(List *rangeset)
{
	ListCell   *lc;
	uint32		result = 0;

	foreach (lc, rangeset)
	{
		IndexRange	irange = lfirst_irange(lc);
		uint32		diff = irange_upper(irange) - irange_lower(irange);

		Assert(irange_upper(irange) >= irange_lower(irange));

		result += diff + 1;
	}

	return (int) result;
}

/* Find particular index in range list */
bool
irange_list_find(List *rangeset, int index, bool *lossy)
{
	ListCell   *lc;

	foreach (lc, rangeset)
	{
		IndexRange irange = lfirst_irange(lc);
		if (index >= irange_lower(irange) && index <= irange_upper(irange))
		{
			if (lossy)
				*lossy = is_irange_lossy(irange);
			return true;
		}
	}
	return false;
}
