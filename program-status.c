/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Matt Koscica <matt.koscica@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <resolv.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * OSC 7501 program status. Programs report a state (and optionally an id,
 * kind, progress, app, title and message) and tmux keeps one record per id
 * on the pane. See https://www.superlogical.com/rex/docs/build/program-status.
 *
 * The record list is kept in least recently updated order (oldest first), so
 * that the record to evict when the cap is reached is always at the head.
 */

/*
 * Limits from the specification. The sequence limit covers the whole
 * sequence, so the body gets 9 bytes less for the OSC ("\033]7501;") and
 * ST ("\033\\").
 */
#define PROGRAM_STATUS_MAX_SEQUENCE 4096
#define PROGRAM_STATUS_MAX_BODY (PROGRAM_STATUS_MAX_SEQUENCE - 9)
#define PROGRAM_STATUS_MAX_KEY 16
#define PROGRAM_STATUS_MAX_APP 32
#define PROGRAM_STATUS_MAX_ID 128
#define PROGRAM_STATUS_MAX_SEGMENT 32
#define PROGRAM_STATUS_MAX_DEPTH 8
#define PROGRAM_STATUS_MAX_TITLE_ENCODED 256
#define PROGRAM_STATUS_MAX_TITLE 192
#define PROGRAM_STATUS_MAX_MSG_ENCODED 2732
#define PROGRAM_STATUS_MAX_MSG 2048
#define PROGRAM_STATUS_MAX_RECORDS 256

static const char *program_status_states[] = {
	"idle", "working", "done", "blocked", "error", "clear"
};
static const char *program_status_kinds[] = {
	"", "permission", "question", "auth"
};

/* Initialize the record list on a pane. */
void
program_status_init(struct window_pane *wp)
{
	TAILQ_INIT(&wp->program_statuses);
	wp->program_status_count = 0;
}

/* Free the strings in a record (not the record itself). */
void
program_status_free(struct program_status *ps)
{
	free(ps->id);
	free(ps->app);
	free(ps->title);
	free(ps->msg);
	ps->id = ps->app = ps->title = ps->msg = NULL;
}

/* Remove and free a stored record. */
static void
program_status_remove(struct window_pane *wp, struct program_status *ps)
{
	TAILQ_REMOVE(&wp->program_statuses, ps, entry);
	wp->program_status_count--;
	program_status_free(ps);
	free(ps);
}

/* Free all records on a pane. */
void
program_status_free_all(struct window_pane *wp)
{
	struct program_status	*ps, *ps1;

	TAILQ_FOREACH_SAFE(ps, &wp->program_statuses, entry, ps1)
		program_status_remove(wp, ps);
}

/* Redraw what shows the records after they have changed. */
static void
program_status_changed(struct window_pane *wp)
{
	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
}

/* Are two ids the same? NULL is the root record. */
static int
program_status_same_id(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return (a == b);
	return (strcmp(a, b) == 0);
}

/* Find a record by id; NULL is the root record. */
struct program_status *
program_status_find(struct window_pane *wp, const char *id)
{
	struct program_status	*ps;

	TAILQ_FOREACH(ps, &wp->program_statuses, entry) {
		if (program_status_same_id(ps->id, id))
			return (ps);
	}
	return (NULL);
}

/* Is id strictly below parent? The root (NULL) is above every id. */
static int
program_status_is_child(const char *id, const char *parent)
{
	size_t	len;

	if (id == NULL)
		return (0);
	if (parent == NULL)
		return (1);
	len = strlen(parent);
	return (strncmp(id, parent, len) == 0 && id[len] == '/');
}

/* Drop working and blocked records (process exit or new prompt). */
void
program_status_prompt(struct window_pane *wp)
{
	struct program_status	*ps, *ps1;
	int			 removed = 0;

	TAILQ_FOREACH_SAFE(ps, &wp->program_statuses, entry, ps1) {
		if (ps->state == PROGRAM_STATUS_WORKING ||
		    ps->state == PROGRAM_STATUS_BLOCKED) {
			program_status_remove(wp, ps);
			removed = 1;
		}
	}
	if (removed)
		program_status_changed(wp);
}

