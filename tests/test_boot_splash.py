from pathlib import Path
import subprocess
import tempfile
import unittest


class BootSplashTest(unittest.TestCase):
    def test_centred_splash_and_duration(self):
        source = (Path(__file__).parents[1] / 'src/main.cpp').read_text()
        start = source.index('void showBootSplash()')
        function = source[start:source.index('\nvoid ', start + 1)]
        with tempfile.TemporaryDirectory() as directory:
            test = Path(directory) / 'test.cpp'
            test.write_text('''#include <cassert>
#include <cstdint>
#include <cstring>
#define PROXY_RELEASE_VERSION "abcdef12+18"
bool oledReady = true;
uint8_t oledFrame[8][128];
unsigned waited = 0, pushed = 0;
void delay(unsigned ms) { waited += ms; }
void pushOled() { ++pushed; }
const uint8_t *oledGlyph(char) { static uint8_t glyph[5] = {127,127,127,127,127}; return glyph; }
''' + function + '''
int main() {
 showBootSplash();
 assert(waited == 3000 && pushed == 1);
 for (int x=0; x<128; ++x) assert(oledFrame[0][x] == 0 && oledFrame[7][x] == 0);
 assert(oledFrame[3][31] != 0 && oledFrame[3][30] == 0);
 oledReady = false;
 showBootSplash();
 assert(waited == 3000 && pushed == 1);
}
''')
            executable = Path(directory) / 'test'
            subprocess.run(['c++', '-std=c++17', str(test), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
