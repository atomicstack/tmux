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
#include <time.h>

#include "tmux.h"

/*
 * OSC 7501 program status. Programs report a state (and optionally an id,
 * kind, progress, app, title and message) and tmux keeps one record per id
 * on the pane. See https://www.superlogical.com/rex/docs/build/program-status.
 */

/* Limits from the specification. */
#define PROGRAM_STATUS_MAX_BODY 4096
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

/* Find a record by id; NULL is the root record. */
struct program_status *
program_status_find(struct window_pane *wp, const char *id)
{
	struct program_status	*ps;

	TAILQ_FOREACH(ps, &wp->program_statuses, entry) {
		if (id == NULL) {
			if (ps->id == NULL)
				return (ps);
		} else if (ps->id != NULL && strcmp(ps->id, id) == 0)
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

	TAILQ_FOREACH_SAFE(ps, &wp->program_statuses, entry, ps1) {
		if (ps->state == PROGRAM_STATUS_WORKING ||
		    ps->state == PROGRAM_STATUS_BLOCKED)
			program_status_remove(wp, ps);
	}
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

/* Is this a valid value: only [A-Za-z0-9_.,+/=-]? */
static int
program_status_valid_value(const char *value)
{
	const char	*cp;

	for (cp = value; *cp != '\0'; cp++) {
		if ((*cp >= 'A' && *cp <= 'Z') ||
		    (*cp >= 'a' && *cp <= 'z') ||
		    (*cp >= '0' && *cp <= '9') ||
		    strchr("_.,+/=-", *cp) != NULL)
			continue;
		return (0);
	}
	return (1);
}

/* Is this a valid id: segments of 1-32 bytes of [A-Za-z0-9_.+-], 8 deep? */
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
			if (segment == 0 || segment > PROGRAM_STATUS_MAX_SEGMENT)
				return (0);
			if (*cp == '\0')
				break;
			if (++depth > PROGRAM_STATUS_MAX_DEPTH)
				return (0);
			segment = 0;
			continue;
		}
		if ((*cp >= 'A' && *cp <= 'Z') ||
		    (*cp >= 'a' && *cp <= 'z') ||
		    (*cp >= '0' && *cp <= '9') ||
		    strchr("_.+-", *cp) != NULL) {
			segment++;
			continue;
		}
		return (0);
	}
	return (1);
}

/*
 * Decode a base64 value of at most maxenc encoded and maxdec decoded bytes,
 * rejecting control characters. Returns NULL if the value is invalid or
 * empty (*bad is set only when invalid).
 */
static char *
program_status_decode(const char *value, size_t maxenc, size_t maxdec,
    int *bad)
{
	size_t	 len = strlen(value), i;
	char	*padded;
	u_char	*out;
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
	outlen = b64_pton(padded, out, len + 1);
	free(padded);
	if (outlen == -1 || (size_t)outlen > maxdec) {
		free(out);
		*bad = 1;
		return (NULL);
	}
	for (i = 0; i < (size_t)outlen; i++) {
		if (out[i] < 0x20 || out[i] == 0x7f) {
			free(out);
			*bad = 1;
			return (NULL);
		}
	}
	if (outlen == 0) {
		free(out);
		return (NULL);
	}
	out[outlen] = '\0';
	return ((char *)out);
}

/*
 * Parse a report body into ps. Returns 0 and a record the caller must free
 * with program_status_free, or -1 if the report must be discarded.
 */
int
program_status_parse(const char *body, struct program_status *ps)
{
	char		*copy, *next, *pair, *value;
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
		if (!program_status_valid_key(pair) ||
		    !program_status_valid_value(value))
			continue;
		if (strcmp(pair, "state") == 0)
			state = value;
		else if (strcmp(pair, "id") == 0)
			id = value;
		else if (strcmp(pair, "kind") == 0)
			kind = value;
		else if (strcmp(pair, "progress") == 0)
			progress = value;
		else if (strcmp(pair, "app") == 0)
			app = value;
		else if (strcmp(pair, "title") == 0)
			title = value;
		else if (strcmp(pair, "msg") == 0)
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
	if (app != NULL && *app != '\0') {
		if (strlen(app) > PROGRAM_STATUS_MAX_APP)
			goto bad;
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

	ps->time = time(NULL);
	free(copy);
	return (0);

bad:
	program_status_free(ps);
	free(copy);
	return (-1);
}

/*
 * Apply a parsed report to a pane. Returns 1 if the pane changed or 0 if
 * the report was dropped (record cap reached). Strings are copied.
 */
int
program_status_apply(struct window_pane *wp, const struct program_status *ps)
{
	struct program_status	*found, *found1;

	if (ps->state == PROGRAM_STATUS_CLEAR) {
		TAILQ_FOREACH_SAFE(found, &wp->program_statuses, entry,
		    found1) {
			if ((ps->id == NULL && found->id == NULL) ||
			    (ps->id != NULL && found->id != NULL &&
			    strcmp(found->id, ps->id) == 0) ||
			    program_status_is_child(found->id, ps->id))
				program_status_remove(wp, found);
		}
		return (1);
	}

	found = program_status_find(wp, ps->id);
	if (found == NULL) {
		if (wp->program_status_count >= PROGRAM_STATUS_MAX_RECORDS)
			return (0);
		found = xcalloc(1, sizeof *found);
		TAILQ_INSERT_TAIL(&wp->program_statuses, found, entry);
		wp->program_status_count++;
	} else
		program_status_free(found);

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
	found->time = ps->time;
	return (1);
}
