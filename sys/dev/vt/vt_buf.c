/*-
 * Copyright (c) 2009, 2013 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Ed Schouten under sponsorship from the
 * FreeBSD Foundation.
 *
 * Portions of this software were developed by Oleksandr Rybalko
 * under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/vt/vt.h>

static MALLOC_DEFINE(M_VTBUF, "vtbuf", "vt buffer");

#define	VTBUF_LOCK(vb)		mtx_lock_spin(&(vb)->vb_lock)
#define	VTBUF_UNLOCK(vb)	mtx_unlock_spin(&(vb)->vb_lock)

#define POS_INDEX(vb, c, r) ((r) * (vb)->vb_scr_size.tp_col + (c))

/*
 * line4
 * line5 <--- curroffset (terminal output to that line)
 * line0
 * line1                  <--- roffset (history display from that point)
 * line2
 * line3
 */
int
vthistory_seek(struct vt_buf *vb, int offset, int whence)
{
	int diff, top, bottom, roffset;

	/* No scrolling if not enabled. */
	if ((vb->vb_flags & VBF_SCROLL) == 0) {
		if (vb->vb_roffset != vb->vb_curroffset) {
			vb->vb_roffset = vb->vb_curroffset;
			return (0xffff);
		}
		return (0); /* No changes */
	}
	top = (vb->vb_flags & VBF_HISTORY_FULL)?
	    (vb->vb_curroffset + vb->vb_scr_size.tp_row):vb->vb_history_size;
	bottom = vb->vb_curroffset + vb->vb_history_size;

	/*
	 * Operate on copy of offset value, since it temporary can be bigger
	 * than amount of rows in buffer.
	 */
	roffset = vb->vb_roffset + vb->vb_history_size;
	switch (whence) {
	case VHS_SET:
		roffset = offset + vb->vb_history_size;
		break;
	case VHS_CUR:
		roffset += offset;
		break;
	case VHS_END:
		/* Go to current offset. */
		roffset = vb->vb_curroffset + vb->vb_history_size;
		break;
	}

	roffset = (roffset < top)?top:roffset;
	roffset = (roffset > bottom)?bottom:roffset;

	roffset %= vb->vb_history_size;

	if (vb->vb_roffset != roffset) {
		diff = vb->vb_roffset - roffset;
		vb->vb_roffset = roffset;
		/*
		 * Offset changed, please update Nth lines on sceen.
		 * +N - Nth lines at top;
		 * -N - Nth lines at bottom.
		 */
		return (diff);
	}
	return (0); /* No changes */
}

void
vthistory_addlines(struct vt_buf *vb, int offset)
{

	vb->vb_curroffset += offset;
	if (vb->vb_curroffset < 0)
		vb->vb_curroffset = 0;
	vb->vb_curroffset %= vb->vb_history_size;
	if ((vb->vb_flags & VBF_SCROLL) == 0) {
		vb->vb_roffset = vb->vb_curroffset;
	}
}

void
vthistory_getpos(const struct vt_buf *vb, unsigned int *offset)
{

	*offset = vb->vb_roffset;
}

int
vtbuf_iscursor(struct vt_buf *vb, int row, int col)
{
	if ((vb->vb_flags & VBF_CURSOR) && (vb->vb_cursor.tp_row == row) &&
	    (vb->vb_cursor.tp_col == col))
		return (1);

	if ((POS_INDEX(vb, vb->vb_mark_start.tp_col, vb->vb_mark_start.tp_row) <
	    POS_INDEX(vb, col, row)) && (POS_INDEX(vb, col, row) <=
	    POS_INDEX(vb, vb->vb_mark_start.tp_col, vb->vb_mark_start.tp_row)))
		return (1);

	return (0);
}

static inline uint64_t
vtbuf_dirty_axis(unsigned int begin, unsigned int end)
{
	uint64_t left, right, mask;

	/*
	 * Mark all bits between begin % 64 and end % 64 dirty.
	 * This code is functionally equivalent to:
	 *
	 * 	for (i = begin; i < end; i++)
	 * 		mask |= (uint64_t)1 << (i % 64);
	 */

	/* Obvious case. Mark everything dirty. */
	if (end - begin >= 64)
		return (VBM_DIRTY);

	/* 1....0; used bits on the left. */
	left = VBM_DIRTY << begin % 64;
	/* 0....1; used bits on the right. */
	right = VBM_DIRTY >> -end % 64;

	/*
	 * Only take the intersection.  If the result of that is 0, it
	 * means that the selection crossed a 64 bit boundary along the
	 * way, which means we have to take the complement.
	 */
	mask = left & right;
	if (mask == 0)
		mask = left | right;
	return (mask);
}

