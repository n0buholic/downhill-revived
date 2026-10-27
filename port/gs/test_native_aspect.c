/* CPU-only viewport regression. Link with function sections and --gc-sections so no GPU is needed. */
#include "gpu_rust.c"
#include <assert.h>

const char *dhdr_version(void) { return "viewport test"; }

int main(void)
{
    static Native n;
    float w, h;
    n.wide = 1;
    n.display_width = 512;
    n.display_height = 448;
    n.aspect = 16.0f / 9;
    native_viewport(&n, 1920, 1080, &w, &h);
    assert(w == 1920 && h == 1080);
    n.aspect = 4.0f / 3;
    native_viewport(&n, 1920, 1080, &w, &h);
    assert(w == 1440 && h == 1080);
    n.aspect = 0; /* stretch */
    native_viewport(&n, 1920, 1080, &w, &h);
    assert(w == 1920 && h == 1080);
    n.aspect = -1; /* raw pixel aspect */
    native_viewport(&n, 1920, 1080, &w, &h);
    assert(w == 1234 && h == 1080);
    puts("PASS: wide, 4:3, stretch and raw viewport");
    return 0;
}