/* Get the app for a record, inheriting from the nearest ancestor. */
const char *
program_status_app(struct window_pane *wp, const struct program_status *ps)
{
	struct program_status	*parent;
	const char		*app = NULL;
	char			*copy, *cp;

	if (ps->app != NULL)
		return (ps->app);
	if (ps->id == NULL)
		return (NULL);

	copy = xstrdup(ps->id);
	while (app == NULL && (cp = strrchr(copy, '/')) != NULL) {
		*cp = '\0';
		parent = program_status_find(wp, copy);
		if (parent != NULL)
			app = parent->app;
	}
	free(copy);

	if (app == NULL) {
		parent = program_status_find(wp, NULL);
		if (parent != NULL)
			app = parent->app;
	}
	return (app);
}

/* Name of a state. */
const char *
program_status_state_name(enum program_status_state state)
{
	return (program_status_states[state]);
}

/* Name of a kind ("" for none). */
const char *
program_status_kind_name(enum program_status_kind kind)
{
	return (program_status_kinds[kind]);
}

/*
 * Strip spaces from both ends of a key or value, in place. Only 0x20 can
 * reach the body: the OSC parser drops the other whitespace bytes.
 */
static char *
program_status_trim(char *s)
{
	char	*end;

	while (*s == ' ')
		s++;
	end = s + strlen(s);
	while (end > s && end[-1] == ' ')
		*--end = '\0';
	return (s);
}

/* Is this a valid key: 1-16 bytes of [a-z]? */
static int
program_status_valid_key(const char *key)
{
	size_t	len = strlen(key), i;

	if (len == 0 || len > PROGRAM_STATUS_MAX_KEY)
		return (0);
	for (i = 0; i < len; i++) {
		if (key[i] < 'a' || key[i] > 'z')
			return (0);
	}
	return (1);
}

/* Is this a byte from the value set [A-Za-z0-9_.,+/=-]? */
static int
program_status_valid_value_byte(char c)
{
	return ((c >= 'A' && c <= 'Z') ||
	    (c >= 'a' && c <= 'z') ||
	    (c >= '0' && c <= '9') ||
	    strchr("_.,+/=-", c) != NULL);
}

/* Is this a valid value: only bytes from the value set? */
static int
program_status_valid_value(const char *value)
{
	const char	*cp;

	for (cp = value; *cp != '\0'; cp++) {
		if (!program_status_valid_value_byte(*cp))
			return (0);
	}
	return (1);
}

/* Is this a byte from the segment (and app) set [A-Za-z0-9_.+-]? */
static int
program_status_valid_segment_byte(char c)
{
	return ((c >= 'A' && c <= 'Z') ||
	    (c >= 'a' && c <= 'z') ||
	    (c >= '0' && c <= '9') ||
	    strchr("_.+-", c) != NULL);
}

/* Is this a valid app: 1-32 bytes from the segment set? */
static int
program_status_valid_app(const char *app)
{
	const char	*cp;

	if (*app == '\0' || strlen(app) > PROGRAM_STATUS_MAX_APP)
		return (0);
	for (cp = app; *cp != '\0'; cp++) {
		if (!program_status_valid_segment_byte(*cp))
			return (0);
	}
	return (1);
}

/* Is this a valid id: segments of 1-32 bytes from the segment set, 8 deep? */
static int
program_status_valid_id(const char *id)
{
	const char	*cp;
	size_t		 segment = 0;
	u_int		 depth = 1;

	if (strlen(id) > PROGRAM_STATUS_MAX_ID)
		return (0);
	for (cp = id;; cp++) {
		if (*cp == '/' || *cp == '\0') {
			if (segment == 0 ||
			    segment > PROGRAM_STATUS_MAX_SEGMENT)
				return (0);
			if (*cp == '\0')
				break;
			if (++depth > PROGRAM_STATUS_MAX_DEPTH)
				return (0);
			segment = 0;
			continue;
		}
		if (!program_status_valid_segment_byte(*cp))
			return (0);
		segment++;
	}
	return (1);
}