static inline void
vtbuf_dirty(struct vt_buf *vb, const term_rect_t *area)
{

	VTBUF_LOCK(vb);
	if (vb->vb_dirtyrect.tr_begin.tp_row > area->tr_begin.tp_row)
		vb->vb_dirtyrect.tr_begin.tp_row = area->tr_begin.tp_row;
	if (vb->vb_dirtyrect.tr_begin.tp_col > area->tr_begin.tp_col)
		vb->vb_dirtyrect.tr_begin.tp_col = area->tr_begin.tp_col;
	if (vb->vb_dirtyrect.tr_end.tp_row < area->tr_end.tp_row)
		vb->vb_dirtyrect.tr_end.tp_row = area->tr_end.tp_row;
	if (vb->vb_dirtyrect.tr_end.tp_col < area->tr_end.tp_col)
		vb->vb_dirtyrect.tr_end.tp_col = area->tr_end.tp_col;
	vb->vb_dirtymask.vbm_row |=
	    vtbuf_dirty_axis(area->tr_begin.tp_row, area->tr_end.tp_row);
	vb->vb_dirtymask.vbm_col |=
	    vtbuf_dirty_axis(area->tr_begin.tp_col, area->tr_end.tp_col);
	VTBUF_UNLOCK(vb);
}

static inline void
vtbuf_dirty_cell(struct vt_buf *vb, const term_pos_t *p)
{
	term_rect_t area;

	area.tr_begin = *p;
	area.tr_end.tp_row = p->tp_row + 1;
	area.tr_end.tp_col = p->tp_col + 1;
	vtbuf_dirty(vb, &area);
}

static void
vtbuf_make_undirty(struct vt_buf *vb)
{

	vb->vb_dirtyrect.tr_begin = vb->vb_scr_size;
	vb->vb_dirtyrect.tr_end.tp_row = vb->vb_dirtyrect.tr_end.tp_col = 0;
	vb->vb_dirtymask.vbm_row = vb->vb_dirtymask.vbm_col = 0;
}

void
vtbuf_undirty(struct vt_buf *vb, term_rect_t *r, struct vt_bufmask *m)
{

	VTBUF_LOCK(vb);
	*r = vb->vb_dirtyrect;
	*m = vb->vb_dirtymask;
	vtbuf_make_undirty(vb);
	VTBUF_UNLOCK(vb);
}

