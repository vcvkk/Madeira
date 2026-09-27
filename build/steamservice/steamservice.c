/* steamservice-x64.exe -- a 64-bit stand-in for Steam's install-script runner.
 *
 * On a game's first launch Steam evaluates the game's install script and
 * hands the parts that need admin rights to
 *     bin\SteamService.exe /installscript "<dir>\runasadmin.vdf" <appid>
 * SteamService.exe is 32-bit x86, and a 32-bit process cannot run under this
 * port (WoW64 needs memory below 2GB, which the iOS 4GB page zero takes), so
 * NtCreateUserProcess redirects that one command line here (process_ios.c).
 *
 * What the script asks for, and what this does with it:
 *   "Registry"                 -- written, the way SteamService writes it
 *   "Registry If Not Present"  -- written only where the value is missing
 *   "Run Process"              -- the VC++/DirectX/.NET/PhysX redistributables.
 *                                 Not run: those libraries are builtin here (and
 *                                 DXMT stands in for DirectX), and their
 *                                 installers are 32-bit anyway. Each entry's
 *                                 HasRunKey is marked done so Steam does not ask
 *                                 again on every launch.
 *   "Firewall", "Run Process On Uninstall" -- nothing to do here.
 *
 * The semantics follow Valve's SteamService.dll: %INSTALLDIR% is the folder
 * holding the script; HKEY_LOCAL_MACHINE is the 32-bit registry view (the real
 * service is a 32-bit process); HasRunKey defaults to
 * HKEY_LOCAL_MACHINE\Software\Valve\Steam\Apps\<appid>, its value name to
 * RunKeyName or else the entry's name, and the value written is
 * MinimumHasRunValue (default 1).
 *
 * Everything is logged to <Steam>\logs\madeira_installscript.log. The exit code
 * is always 0: a script this cannot read is logged, but must not stop the game
 * from starting. */
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct kv
{
    WCHAR *name;
    WCHAR *value;           /* NULL for a section */
    struct kv *child;       /* first child of a section */
    struct kv *next;
} kv;

static HANDLE log_file = INVALID_HANDLE_VALUE;
static WCHAR install_dir[MAX_PATH];
static WCHAR language[64] = L"english";
static unsigned int app_id;
static int errors;

static void logf( const WCHAR *fmt, ... )
{
    WCHAR buf[2048];
    char out[4096];
    va_list ap;
    int n;
    DWORD written;

    va_start( ap, fmt );
    _vsnwprintf( buf, ARRAYSIZE(buf) - 2, fmt, ap );
    va_end( ap );
    buf[ARRAYSIZE(buf) - 2] = 0;
    wcscat( buf, L"\n" );
    n = WideCharToMultiByte( CP_UTF8, 0, buf, -1, out, sizeof(out), NULL, NULL );
    if (n > 1)
    {
        if (log_file != INVALID_HANDLE_VALUE) WriteFile( log_file, out, n - 1, &written, NULL );
        fputs( out, stderr );
    }
}

/* ---- VDF (KeyValues text) parser ------------------------------------------ */

static const char *src, *src_end;

static WCHAR *to_wide( const char *s, int len )
{
    int n = MultiByteToWideChar( CP_UTF8, 0, s, len, NULL, 0 );
    WCHAR *w = malloc( (n + 1) * sizeof(WCHAR) );
    MultiByteToWideChar( CP_UTF8, 0, s, len, w, n );
    w[n] = 0;
    return w;
}

static void skip_space( void )
{
    for (;;)
    {
        while (src < src_end && (*src == ' ' || *src == '\t' || *src == '\r' || *src == '\n')) src++;
        if (src + 1 < src_end && src[0] == '/' && src[1] == '/')
        {
            while (src < src_end && *src != '\n') src++;
            continue;
        }
        break;
    }
}

enum { TOK_END, TOK_STR, TOK_OPEN, TOK_CLOSE };