/*
 * Decode a base64 value of at most maxenc encoded and maxdec decoded bytes.
 * The text must be valid UTF-8 with no control characters (C0, DEL or C1)
 * and "#(" is neutered as for titles so it cannot reach #{E:}. Returns NULL
 * if the value is invalid or empty (*bad is set only when invalid).
 */
static char *
program_status_decode(const char *value, size_t maxenc, size_t maxdec,
    int *bad)
{
	size_t	 len = strlen(value);
	char	*padded, *out, *cp;
	int	 outlen;

	*bad = 0;
	if (len == 0)
		return (NULL);
	if (len > maxenc) {
		*bad = 1;
		return (NULL);
	}

	/* Padding is optional in the protocol but not for b64_pton. */
	padded = xmalloc(len + 4);
	strlcpy(padded, value, len + 4);
	while (strlen(padded) % 4 != 0)
		strlcat(padded, "=", len + 4);

	out = xmalloc(len + 1);
	outlen = b64_pton(padded, (u_char *)out, len + 1);
	free(padded);
	if (outlen == -1 || (size_t)outlen > maxdec)
		goto bad;
	out[outlen] = '\0';
	if (outlen == 0) {
		free(out);
		return (NULL);
	}

	/*
	 * utf8_isvalid rejects C0 controls, DEL and invalid UTF-8 but stops at
	 * an embedded NUL (also a control character) and accepts the C1
	 * controls U+0080 to U+009F, which are 0xc2 followed by 0x80 to 0x9f
	 * (0xc2 is only ever a lead byte in valid UTF-8).
	 */
	if (strlen(out) != (size_t)outlen || !utf8_isvalid(out))
		goto bad;
	for (cp = out; *cp != '\0'; cp++) {
		if ((u_char)cp[0] == 0xc2 &&
		    (u_char)cp[1] >= 0x80 && (u_char)cp[1] <= 0x9f)
			goto bad;
		if (cp[0] == '#' && cp[1] == '(')
			*cp = '_';
	}
	return (out);

bad:
	free(out);
	*bad = 1;
	return (NULL);
}

/*
 * Parse a report body into ps. Returns 0 and a record the caller must free
 * with program_status_free, or -1 if the report must be discarded.
 */
int
program_status_parse(const char *body, struct program_status *ps)
{
	char		*copy, *next, *pair, *key, *value;
	const char	*state = NULL, *id = NULL, *kind = NULL;
	const char	*progress = NULL, *app = NULL;
	const char	*title = NULL, *msg = NULL;
	const char	*errstr;
	u_int		 i;
	long long	 n;
	int		 bad;

	memset(ps, 0, sizeof *ps);
	ps->progress = -1;

	if (strlen(body) > PROGRAM_STATUS_MAX_BODY)
		return (-1);
	copy = xstrdup(body);

	next = copy;
	while ((pair = strsep(&next, ":")) != NULL) {
		value = strchr(pair, '=');
		if (value == NULL)
			continue;
		*value++ = '\0';
		key = program_status_trim(pair);
		value = program_status_trim(value);
		if (!program_status_valid_key(key))
			continue;

		/*
		 * A malformed id is not skipped like other malformed pairs,
		 * since that would make the report address the root record
		 * instead: it is checked against the id grammar below and
		 * discards the report.
		 */
		if (strcmp(key, "id") == 0) {
			id = value;
			continue;
		}
		if (!program_status_valid_value(value))
			continue;
		if (strcmp(key, "state") == 0)
			state = value;
		else if (strcmp(key, "kind") == 0)
			kind = value;
		else if (strcmp(key, "progress") == 0)
			progress = value;
		else if (strcmp(key, "app") == 0)
			app = value;
		else if (strcmp(key, "title") == 0)
			title = value;
		else if (strcmp(key, "msg") == 0)
			msg = value;
	}

	if (state == NULL)
		goto bad;
	for (i = 0; i < nitems(program_status_states); i++) {
		if (strcmp(state, program_status_states[i]) == 0)
			break;
	}
	if (i == nitems(program_status_states))
		goto bad;
	ps->state = i;

	if (id != NULL && *id != '\0') {
		if (!program_status_valid_id(id))
			goto bad;
		ps->id = xstrdup(id);
	}

	/*
	 * An app over the length limit discards the report; one outside the
	 * character set is treated as absent.
	 */
	if (app != NULL) {
		if (strlen(app) > PROGRAM_STATUS_MAX_APP)
			goto bad;
		if (program_status_valid_app(app))
			ps->app = xstrdup(app);
	}
	if (title != NULL) {
		ps->title = program_status_decode(title,
		    PROGRAM_STATUS_MAX_TITLE_ENCODED, PROGRAM_STATUS_MAX_TITLE,
		    &bad);
		if (bad)
			goto bad;
	}
	if (msg != NULL) {
		ps->msg = program_status_decode(msg,
		    PROGRAM_STATUS_MAX_MSG_ENCODED, PROGRAM_STATUS_MAX_MSG,
		    &bad);
		if (bad)
			goto bad;
	}

	if (ps->state == PROGRAM_STATUS_BLOCKED && kind != NULL) {
		for (i = 1; i < nitems(program_status_kinds); i++) {
			if (strcmp(kind, program_status_kinds[i]) == 0) {
				ps->kind = i;
				break;
			}
		}
	}
	if ((ps->state == PROGRAM_STATUS_WORKING ||
	    ps->state == PROGRAM_STATUS_BLOCKED) && progress != NULL) {
		n = strtonum(progress, 0, 100, &errstr);
		if (errstr == NULL)
			ps->progress = n;
	}

	free(copy);
	return (0);

bad:
	program_status_free(ps);
	free(copy);
	return (-1);
}

