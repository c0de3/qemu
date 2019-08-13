# Functional test that boots a VM and run OCR on the framebuffer
#
# Copyright (c) Philippe Mathieu-Daudé <f4bug@amsat.org>
#
# This work is licensed under the terms of the GNU GPL, version 2 or
# later.  See the COPYING file in the top-level directory.

import os
import re
import time
import logging
import distutils.spawn

from avocado_qemu import Test
from avocado import skipUnless
from vncdotool import api as vnc
from avocado.utils import process
from avocado.utils.path import find_command, CmdNotFoundError

PIL_AVAILABLE = True
try:
    from PIL import Image
except ImportError:
    PIL_AVAILABLE = False


def tesseract_available(expected_version):
    try:
        find_command('tesseract')
    except CmdNotFoundError:
        return False
    res = process.run('tesseract --version')
    try:
        version = res.stdout_text.split()[1]
    except IndexError:
        version = res.stderr_text.split()[1]
    return int(version.split('.')[0]) == expected_version

    match = re.match(r'tesseract\s(\d)', res)
    if match is None:
        return False
    # now this is guaranteed to be a digit
    return int(match.groups()[0]) == expected_version


class NextCubeMachine(Test):

    timeout = 15

    def check_bootrom_framebuffer(self, screenshot_path):
        rom_url = ('http://www.nextcomputers.org/NeXTfiles/Software/ROM_Files/'
                   '68040_Non-Turbo_Chipset/Rev_2.5_v66.BIN')
        rom_hash = 'b3534796abae238a0111299fc406a9349f7fee24'
        rom_path = self.fetch_asset(rom_url, asset_hash=rom_hash)

        self.vm.set_machine('next-cube')
        self.vm.add_args('-bios', rom_path)
        self.vm.launch()

        self.log.info('VM launched, waiting for display')
        # TODO: Use avocado.utils.wait.wait_for to catch the
        #       'displaysurface_create 1120x832' trace-event.
        time.sleep(2)

        self.vm.command('human-monitor-command',
                        command_line='screendump %s' % screenshot_path)

    @skipUnless(PIL_AVAILABLE, 'Python PIL not installed')
    def test_bootrom_framebuffer_size(self):
        """
        :avocado: tags=arch:m68k
        :avocado: tags=machine:next_cube
        :avocado: tags=device:framebuffer
        """
        screenshot_path = os.path.join(self.workdir, "dump.png")
        self.check_bootrom_framebuffer(screenshot_path)

        width, height = Image.open(screenshot_path).size
        self.assertEqual(width, 1120)
        self.assertEqual(height, 832)

    @skipUnless(tesseract_available(3), 'tesseract v3 OCR tool not available')
    def test_bootrom_framebuffer_ocr_with_tesseract_v3(self):
        """
        :avocado: tags=arch:m68k
        :avocado: tags=machine:next_cube
        :avocado: tags=device:framebuffer
        """
        screenshot_path = os.path.join(self.workdir, "dump.png")
        self.check_bootrom_framebuffer(screenshot_path)

        console_logger = logging.getLogger('console')
        text = process.run("tesseract %s stdout" % screenshot_path).stdout_text
        for line in text.split('\n'):
            if len(line):
                console_logger.debug(line)
        self.assertIn('Backplane', text)
        self.assertIn('Ethernet address', text)

    # Tesseract 4 adds a new OCR engine based on LSTM neural networks. The
    # new version is faster and more accurate than version 3. The drawback is
    # that it is still alpha-level software.
    @skipUnless(tesseract_available(4), 'tesseract v4 OCR tool not available')
    def test_bootrom_framebuffer_ocr_with_tesseract_v4(self):
        """
        :avocado: tags=arch:m68k
        :avocado: tags=machine:next_cube
        :avocado: tags=device:framebuffer
        """
        screenshot_path = os.path.join(self.workdir, "dump.png")
        self.check_bootrom_framebuffer(screenshot_path)

        console_logger = logging.getLogger('console')
        proc = process.run("tesseract --oem 1 %s stdout" % screenshot_path)
        text = proc.stdout_text
        for line in text.split('\n'):
            if len(line):
                console_logger.debug(line)
        self.assertIn('Testing the FPU, SCC', text)
        self.assertIn('System test failed. Error code 51', text)
        self.assertIn('Boot command', text)
        self.assertIn('Next>', text)


    @staticmethod
    def vnc_send_string(client, string, eol=True):
        for key in string:
            client.keyPress(key)
            time.sleep(0.02)
        if eol:
            client.keyPress('enter')
            time.sleep(0.02)

    @skipUnless(tesseract_available(3), 'tesseract v3 OCR tool not available')
    def test_bootrom_via_vnc_with_tesseract_v3(self):
        """
        :avocado: tags=arch:m68k
        :avocado: tags=machine:next_cube
        :avocado: tags=device:framebuffer
        """
        screenshot_path = os.path.join(self.workdir, "dump.png")

        rom_url = ('http://www.nextcomputers.org/NeXTfiles/Software/ROM_Files/'
                   '68040_Non-Turbo_Chipset/Rev_2.5_v66.BIN')
        rom_hash = 'b3534796abae238a0111299fc406a9349f7fee24'
        rom_path = self.fetch_asset(rom_url, asset_hash=rom_hash)

        self.vm.set_machine('next-cube')
        self.vm.add_args('-bios', rom_path,
                         '-vnc', ':0') # XXX do not use static TCP port...
        self.vm.launch()

        self.log.info('VM launched, waiting for display')
        # TODO: Use avocado.utils.wait.wait_for to catch the
        #       'displaysurface_create 1120x832' trace-event.
        time.sleep(2)

        with vnc.connect('127.0.0.1') as client:
            self.vnc_send_string(client, 'help')
            self.vnc_send_string(client, 'mem')
            client.captureScreen(screenshot_path)
            client.disconnect()

        console_logger = logging.getLogger('console')
        text = process.run("tesseract %s stdout" % screenshot_path).stdout_text
        for line in text.split('\n'):
            if len(line):
                console_logger.debug(line)
        self.assertIn('address space', text)
        self.assertIn('System', text)
