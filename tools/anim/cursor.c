// Moves the cursor on a circle at ~120 Hz for N seconds (CGWarpMouseCursorPosition): WindowServer load with no app
// content involved. Run in the GUI session: launchctl asuser <uid> ./cursor 5
#include <ApplicationServices/ApplicationServices.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const double secs = argc > 1 ? atof(argv[1]) : 5;
    const int n = (int)(secs * 120);
    for (int i = 0; i < n; i++) {
        CGWarpMouseCursorPosition(CGPointMake(800 + 300 * cos(i / 40.0), 500 + 300 * sin(i / 40.0)));
        usleep(8333);
    }
    return 0;
}
