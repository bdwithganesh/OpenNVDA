// warp2 x y: move the pointer (points) in the logged-in session; run via launchctl asuser.
#include <ApplicationServices/ApplicationServices.h>
int main(int c, char **v) {
    if (c < 3) return 2;
    CGPoint p = {atof(v[1]), atof(v[2])};
    int r = CGWarpMouseCursorPosition(p);
    CGDisplayShowCursor(kCGDirectMainDisplay);
    usleep(100000);
    CGEventRef e = CGEventCreate(NULL);
    CGPoint q = CGEventGetLocation(e);
    printf("warp %d now %.0f %.0f\n", r, q.x, q.y);
    return 0;
}
