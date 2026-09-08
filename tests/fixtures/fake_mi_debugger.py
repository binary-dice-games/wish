#!/usr/bin/env python3
# MIT License (c) 2026 Binary Dice Games
#
# A tiny scripted stand-in for `gdb --interpreter=mi`, used by
# tests/test_posix_debug_backend.cpp to exercise posix_debug_backend's
# subprocess plumbing, MI command/response correlation, stream capture, and
# *stopped -> on_stop dispatch on a host with no real debugger installed.
#
# It implements just enough of the GDB/MI protocol for one canned session:
# attach -> break-insert -> exec-continue (breakpoint hit) -> thread-info /
# stack-list-frames / evaluate -> exec-step -> detach -> exit. It does NOT
# touch any real process; the pid passed to -target-attach is ignored.
import sys

FULLNAME = "/tmp/dbg_fixture.cpp"


def emit(line):
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def frame(func, line, level=None):
    lvl = 'level="%d",' % level if level is not None else ""
    return ('{%saddr="0x400abc",func="%s",file="dbg_fixture.cpp",'
            'fullname="%s",line="%d"}' % (lvl, func, FULLNAME, line))


def main():
    emit("(gdb) ")
    for raw in sys.stdin:
        cmd = raw.strip()
        if not cmd:
            continue
        # Split the leading numeric token from the command body.
        i = 0
        while i < len(cmd) and cmd[i].isdigit():
            i += 1
        token, body = cmd[:i], cmd[i:]

        if body.startswith("-gdb-set") or body.startswith("-break-delete"):
            emit(token + "^done")
        elif body.startswith("-target-attach"):
            emit('=thread-group-started,id="i1",pid="999"')
            emit('=thread-created,id="1",group-id="i1"')
            emit(token + "^done")
            emit('*stopped,frame=%s,thread-id="1",stopped-threads="all"' % frame("main", 41))
        elif body.startswith("-break-insert"):
            emit(token + '^done,bkpt={number="1",type="breakpoint",disp="keep",'
                 'enabled="y",addr="0x400abc",func="inner_function",'
                 'file="dbg_fixture.cpp",fullname="%s",line="24",times="0"}' % FULLNAME)
        elif body.startswith("-exec-continue"):
            emit(token + "^running")
            emit('*running,thread-id="all"')
            emit('*stopped,reason="breakpoint-hit",disp="keep",bkptno="1",'
                 'frame=%s,thread-id="1",stopped-threads="all"' % frame("inner_function", 24))
        elif body.startswith("-exec-step") or body.startswith("-exec-next"):
            emit(token + "^running")
            emit('*running,thread-id="all"')
            emit('*stopped,reason="end-stepping-range",frame=%s,thread-id="1"'
                 % frame("inner_function", 25))
        elif body.startswith("-exec-finish"):
            emit(token + "^running")
            emit('*running,thread-id="all"')
            emit('*stopped,reason="function-finished",frame=%s,thread-id="1"'
                 % frame("outer_function", 29))
        elif body.startswith("-exec-interrupt"):
            emit(token + "^done")
            emit('*stopped,reason="signal-received",signal-name="SIGINT",'
                 'frame=%s,thread-id="1"' % frame("inner_function", 24))
        elif body.startswith("-thread-info"):
            emit(token + '^done,threads=[{id="1",target-id="Thread 1",'
                 'frame=%s,state="stopped"}],current-thread-id="1"' % frame("inner_function", 24, 0))
        elif body.startswith("-stack-list-frames"):
            emit(token + "^done,stack=[frame=%s,frame=%s,frame=%s]"
                 % (frame("inner_function", 24, 0),
                    frame("outer_function", 29, 1),
                    frame("main", 41, 2)))
        elif body.startswith("-data-evaluate-expression"):
            emit(token + '^done,value="3"')
        elif body.startswith("-interpreter-exec"):
            emit('~"type = int\\n"')
            emit(token + "^done")
        elif body.startswith("-target-detach"):
            emit(token + "^done")
        elif body.startswith("-gdb-exit"):
            emit(token + "^exit")
            return
        else:
            emit(token + '^error,msg="fake_mi_debugger: unhandled command"')
        emit("(gdb) ")


if __name__ == "__main__":
    main()