void
vtbuf_copy(struct vt_buf *vb, const term_rect_t *r, const term_pos_t *p2)
{
	const term_pos_t *p1 = &r->tr_begin;
	term_rect_t area;
	unsigned int rows, cols;
	int pr, rdiff;

	KASSERT(r->tr_begin.tp_row < vb->vb_scr_size.tp_row,
	    ("vtbuf_copy begin.tp_row %d must be less than screen width %d",
		r->tr_begin.tp_row, vb->vb_scr_size.tp_row));
	KASSERT(r->tr_begin.tp_col < vb->vb_scr_size.tp_col,
	    ("vtbuf_copy begin.tp_col %d must be less than screen height %d",
		r->tr_begin.tp_col, vb->vb_scr_size.tp_col));

	KASSERT(r->tr_end.tp_row <= vb->vb_scr_size.tp_row,
	    ("vtbuf_copy end.tp_row %d must be less than screen width %d",
		r->tr_end.tp_row, vb->vb_scr_size.tp_row));
	KASSERT(r->tr_end.tp_col <= vb->vb_scr_size.tp_col,
	    ("vtbuf_copy end.tp_col %d must be less than screen height %d",
		r->tr_end.tp_col, vb->vb_scr_size.tp_col));

	KASSERT(p2->tp_row < vb->vb_scr_size.tp_row,
	    ("vtbuf_copy tp_row %d must be less than screen width %d",
		p2->tp_row, vb->vb_scr_size.tp_row));
	KASSERT(p2->tp_col < vb->vb_scr_size.tp_col,
	    ("vtbuf_copy tp_col %d must be less than screen height %d",
		p2->tp_col, vb->vb_scr_size.tp_col));

	rows = r->tr_end.tp_row - r->tr_begin.tp_row;
	rdiff = r->tr_begin.tp_row - p2->tp_row;
	cols = r->tr_end.tp_col - r->tr_begin.tp_col;
	if (r->tr_begin.tp_row > p2->tp_row && r->tr_begin.tp_col == 0 &&
	    r->tr_end.tp_col == vb->vb_scr_size.tp_col && /* Full row. */
	    (rows + rdiff) == vb->vb_scr_size.tp_row && /* Whole screen. */
	    rdiff > 0) { /* Only forward dirrection. Do not eat history. */
		vthistory_addlines(vb, rdiff);
	} else if (p2->tp_row < p1->tp_row) {
		/* Handle overlapping copies of line segments. */
		/* Move data up. */
		for (pr = 0; pr < rows; pr++)
			memmove(
			    &VTBUF_FIELD(vb, p2->tp_row + pr, p2->tp_col),
			    &VTBUF_FIELD(vb, p1->tp_row + pr, p1->tp_col),
			    cols * sizeof(term_char_t));
	} else {
		/* Move data down. */
		for (pr = rows - 1; pr >= 0; pr--)
			memmove(
			    &VTBUF_FIELD(vb, p2->tp_row + pr, p2->tp_col),
			    &VTBUF_FIELD(vb, p1->tp_row + pr, p1->tp_col),
			    cols * sizeof(term_char_t));
	}

	area.tr_begin = *p2;
	area.tr_end.tp_row = MIN(p2->tp_row + rows, vb->vb_scr_size.tp_row);
	area.tr_end.tp_col = MIN(p2->tp_col + cols, vb->vb_scr_size.tp_col);
	vtbuf_dirty(vb, &area);
}

static void
vtbuf_fill(struct vt_buf *vb, const term_rect_t *r, term_char_t c)
{
	unsigned int pr, pc;
	term_char_t *row;

	for (pr = r->tr_begin.tp_row; pr < r->tr_end.tp_row; pr++) {
		row = vb->vb_rows[(vb->vb_curroffset + pr) %
		    VTBUF_MAX_HEIGHT(vb)];
		for (pc = r->tr_begin.tp_col; pc < r->tr_end.tp_col; pc++) {
			row[pc] = c;
		}
	}
}

void
vtbuf_fill_locked(struct vt_buf *vb, const term_rect_t *r, term_char_t c)
{
	KASSERT(r->tr_begin.tp_row < vb->vb_scr_size.tp_row,
	    ("vtbuf_fill_locked begin.tp_row %d must be < screen width %d",
		r->tr_begin.tp_row, vb->vb_scr_size.tp_row));
	KASSERT(r->tr_begin.tp_col < vb->vb_scr_size.tp_col,
	    ("vtbuf_fill_locked begin.tp_col %d must be < screen height %d",
		r->tr_begin.tp_col, vb->vb_scr_size.tp_col));

	KASSERT(r->tr_end.tp_row <= vb->vb_scr_size.tp_row,
	    ("vtbuf_fill_locked end.tp_row %d must be <= screen width %d",
		r->tr_end.tp_row, vb->vb_scr_size.tp_row));
	KASSERT(r->tr_end.tp_col <= vb->vb_scr_size.tp_col,
	    ("vtbuf_fill_locked end.tp_col %d must be <= screen height %d",
		r->tr_end.tp_col, vb->vb_scr_size.tp_col));

	VTBUF_LOCK(vb);
	vtbuf_fill(vb, r, c);
	VTBUF_UNLOCK(vb);

	vtbuf_dirty(vb, r);
}

static void
vtbuf_init_rows(struct vt_buf *vb)
{
	int r;

	vb->vb_history_size = MAX(vb->vb_history_size, vb->vb_scr_size.tp_row);

	for (r = 0; r < vb->vb_history_size; r++)
		vb->vb_rows[r] = &vb->vb_buffer[r *
		    vb->vb_scr_size.tp_col];
}

