/* Linked in place of the madeira-d3d12 unix objects when the Metal Shader
 * Converter package is not available (build/madeira-d3d12/deps.sh). The app
 * and winemetal still reference these entry points; with the converter absent
 * every conversion reports MADEIRA_IR_NO_DYLIB and the in-app canary reports
 * one failed check, so native D3D12 is off while D3D11 (DXMT) is unaffected. */
#include <stdint.h>
#include <stdio.h>
#include "madeira_ir_abi.h"

static const char absent_msg[] =
    "madeira-d3d12: built without the Metal Shader Converter; native D3D12 is unavailable";

int madeira_ir_convert(void *args)
{
    struct madeira_ir_convert_args *a = args;
    if (a) {
        a->ret_len = 0;
        a->ret_status = MADEIRA_IR_NO_DYLIB;
        a->ret_error_code = 0;
    }
    return 0;
}

int madeira_d3d12_canary_run(const char *fixture_dir, const char *dylib_path,
                             void (*sink)(const char *))
{
    (void)fixture_dir; (void)dylib_path;
    if (sink) sink(absent_msg);
    fprintf(stderr, "%s\n", absent_msg);
    return 1;
}

int madeira_d3d12_canary_run_log(const char *fixture_dir, const char *dylib_path,
                                 void (*sink)(const char *), const char *log_path,
                                 const char *build_id)
{
    (void)build_id;
    if (log_path) {
        FILE *f = fopen(log_path, "w");
        if (f) { fprintf(f, "%s\n", absent_msg); fclose(f); }
    }
    return madeira_d3d12_canary_run(fixture_dir, dylib_path, sink);
}
