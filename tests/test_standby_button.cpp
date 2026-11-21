#include <assert.h>

#include "../src/standby_button.h"

int main() {
    StandbyButton button(3000);

    assert(!button.update(false, 0));
    assert(!button.update(true, 0));
    assert(!button.update(true, 2999));
    assert(button.update(true, 3000));

    assert(!button.update(false, 3001));
    assert(!button.update(true, 3002));
    assert(!button.update(true, 6001));
    assert(button.update(true, 6002));
}