void
vtbuf_init_early(struct vt_buf *vb)
{

	vb->vb_flags |= VBF_CURSOR;
	vb->vb_roffset = 0;
	vb->vb_curroffset = 0;

	vtbuf_init_rows(vb);
	vtbuf_make_undirty(vb);
	if ((vb->vb_flags & VBF_MTX_INIT) == 0) {
		mtx_init(&vb->vb_lock, "vtbuf", NULL, MTX_SPIN);
		vb->vb_flags |= VBF_MTX_INIT;
	}
}

void
vtbuf_init(struct vt_buf *vb, const term_pos_t *p)
{
	int sz;

	vb->vb_scr_size = *p;
	vb->vb_history_size = VBF_DEFAULT_HISTORY_SIZE;

	if ((vb->vb_flags & VBF_STATIC) == 0) {
		sz = vb->vb_history_size * p->tp_col * sizeof(term_char_t);
		vb->vb_buffer = malloc(sz, M_VTBUF, M_WAITOK | M_ZERO);

		sz = vb->vb_history_size * sizeof(term_char_t *);
		vb->vb_rows = malloc(sz, M_VTBUF, M_WAITOK | M_ZERO);
	}

	vtbuf_init_early(vb);
}

void
vtbuf_sethistory_size(struct vt_buf *vb, int size)
{
	term_pos_t p;

	/* With same size */
	p.tp_row = vb->vb_scr_size.tp_row;
	p.tp_col = vb->vb_scr_size.tp_col;
	vtbuf_grow(vb, &p, size);
}

void
vtbuf_grow(struct vt_buf *vb, const term_pos_t *p, int history_size)
{
	term_char_t *old, *new, **rows, **oldrows, **copyrows, *row;
	int bufsize, rowssize, w, h, c, r;
	term_rect_t rect;

	history_size = MAX(history_size, p->tp_row);

	if (history_size > vb->vb_history_size || p->tp_col >
	    vb->vb_scr_size.tp_col) {
		/* Allocate new buffer. */
		bufsize = history_size * p->tp_col * sizeof(term_char_t);
		new = malloc(bufsize, M_VTBUF, M_WAITOK | M_ZERO);
		rowssize = history_size * sizeof(term_pos_t *);
		rows = malloc(rowssize, M_VTBUF, M_WAITOK | M_ZERO);

		/* Toggle it. */
		VTBUF_LOCK(vb);
		old = vb->vb_flags & VBF_STATIC ? NULL : vb->vb_buffer;
		oldrows = vb->vb_flags & VBF_STATIC ? NULL : vb->vb_rows;
		copyrows = vb->vb_rows;
		w = vb->vb_scr_size.tp_col;
		h = vb->vb_history_size;

		vb->vb_history_size = history_size;
		vb->vb_buffer = new;
		vb->vb_rows = rows;
		vb->vb_flags &= ~VBF_STATIC;
		vb->vb_scr_size = *p;
		vtbuf_init_rows(vb);

		/* Copy history and fill extra space. */
		for (r = 0; r < history_size; r ++) {
			row = rows[r];
			if (r < h) { /* Copy. */
				memmove(rows[r], copyrows[r],
				    MIN(p->tp_col, w) * sizeof(term_char_t));
				for (c = MIN(p->tp_col, w); c < p->tp_col;
				    c++) {
					row[c] = VTBUF_SPACE_CHAR;
				}
			} else { /* Just fill. */
				rect.tr_begin.tp_col = 0;
				rect.tr_begin.tp_row = r;
				rect.tr_end.tp_col = p->tp_col;
				rect.tr_end.tp_row = p->tp_row;
				vtbuf_fill(vb, &rect, VTBUF_SPACE_CHAR);
				break;
			}
		}
		vtbuf_make_undirty(vb);
		VTBUF_UNLOCK(vb);
		/* Deallocate old buffer. */
		free(old, M_VTBUF);
		free(oldrows, M_VTBUF);
	}
}

void
vtbuf_putchar(struct vt_buf *vb, const term_pos_t *p, term_char_t c)
{
	term_char_t *row;

	KASSERT(p->tp_row < vb->vb_scr_size.tp_row,
	    ("vtbuf_putchar tp_row %d must be less than screen width %d",
		p->tp_row, vb->vb_scr_size.tp_row));
	KASSERT(p->tp_col < vb->vb_scr_size.tp_col,
	    ("vtbuf_putchar tp_col %d must be less than screen height %d",
		p->tp_col, vb->vb_scr_size.tp_col));

	row = vb->vb_rows[(vb->vb_curroffset + p->tp_row) %
	    VTBUF_MAX_HEIGHT(vb)];
	if (row[p->tp_col] != c) {
		VTBUF_LOCK(vb);
		row[p->tp_col] = c;
		VTBUF_UNLOCK(vb);
		vtbuf_dirty_cell(vb, p);
	}
}