/* Reads one token. A string token (quoted or bare) is returned in *out. */
static int next_token( WCHAR **out )
{
    char *buf;
    int len = 0;

    skip_space();
    if (src >= src_end) return TOK_END;
    if (*src == '{') { src++; return TOK_OPEN; }
    if (*src == '}') { src++; return TOK_CLOSE; }

    buf = malloc( src_end - src + 1 );
    if (*src == '"')
    {
        src++;
        while (src < src_end && *src != '"')
        {
            if (*src == '\\' && src + 1 < src_end)
            {
                src++;
                switch (*src)
                {
                case 'n': buf[len++] = '\n'; break;
                case 't': buf[len++] = '\t'; break;
                case '\\': buf[len++] = '\\'; break;
                case '"': buf[len++] = '"'; break;
                default: buf[len++] = '\\'; buf[len++] = *src; break;
                }
                src++;
            }
            else buf[len++] = *src++;
        }
        if (src < src_end) src++;
    }
    else
    {
        while (src < src_end && !strchr( " \t\r\n{}\"", *src )) buf[len++] = *src++;
    }
    *out = to_wide( buf, len );
    free( buf );
    return TOK_STR;
}

/* Skips a platform conditional such as [$WIN32] after a key or value. */
static void skip_conditional( void )
{
    const char *save = src;
    skip_space();
    if (src < src_end && *src == '[')
    {
        while (src < src_end && *src != ']' && *src != '\n') src++;
        if (src < src_end && *src == ']') { src++; return; }
    }
    src = save;
}

/* Parses "name value" / "name { ... }" pairs until '}' or end of input. */
static kv *parse_list( int depth, int *ok )
{
    kv *head = NULL, **tail = &head;

    for (;;)
    {
        WCHAR *name, *value;
        kv *node;
        int t = next_token( &name );

        if (t == TOK_END) { if (depth) *ok = 0; return head; }
        if (t == TOK_CLOSE) { if (!depth) *ok = 0; return head; }
        if (t == TOK_OPEN) { *ok = 0; return head; }
        skip_conditional();

        node = calloc( 1, sizeof(*node) );
        node->name = name;
        t = next_token( &value );
        if (t == TOK_STR)
        {
            node->value = value;
            skip_conditional();
        }
        else if (t == TOK_OPEN)
        {
            node->child = parse_list( depth + 1, ok );
            if (!*ok) return head;
        }
        else { *ok = 0; free( node ); return head; }
        *tail = node;
        tail = &node->next;
    }
}

static const WCHAR *kv_str( const kv *sec, const WCHAR *name )
{
    for (sec = sec->child; sec; sec = sec->next)
        if (sec->value && !_wcsicmp( sec->name, name )) return sec->value;
    return NULL;
}

/* ---- variables ------------------------------------------------------------ */

static int folder( int csidl, WCHAR *out )
{
    return SUCCEEDED( SHGetFolderPathW( NULL, csidl | CSIDL_FLAG_CREATE, NULL, 0, out ) );
}

static int lookup_var( const WCHAR *name, WCHAR *out )
{
    if (!_wcsicmp( name, L"INSTALLDIR" )) { wcscpy( out, install_dir ); return 1; }
    if (!_wcsicmp( name, L"ROOTDRIVE" ))
    {
        GetWindowsDirectoryW( out, MAX_PATH );
        out[2] = 0;
        return 1;
    }
    if (!_wcsicmp( name, L"WinDir" )) return GetWindowsDirectoryW( out, MAX_PATH ) != 0;
    if (!_wcsicmp( name, L"USER_MYDOCS" )) return folder( CSIDL_PERSONAL, out );
    if (!_wcsicmp( name, L"COMMON_MYDOCS" )) return folder( CSIDL_COMMON_DOCUMENTS, out );
    if (!_wcsicmp( name, L"USER_APPDATA" ) || !_wcsicmp( name, L"APPDATA" )) return folder( CSIDL_APPDATA, out );
    if (!_wcsicmp( name, L"LOCAL_APPDATA" ) || !_wcsicmp( name, L"USER_LOCALAPPDATA" ))
        return folder( CSIDL_LOCAL_APPDATA, out );
    if (!_wcsicmp( name, L"COMMON_APPDATA" )) return folder( CSIDL_COMMON_APPDATA, out );
    if (!_wcsicmp( name, L"APPID" )) { swprintf( out, 16, L"%u", app_id ); return 1; }
    {
        DWORD n = GetEnvironmentVariableW( name, out, MAX_PATH );
        return n && n < MAX_PATH;
    }
}

