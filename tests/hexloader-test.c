/*
 * QTest testcase for the Intel Hexadecimal Object File Loader
 *
 * Authors:
 *  Su Hang <suhang16@mails.ucas.ac.cn> 2018
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "libqtest.h"

/* success if no crash or abort */
static void hex_loader_test(void)
{
    unsigned int i;
    const unsigned int base_addr = 0x00010000;

    QTestState *s = qtest_startf(
        "-M vexpress-a9 -nographic -device loader,file=tests/hex-loader-check-data/test.hex");

    for (i = 0; i < 256; ++i) {
        uint8_t val = qtest_readb(s, base_addr + i);
        g_assert_cmpuint(i, ==, val);
    }
    qtest_quit(s);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/tmp/hex_loader", hex_loader_test);
    ret = g_test_run();

    return ret;
}