void
vtbuf_cursor_position(struct vt_buf *vb, const term_pos_t *p)
{

	if (vb->vb_flags & VBF_CURSOR) {
		vtbuf_dirty_cell(vb, &vb->vb_cursor);
		vb->vb_cursor = *p;
		vtbuf_dirty_cell(vb, &vb->vb_cursor);
	} else {
		vb->vb_cursor = *p;
	}
}

void
vtbuf_mouse_cursor_position(struct vt_buf *vb, int col, int row)
{
	term_rect_t area;

	area.tr_begin.tp_row = MAX(row - 1, 0);
	area.tr_begin.tp_col = MAX(col - 1, 0);
	area.tr_end.tp_row = MIN(row + 2, vb->vb_scr_size.tp_row);
	area.tr_end.tp_col = MIN(col + 2, vb->vb_scr_size.tp_col);
	vtbuf_dirty(vb, &area);
}

void
vtbuf_set_mark(struct vt_buf *vb, int type, int col, int row)
{
	term_rect_t area;
	vt_axis_t tmp;

	switch (type) {
	case VTB_MARK_END:
	case VTB_MARK_EXTEND:
		vb->vb_mark_end.tp_col = col;
		vb->vb_mark_end.tp_row = row;
		break;
	case VTB_MARK_START:
		vb->vb_mark_start.tp_col = col;
		vb->vb_mark_start.tp_row = row;
		/* Start again, so clear end point. */
		vb->vb_mark_end.tp_col = 0;
		vb->vb_mark_end.tp_row = 0;
		break;
	case VTB_MARK_WORD:
		vb->vb_mark_start.tp_col = 0; /* XXX */
		vb->vb_mark_end.tp_col = 10; /* XXX */
		vb->vb_mark_start.tp_row = vb->vb_mark_end.tp_row = row;
		break;
	case VTB_MARK_ROW:
		vb->vb_mark_start.tp_col = 0;
		vb->vb_mark_end.tp_col = vb->vb_scr_size.tp_col;
		vb->vb_mark_start.tp_row = vb->vb_mark_end.tp_row = row;
		break;
	}

	/* Swap start and end if start > end. */
	if (POS_INDEX(vb, vb->vb_mark_start.tp_col, vb->vb_mark_start.tp_row) >
	    POS_INDEX(vb, vb->vb_mark_end.tp_col, vb->vb_mark_end.tp_row)) {
		tmp = vb->vb_mark_start.tp_col;
		vb->vb_mark_start.tp_col = vb->vb_mark_end.tp_col;
		vb->vb_mark_end.tp_col = tmp;
		tmp = vb->vb_mark_start.tp_row;
		vb->vb_mark_start.tp_row = vb->vb_mark_end.tp_row;
		vb->vb_mark_end.tp_row = tmp;
	}

	/* Notify renderer to update marked region. */
	if (vb->vb_mark_start.tp_col || vb->vb_mark_end.tp_col ||
	    vb->vb_mark_start.tp_row || vb->vb_mark_end.tp_row) {

		area.tr_begin.tp_col = 0;
		area.tr_begin.tp_row = MIN(vb->vb_mark_start.tp_row,
		    vb->vb_mark_end.tp_row);

		area.tr_end.tp_col = vb->vb_scr_size.tp_col;
		area.tr_end.tp_row = MAX(vb->vb_mark_start.tp_row,
		    vb->vb_mark_end.tp_row);

		vtbuf_dirty(vb, &area);
	}

}

void
vtbuf_cursor_visibility(struct vt_buf *vb, int yes)
{
	int oflags, nflags;

	VTBUF_LOCK(vb);
	oflags = vb->vb_flags;
	if (yes)
		vb->vb_flags |= VBF_CURSOR;
	else
		vb->vb_flags &= ~VBF_CURSOR;
	nflags = vb->vb_flags;
	VTBUF_UNLOCK(vb);

	if (oflags != nflags)
		vtbuf_dirty_cell(vb, &vb->vb_cursor);
}
