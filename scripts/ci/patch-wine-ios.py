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


# #84 continued: with the probes above, a session showed steamwebhelper's
# FIRST transport attempt reaching steam.exe over IPv4 (accepted at once via
# AcceptEx), and every retry after it dialing only [::1] -- where Steam does
# not listen -- and being refused (11 of 11 per port). Chromium resolves
# "localhost" to both ::1 and 127.0.0.1; on a dual-stack host a [::1] dial to
# an IPv4-only listener is the failure we see. When a TCP connect targets
# [::1]:port and wineserver knows an IPv4 socket bound to that port but no
# IPv6 one, dial the IPv4-mapped address instead (clearing IPV6_V6ONLY on the
# client socket, which Wine sets to mirror Windows), so the retries reach the
# listener. A port that has an IPv6 listener is left alone.
def loopback6_to_v4(t):
    return replace_once(
        t,
        '        if (unix_addr.addr.sa_family == AF_INET && !memcmp( &unix_addr.in.sin_addr, magic_loopback_addr, 4 ))\n'
        '            unix_addr.in.sin_addr.s_addr = htonl( INADDR_LOOPBACK );\n'
        '\n'
        '        memcpy( &peer_addr, &unix_addr, sizeof(unix_addr) );\n',
        '        if (unix_addr.addr.sa_family == AF_INET && !memcmp( &unix_addr.in.sin_addr, magic_loopback_addr, 4 ))\n'
        '            unix_addr.in.sin_addr.s_addr = htonl( INADDR_LOOPBACK );\n'
        f'        {MARK}\n'
        '        if (unix_addr.addr.sa_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK( &unix_addr.in6.sin6_addr )\n'
        '            && sock->type == WS_SOCK_STREAM)\n'
        '        {\n'
        '            struct bound_addr probe;\n'
        '            int has_v6, has_v4;\n'
        '\n'
        '            memset( &probe, 0, sizeof(probe) );\n'
        '            probe.match_any_addr = 1;\n'
        '            probe.addr.in6.sin6_family = AF_INET6;\n'
        '            probe.addr.in6.sin6_port = unix_addr.in6.sin6_port;\n'
        '            has_v6 = rb_get( &bound_addresses_tree, &probe ) != NULL;\n'
        '            memset( &probe, 0, sizeof(probe) );\n'
        '            probe.match_any_addr = 1;\n'
        '            probe.addr.in.sin_family = AF_INET;\n'
        '            probe.addr.in.sin_port = unix_addr.in6.sin6_port;\n'
        '            has_v4 = rb_get( &bound_addresses_tree, &probe ) != NULL;\n'
        '            if (!has_v6 && has_v4)\n'
        '            {\n'
        '                static int loop6_logged;\n'
        '                int off = 0;\n'
        '\n'
        '                setsockopt( unix_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off) );\n'
        '                memset( &unix_addr.in6.sin6_addr, 0, sizeof(unix_addr.in6.sin6_addr) );\n'
        '                unix_addr.in6.sin6_addr.s6_addr[10] = 0xff;\n'
        '                unix_addr.in6.sin6_addr.s6_addr[11] = 0xff;\n'
        '                unix_addr.in6.sin6_addr.s6_addr[12] = 127;\n'
        '                unix_addr.in6.sin6_addr.s6_addr[15] = 1;\n'
        '                if (loop6_logged < 16)\n'
        '                {\n'
        '                    loop6_logged++;\n'
        '                    fprintf( stderr, "[loop6to4] [::1]:%u has only an IPv4 listener -- dialing ::ffff:127.0.0.1 instead\\n",\n'
        '                             (unsigned int)ntohs( unix_addr.in6.sin6_port ) );\n'
        '                }\n'
        '            }\n'
        '        }\n'
        '\n'
        '        memcpy( &peer_addr, &unix_addr, sizeof(unix_addr) );\n',
    )


patch("server/sock.c", listener_waiters)
patch("server/sock.c", acceptex_completion)
patch("server/sock.c", loopback6_to_v4)
