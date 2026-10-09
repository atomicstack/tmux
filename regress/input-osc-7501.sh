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

# msg and title are base64; padding is optional.
start_pane msg 40 5 \
    '\033]7501;state=done:msg=QXBwbHkgMyB0byBhZGQ=:title=ZGVwbG95\033\\X\n'
check_format msg '#{pane_program_msg}' 'Apply 3 to add'
check_format msg '#{pane_program_title}' 'deploy'
start_pane unpadded 40 5 '\033]7501;state=done:msg=QXBwbHk\033\\X\n'
check_format unpadded '#{pane_program_msg}' 'Apply'

# An empty msg is absent, not an error.
start_pane emptymsg 40 5 '\033]7501;state=done:msg=\033\\X\n'
check_format emptymsg '#{pane_program_state}' 'done'
check_format emptymsg '#{pane_program_msg}' ''

# A control character in the decoded text discards the whole report
# ("a\nb" is YQpi; "a\x7fb" is YX9i), as does base64 that does not
# decode (a single character cannot form a byte).
start_pane ctrl 40 5 \
    '\033]7501;state=working\033\\\033]7501;state=done:msg=YQpi\033\\\033]7501;state=done:msg=YX9i\033\\\033]7501;state=done:msg=A\033\\X\n'
check_format ctrl '#{pane_program_state}' 'working'

# UTF-8 survives decoding ("héllo" is aMOpbGxv).
start_pane utf8 40 5 '\033]7501;state=done:msg=aMOpbGxv\033\\X\n'
check_format utf8 '#{pane_program_msg}' 'héllo'

# Over the limits: a title of 260 encoded bytes and a body over 4096
# bytes are both discarded (an unknown key, so that only the body limit
# can reject it).
t=$(printf 'A%.0s' $(seq 1 260))
start_pane longtitle 40 5 "\\033]7501;state=done:title=$t\\033\\\\X\\n"
check_format longtitle '#{pane_program_count}' '0'
m=$(printf 'A%.0s' $(seq 1 4100))
start_pane longbody 40 5 "\\033]7501;state=done:junk=$m\\033\\\\X\\n"
check_format longbody '#{pane_program_count}' '0'

# An OSC 133 A prompt drops working and blocked but keeps done and error.
start_pane prompt 40 5 \
    '\033]7501;state=working:id=w\033\\\033]7501;state=blocked:id=b\033\\\033]7501;state=done\033\\\033]7501;state=error:id=e\033\\\033]133;A\033\\X\n'
check_format prompt '#{pane_program_count}' '2'
check_format prompt '#{pane_program_state}' 'done'

# RIS removes everything.
start_pane ris 40 5 \
    '\033]7501;state=done\033\\\033]7501;state=error:id=e\033\\\033cX\n'
check_format ris '#{pane_program_count}' '0'

# DECSTR does not.
start_pane decstr 40 5 '\033]7501;state=working\033\\\033[!pX\n'
check_format decstr '#{pane_program_state}' 'working'

# Process exit drops working but keeps done (remain-on-exit keeps the
# pane around so the formats can be read).
$TMUX kill-server 2>/dev/null
sleep 0.1
$TMUX new-session -d -x 40 -y 5 -s exited \
    "printf '\\033]7501;state=working:id=w\\033\\\\\\033]7501;state=done\\033\\\\'; sleep 0.3" || exit 1
$TMUX set-option -t exited: remain-on-exit on || exit 1
sleep 1
check_format exited '#{pane_dead}' '1'
check_format exited '#{pane_program_count}' '1'
check_format exited '#{pane_program_state}' 'done'

# Respawn starts from no records.
$TMUX respawn-pane -k -t exited: "printf X; $INPUT_HOLD" || exit 1
sleep 0.3
check_format exited '#{pane_program_count}' '0'

# The feature query is answered with the same body and terminator; with
# the option off there is no reply and reports are ignored.
query()
{
	name=$1
	expected=$2
	seq=$3
	count=$4

	$TMUX kill-server 2>/dev/null
	sleep 0.1
	$TMUX new-session -d -x 40 -y 5 -s "$name" \
	    "stty raw -echo min 0 time 10; printf '$seq'; dd bs=1 count=$count 2>/dev/null | cat -v >$TMP; $INPUT_HOLD" || exit 1
	sleep 1.5
	printf '%s' "$expected" >"$EXP"
	cmp -s "$TMP" "$EXP" || fail "$name"
}
query query-st '^[]7501;?^[\' '\033]7501;?\033\\' 11
query query-bel '^[]7501;?^G' '\033]7501;?\007' 10

$TMUX kill-server 2>/dev/null
sleep 0.1
$TMUX new-session -d -x 40 -y 5 -s off "$INPUT_HOLD" || exit 1
$TMUX set-option -g allow-program-status off || exit 1
$TMUX respawn-pane -k -t off: \
    "stty raw -echo min 0 time 10; printf '\\033]7501;?\\033\\\\\\033]7501;state=working\\033\\\\'; dd bs=1 count=11 2>/dev/null | cat -v >$TMP; $INPUT_HOLD" || exit 1
sleep 1.5
printf '' >"$EXP"
cmp -s "$TMP" "$EXP" || fail "off-no-reply"
check_format off '#{pane_program_state}' ''

$TMUX kill-server 2>/dev/null
exit $exit_status
