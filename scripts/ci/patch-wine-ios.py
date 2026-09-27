#!/usr/bin/env python3
"""CI-side edits to the Wine fork (submodule `wine`, not pushable from this
repository). Same contract as patch-fex-ios.py: each patch is idempotent and
fails loudly if its anchor is gone, so a fork update that already carries the
change makes the patch obsolete instead of silently wrong."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2] / "wine"
MARK = "/* CI: patch-wine-ios */"


def patch(rel, edit):
    path = ROOT / rel
    text = path.read_text()
    tag = f"patch-wine-ios:{edit.__name__}"
    if tag in text:
        print(f"already patched: {rel} ({edit.__name__})")
        return
    new = edit(text)
    if new == text or MARK not in new:
        sys.exit(f"patch-wine-ios: anchor not found in {rel} ({edit.__name__})")
    path.write_text(new.replace(MARK, f"/* CI: {tag} */"))
    print(f"patched: {rel} ({edit.__name__})")


def replace_once(text, old, new):
    return text.replace(old, new, 1) if text.count(old) == 1 else text


# #84, Steam UI transport: steamwebhelper's JS connects to steam.exe's local
# websocket servers (ws://localhost:<port>/transportsocket/) every ~11s and
# never reaches the open state. The server sees each connection ([acc-ready]
# fires in step with the attempts) but [acc-take] never does -- and [acc-take]
# only covers plain accept(), not AcceptEx. Name, at the moment a connection is
# pending, who is waiting on the listener (AcceptEx requests, select/poll
# asyncs, WSAEventSelect mask/event, WSAAsyncSelect window), and stamp the
# AcceptEx completion path so the next device log says where the hand-off stops.
def listener_waiters(t):
    return replace_once(
        t,
        '                fprintf( stderr, "[acc-ready] lport=%u t=%llums rev=ml480\\n",\n'
        '                         (unsigned int)ntohs( sock->addr.in.sin_port ), ios_acc_now_ms() );',
        f'                {MARK}\n'
        '                fprintf( stderr, "[acc-ready] lport=%u t=%llums acceptex=%d poll=%d mask=%#x event=%d window=%d "\n'
        '                         "pending=%#x reported=%d deferred=%d rev=ml480+ci\\n",\n'
        '                         (unsigned int)ntohs( sock->addr.in.sin_port ), ios_acc_now_ms(),\n'
        '                         !list_empty( &sock->accept_list ), async_queued( &sock->poll_q ),\n'
        '                         sock->mask, sock->event != NULL, sock->window != 0,\n'
        '                         sock->pending_events, sock->reported_events != 0, sock->deferred != NULL );',
    )


def acceptex_completion(t):
    return replace_once(
        t,
        '    if (debug_level) fprintf( stderr, "completing accept request for socket %p\\n", sock );\n',
        '    if (debug_level) fprintf( stderr, "completing accept request for socket %p\\n", sock );\n'
        f'    {MARK}\n'
        '    {\n'
        '        static int acc_into_logged;\n'
        '        if (acc_into_logged < 48)\n'
        '        {\n'
        '            acc_into_logged++;\n'
        '            fprintf( stderr, "[acc-into] lport=%u t=%llums into_existing=%d owner=%u\\n",\n'
        '                     (unsigned int)ntohs( sock->addr.in.sin_port ), ios_acc_now_ms(),\n'
        '                     req->acceptsock != NULL,\n'
        '                     async_get_thread( req->async ) ? async_get_thread( req->async )->process->id : 0 );\n'
        '        }\n'
        '    }\n',
    )


patch("server/sock.c", listener_waiters)
patch("server/sock.c", acceptex_completion)