/* Are two optional strings the same? */
static int
program_status_same_string(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return (a == b);
	return (strcmp(a, b) == 0);
}

/* Do two records (with the same id) hold the same report? */
static int
program_status_same(const struct program_status *a,
    const struct program_status *b)
{
	return (a->state == b->state &&
	    a->kind == b->kind &&
	    a->progress == b->progress &&
	    program_status_same_string(a->app, b->app) &&
	    program_status_same_string(a->title, b->title) &&
	    program_status_same_string(a->msg, b->msg));
}

/*
 * Apply a parsed report to a pane. Returns 1 if a record changed or 0 if
 * the report left the records as they were (a repeat of the same report, or
 * a clear that matched nothing). Strings are copied.
 */
int
program_status_apply(struct window_pane *wp, const struct program_status *ps)
{
	struct program_status	*found, *found1;
	int			 removed = 0;

	if (ps->state == PROGRAM_STATUS_CLEAR) {
		if (ps->id == NULL) {
			removed = (wp->program_status_count != 0);
			program_status_free_all(wp);
			return (removed);
		}
		TAILQ_FOREACH_SAFE(found, &wp->program_statuses, entry,
		    found1) {
			if (program_status_same_id(found->id, ps->id) ||
			    program_status_is_child(found->id, ps->id)) {
				program_status_remove(wp, found);
				removed = 1;
			}
		}
		return (removed);
	}

	/*
	 * An updated record moves to the tail so the head is always the
	 * least recently updated record, which is the one evicted when a new
	 * record would exceed the cap.
	 */
	found = program_status_find(wp, ps->id);
	if (found != NULL) {
		TAILQ_REMOVE(&wp->program_statuses, found, entry);
		TAILQ_INSERT_TAIL(&wp->program_statuses, found, entry);
		if (program_status_same(found, ps))
			return (0);
		program_status_free(found);
	} else {
		if (wp->program_status_count >= PROGRAM_STATUS_MAX_RECORDS) {
			program_status_remove(wp,
			    TAILQ_FIRST(&wp->program_statuses));
		}
		found = xcalloc(1, sizeof *found);
		TAILQ_INSERT_TAIL(&wp->program_statuses, found, entry);
		wp->program_status_count++;
	}

	if (ps->id != NULL)
		found->id = xstrdup(ps->id);
	found->state = ps->state;
	found->kind = ps->kind;
	found->progress = ps->progress;
	if (ps->app != NULL)
		found->app = xstrdup(ps->app);
	if (ps->title != NULL)
		found->title = xstrdup(ps->title);
	if (ps->msg != NULL)
		found->msg = xstrdup(ps->msg);
	return (1);
}