/* Returns a malloc'd copy of s with every known %VAR% replaced. */
static WCHAR *expand( const WCHAR *s )
{
    size_t cap = wcslen( s ) + 1, len = 0;
    WCHAR *out = malloc( cap * sizeof(WCHAR) );

    while (*s)
    {
        const WCHAR *end;
        WCHAR name[64], val[MAX_PATH];
        const WCHAR *piece = s;
        size_t plen = 1;

        if (*s == '%' && (end = wcschr( s + 1, '%' )) && end - s - 1 > 0 && end - s - 1 < 64)
        {
            memcpy( name, s + 1, (end - s - 1) * sizeof(WCHAR) );
            name[end - s - 1] = 0;
            if (lookup_var( name, val ))
            {
                piece = val;
                plen = wcslen( val );
                s = end + 1;
            }
            else s++;
        }
        else s++;

        if (len + plen + 1 > cap)
        {
            cap = (len + plen + 1) * 2;
            out = realloc( out, cap * sizeof(WCHAR) );
        }
        memcpy( out + len, piece, plen * sizeof(WCHAR) );
        len += plen;
    }
    out[len] = 0;
    return out;
}

/* ---- registry --------------------------------------------------------------- */

/* Splits "HKEY_LOCAL_MACHINE\Sub\Key" into a root and a subkey, remapping the
 * root the way SteamService's BExtractRegPartsAndRemapRoot does. */
