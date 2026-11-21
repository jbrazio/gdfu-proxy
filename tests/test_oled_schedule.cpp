#include <assert.h>

#include "../src/oled_schedule.h"

int main() {
    // Catches a regression that disables display updates for an entire DFU session.
    assert(!oledShouldUpdate(true, false, true, true, 1999));
    assert(oledShouldUpdate(true, false, true, true, 2000));

    assert(!oledShouldUpdate(true, false, false, true, 99));
    assert(oledShouldUpdate(true, false, false, true, 100));
    assert(!oledShouldUpdate(true, false, true, false, 4000));
    assert(oledShouldUpdate(true, true, true, false, 0));
    assert(!oledShouldUpdate(false, true, true, true, 4000));
}
