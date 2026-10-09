#!/bin/sh

# OSC 7501 program status: records are stored per pane and the root
# record is exposed through the pane_program_* formats.

. ./input-common.inc

LC_ALL=C.UTF-8
LANG=C.UTF-8
export LC_ALL LANG

check_format()
{
	name=$1
	fmt=$2
	expected=$3

	printf '%s\n' "$expected" >"$EXP"
	$TMUX display-message -p -t "$name:" "$fmt" >"$TMP" 2>&1
	cmp -s "$EXP" "$TMP" || fail "$name $fmt"
}

# A working report with progress and app sets the root record.
start_pane basic 40 5 \
    '\033]7501;state=working:progress=40:app=cargo\033\\X\n'
check_capture basic 'X'
check_format basic '#{pane_program_state}' 'working'
check_format basic '#{pane_program_progress}' '40'
check_format basic '#{pane_program_app}' 'cargo'
check_format basic '#{pane_program_kind}' ''
check_format basic '#{pane_program_count}' '1'

# BEL is accepted as the terminator too.
start_pane bel 40 5 '\033]7501;state=error\007X\n'
check_format bel '#{pane_program_state}' 'error'

# No root record: every format is empty and the count is 0.
start_pane empty 40 5 'X\n'
check_format empty '#{pane_program_state}' ''
check_format empty '#{pane_program_progress}' ''
check_format empty '#{pane_program_count}' '0'

# A report replaces its record completely: the second one has no app.
start_pane replace 40 5 \
    '\033]7501;state=working:app=cargo\033\\\033]7501;state=done\033\\X\n'
check_format replace '#{pane_program_state}' 'done'
check_format replace '#{pane_program_app}' ''
check_format replace '#{pane_program_count}' '1'

# Unknown state: the whole report is ignored and the old record stays.
start_pane unknown 40 5 \
    '\033]7501;state=working\033\\\033]7501;state=snoozing:app=x\033\\X\n'
check_format unknown '#{pane_program_state}' 'working'
check_format unknown '#{pane_program_app}' ''

# No state at all: ignored.
start_pane nostate 40 5 '\033]7501;app=cargo\033\\X\n'
check_format nostate '#{pane_program_count}' '0'

# A malformed pair (no '=', bad key, bad value byte) is skipped but the
# rest of the report applies; repeated keys: last wins; unknown keys are
# ignored.
start_pane malformed 40 5 \
    '\033]7501;junk::APP=no:app=a b:state=idle:state=working:zzz=1:app=ok:\033\\X\n'
check_format malformed '#{pane_program_state}' 'working'
check_format malformed '#{pane_program_app}' 'ok'

# kind only applies to blocked; progress only to working or blocked.
start_pane blocked 40 5 \
    '\033]7501;state=blocked:kind=permission:progress=10\033\\X\n'
check_format blocked '#{pane_program_kind}' 'permission'
check_format blocked '#{pane_program_progress}' '10'
start_pane donekind 40 5 \
    '\033]7501;state=done:kind=permission:progress=10\033\\X\n'
check_format donekind '#{pane_program_kind}' ''
check_format donekind '#{pane_program_progress}' ''
start_pane badkind 40 5 \
    '\033]7501;state=blocked:kind=coffee:progress=101\033\\X\n'
check_format badkind '#{pane_program_state}' 'blocked'
check_format badkind '#{pane_program_kind}' ''
check_format badkind '#{pane_program_progress}' ''

# app longer than 32 bytes discards the report.
start_pane longapp 40 5 \
    '\033]7501;state=idle:app=abcdefghijklmnopqrstuvwxyz0123456\033\\X\n'
check_format longapp '#{pane_program_count}' '0'

# An empty id addresses the root record.
start_pane emptyid 40 5 '\033]7501;state=idle:id=\033\\X\n'
check_format emptyid '#{pane_program_state}' 'idle'
check_format emptyid '#{pane_program_count}' '1'

# Bad ids (empty segment, too deep, a value byte that is not a segment
# byte) discard the report. (A byte outside the value set, such as '!',
# makes the pair malformed instead, so the report then addresses the root.)
start_pane badid 40 5 \
    '\033]7501;state=idle:id=a//b\033\\\033]7501;state=idle:id=1/2/3/4/5/6/7/8/9\033\\\033]7501;state=idle:id=,\033\\X\n'
check_format badid '#{pane_program_count}' '0'

# Children: a/b, a and the root are three records; clearing a removes a
# and a/b but not ab; clearing with no id removes everything.
start_pane children 40 5 \
    '\033]7501;state=working:id=a/b\033\\\033]7501;state=working:id=a\033\\\033]7501;state=working:id=ab\033\\\033]7501;state=working\033\\X\n'
check_format children '#{pane_program_count}' '4'
$TMUX respawn-pane -k -t children: \
    "printf '\\033]7501;state=working:id=a/b\\033\\\\\\033]7501;state=working:id=a\\033\\\\\\033]7501;state=working:id=ab\\033\\\\\\033]7501;state=working\\033\\\\\\033]7501;state=clear:id=a\\033\\\\'; $INPUT_HOLD" || exit 1
sleep 0.3
check_format children '#{pane_program_count}' '2'
check_format children '#{pane_program_state}' 'working'
$TMUX respawn-pane -k -t children: \
    "printf '\\033]7501;state=working:id=a\\033\\\\\\033]7501;state=working\\033\\\\\\033]7501;state=clear\\033\\\\'; $INPUT_HOLD" || exit 1
sleep 0.3
check_format children '#{pane_program_count}' '0'

# The 257th distinct record is dropped; existing ones still update.
cmd='i=0; while [ "$i" -lt 257 ]; do '
cmd="${cmd}printf '\\033]7501;state=idle:id=r%d\\033\\\\' \"\$i\"; i=\$((i + 1)); done; "
cmd="${cmd}printf '\\033]7501;state=done:id=r0\\033\\\\'; $INPUT_HOLD"
start_cmd cap 40 5 "$cmd"
sleep 0.5
check_format cap '#{pane_program_count}' '256'

$TMUX kill-server 2>/dev/null
exit $exit_status