static int split_key( const WCHAR *path, HKEY *root, REGSAM *view, const WCHAR **sub )
{
    static const struct { const WCHAR *name; HKEY root; REGSAM view; } roots[] =
    {
        { L"HKEY_LOCAL_MACHINE_WOW64_32", HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
        { L"HKEY_LOCAL_MACHINE_WOW64_64", HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY },
        { L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
        { L"HKLM", HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
        { L"HKEY_CURRENT_USER", HKEY_CURRENT_USER, 0 },
        { L"HKCU", HKEY_CURRENT_USER, 0 },
        { L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT, 0 },
        { L"HKCR", HKEY_CLASSES_ROOT, 0 },
        { L"HKEY_USERS", HKEY_USERS, 0 },
    };
    unsigned i;

    for (i = 0; i < ARRAYSIZE(roots); i++)
    {
        size_t n = wcslen( roots[i].name );
        if (!_wcsnicmp( path, roots[i].name, n ) && (path[n] == '\\' || !path[n]))
        {
            *root = roots[i].root;
            *view = roots[i].view;
            *sub = path[n] ? path + n + 1 : path + n;
            return 1;
        }
    }
    return 0;
}

static HKEY create_key( const WCHAR *path )
{
    HKEY root, key;
    REGSAM view;
    const WCHAR *sub;
    LSTATUS st;

    if (!split_key( path, &root, &view, &sub ))
    {
        logf( L"  ! unknown registry root in \"%ls\"", path );
        errors++;
        return NULL;
    }
    st = RegCreateKeyExW( root, sub, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE | view, NULL, &key, NULL );
    if (st)
    {
        logf( L"  ! cannot create \"%ls\" (error %ld)", path, (long)st );
        errors++;
        return NULL;
    }
    return key;
}

static int hexval( WCHAR c )
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void set_value( HKEY key, const WCHAR *path, const WCHAR *type, const WCHAR *name,
                       const WCHAR *raw, int if_not_present )
{
    const WCHAR *vname = (!*name || !_wcsicmp( name, L"(Default)" )) ? NULL : name;
    WCHAR *val = expand( raw );
    LSTATUS st;

    if (if_not_present && !RegQueryValueExW( key, vname, NULL, NULL, NULL, NULL ))
    {
        logf( L"  = %ls\\%ls already set, left alone", path, name );
        free( val );
        return;
    }

    if (!_wcsicmp( type, L"dword" ))
    {
        DWORD d = (DWORD)wcstoul( val, NULL, 0 );
        st = RegSetValueExW( key, vname, 0, REG_DWORD, (BYTE *)&d, sizeof(d) );
    }
    else if (!_wcsicmp( type, L"qword" ))
    {
        ULONGLONG q = _wcstoui64( val, NULL, 0 );
        st = RegSetValueExW( key, vname, 0, REG_QWORD, (BYTE *)&q, sizeof(q) );
    }
    else if (!_wcsicmp( type, L"binary" ))
    {
        BYTE *bin = malloc( wcslen( val ) / 2 + 1 );
        DWORD n = 0;
        const WCHAR *p = val;

        while (*p)
        {
            int hi, lo;
            while (*p && hexval( *p ) < 0) p++;
            if (!*p || (hi = hexval( p[0] )) < 0 || (lo = hexval( p[1] )) < 0) break;
            bin[n++] = (BYTE)(hi << 4 | lo);
            p += 2;
        }
        st = RegSetValueExW( key, vname, 0, REG_BINARY, bin, n );
        free( bin );
    }
    else if (!_wcsicmp( type, L"string" ))
        st = RegSetValueExW( key, vname, 0, REG_SZ, (BYTE *)val, (DWORD)((wcslen( val ) + 1) * sizeof(WCHAR)) );
    else
    {
        logf( L"  ! %ls\\%ls: unknown value type \"%ls\", skipped", path, name, type );
        free( val );
        return;
    }

    if (st) { logf( L"  ! %ls\\%ls: error %ld", path, name, (long)st ); errors++; }
    else logf( L"  + %ls\\%ls = (%ls) %ls", path, *name ? name : L"(Default)", type, val );
    free( val );
}

/* "Registry" { "<key>" { "<type>" { "<language>" { "<name>" "<value>" } } } } */
static void do_registry( const kv *sec, int if_not_present )
{
    const kv *k, *t, *l, *v;

    logf( L"[%ls]", sec->name );
    for (k = sec->child; k; k = k->next)
    {
        WCHAR *path;
        HKEY key;

        if (k->value) continue;
        path = expand( k->name );
        if (!(key = create_key( path ))) { free( path ); continue; }
        for (t = k->child; t; t = t->next)
        {
            if (t->value) continue;
            for (l = t->child; l; l = l->next)
            {
                if (l->value)   /* no language level */
                {
                    set_value( key, path, t->name, l->name, l->value, if_not_present );
                    continue;
                }
                if (_wcsicmp( l->name, L"any" ) && _wcsicmp( l->name, language )) continue;
                for (v = l->child; v; v = v->next)
                    if (v->value) set_value( key, path, t->name, v->name, v->value, if_not_present );
            }
        }
        RegCloseKey( key );
        free( path );
    }
}

/* "Run Process" { "<entry>" { "HasRunKey" ... "process 1" ... "command 1" ... } } */
static void do_run_process( const kv *sec )
{
    const kv *e, *f;

    logf( L"[%ls]", sec->name );
    for (e = sec->child; e; e = e->next)
    {
        const WCHAR *has_run = kv_str( e, L"HasRunKey" );
        const WCHAR *run_name = kv_str( e, L"RunKeyName" );
        const WCHAR *minimum = kv_str( e, L"MinimumHasRunValue" );
        WCHAR def_key[128], *path;
        DWORD done;
        HKEY key;
        LSTATUS st;

        if (e->value) continue;
        for (f = e->child; f; f = f->next)
            if (f->value && !_wcsnicmp( f->name, L"process", 7 ))
            {
                WCHAR *p = expand( f->value );
                logf( L"  - %ls: not running %ls (installers are skipped here)", e->name, p );
                free( p );
            }

        if (!has_run)
        {
            if (!app_id) continue;
            swprintf( def_key, ARRAYSIZE(def_key), L"HKEY_LOCAL_MACHINE\\Software\\Valve\\Steam\\Apps\\%u", app_id );
            has_run = def_key;
        }
        if (!run_name) run_name = e->name;
        done = minimum ? (DWORD)wcstoul( minimum, NULL, 0 ) : 1;
        if (!done) done = 1;

        path = expand( has_run );
        if ((key = create_key( path )))
        {
            st = RegSetValueExW( key, run_name, 0, REG_DWORD, (BYTE *)&done, sizeof(done) );
            if (st) { logf( L"  ! %ls\\%ls: error %ld", path, run_name, (long)st ); errors++; }
            else logf( L"  + %ls\\%ls = (dword) %lu  [marked as run]", path, run_name, done );
            RegCloseKey( key );
        }
        free( path );
    }
}

static void walk( const kv *list )
{
    const kv *n;

    for (n = list; n; n = n->next)
    {
        if (n->value) continue;
        if (!_wcsicmp( n->name, L"Registry" )) do_registry( n, 0 );
        else if (!_wcsicmp( n->name, L"Registry If Not Present" )) do_registry( n, 1 );
        else if (!_wcsicmp( n->name, L"Run Process" )) do_run_process( n );
        else if (!_wcsicmp( n->name, L"Run Process On Uninstall" ) || !_wcsicmp( n->name, L"Firewall" )
                 || !_wcsicmp( n->name, L"Environment" ))
            logf( L"[%ls] skipped", n->name );
        else walk( n->child );   /* "InstallScript" and any other wrapper */
    }
}

/* ---- main ------------------------------------------------------------------- */

static void open_log( const WCHAR *script )
{
    WCHAR path[MAX_PATH * 2];
    const WCHAR *p = NULL, *q;

    /* <Steam>\steamapps\...  ->  <Steam>\logs */
    for (q = script; *q; q++)
        if (!_wcsnicmp( q, L"\\steamapps\\", 11 )) p = q;
    if (p)
    {
        swprintf( path, ARRAYSIZE(path), L"%.*ls\\logs", (int)(p - script), script );
        CreateDirectoryW( path, NULL );
        wcscat( path, L"\\madeira_installscript.log" );
    }
    else swprintf( path, ARRAYSIZE(path), L"%ls\\madeira_installscript.log", install_dir );

    log_file = CreateFileW( path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
}

static void read_language( void )
{
    WCHAR buf[64];
    DWORD size = sizeof(buf), type;
    HKEY key;

    if (RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &key )) return;
    if (!RegQueryValueExW( key, L"Language", NULL, &type, (BYTE *)buf, &size ) && type == REG_SZ && buf[0])
    {
        buf[ARRAYSIZE(buf) - 1] = 0;
        wcscpy( language, buf );
    }
    RegCloseKey( key );
}

static int run_script( const WCHAR *script )
{
    HANDLE f;
    DWORD size, got;
    char *data;
    kv *root;
    int ok = 1;
    WCHAR *slash;
    SYSTEMTIME now;

    wcsncpy( install_dir, script, MAX_PATH - 1 );
    if ((slash = wcsrchr( install_dir, '\\' ))) *slash = 0;
    open_log( script );
    read_language();
    GetLocalTime( &now );
    logf( L"==== %04u-%02u-%02u %02u:%02u:%02u  /installscript \"%ls\" %u  (language %ls)",
          now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, script, app_id, language );

    f = CreateFileW( script, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
    if (f == INVALID_HANDLE_VALUE)
    {
        logf( L"! cannot open the script (error %lu)", GetLastError() );
        return 0;
    }
    size = GetFileSize( f, NULL );
    data = malloc( size + 1 );
    if (!ReadFile( f, data, size, &got, NULL )) got = 0;
    CloseHandle( f );

    src = data;
    src_end = data + got;
    if (got >= 3 && !memcmp( data, "\xef\xbb\xbf", 3 )) src += 3;
    root = parse_list( 0, &ok );
    if (!ok)
    {
        logf( L"! the script is not valid VDF (stopped at byte %ld)", (long)(src - data) );
        return 0;
    }
    walk( root );
    logf( L"==== done, %d error(s)", errors );
    return 0;
}

int wmain( void )
{
    int argc, i;
    WCHAR **argv = CommandLineToArgvW( GetCommandLineW(), &argc );

    for (i = 1; argv && i < argc; i++)
    {
        if (!_wcsicmp( argv[i], L"/installscript" ) && i + 1 < argc)
        {
            if (i + 2 < argc) app_id = (unsigned int)wcstoul( argv[i + 2], NULL, 10 );
            return run_script( argv[i + 1] );
        }
    }
    /* Any other SteamService verb (/uninstallscript, /repair, ...): nothing to
     * do in this port, and Steam only needs it to have succeeded. */
    fwprintf( stderr, L"steamservice-x64: nothing to do for %ls\n", GetCommandLineW() );
    return 0;
}
