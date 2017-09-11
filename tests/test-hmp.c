/*
 * Test HMP commands.
 *
 * Copyright (c) 2017 Red Hat Inc.
 *
 * Author:
 *    Thomas Huth <thuth@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2
 * or later. See the COPYING file in the top-level directory.
 *
 * This test calls some HMP commands for all machines that the current
 * QEMU binary provides, to check whether they terminate successfully
 * (i.e. do not crash QEMU).
 */

#include "qemu/osdep.h"
#include "libqtest.h"

static int verbose;

static const char *hmp_cmds[] = {
    "boot_set ndc",
    "chardev-add null,id=testchardev1",
    "chardev-send-break testchardev1",
    "chardev-change testchardev1 ringbuf",
    "chardev-remove testchardev1",
    "commit all",
    "cpu-add 1",
    "cpu 0",
    "device_add ?",
    "device_add usb-mouse,id=mouse1",
    "mouse_button 7",
    "mouse_move 10 10",
    "mouse_button 0",
    "device_del mouse1",
    "dump-guest-memory /dev/null 0 4096",
    "gdbserver",
    "host_net_add user id=net0",
    "hostfwd_add tcp::43210-:43210",
    "hostfwd_remove tcp::43210-:43210",
    "host_net_remove 0 net0",
    "i /w 0",
    "log all",
    "log none",
    "memsave 0 4096 \"/dev/null\"",
    "migrate_set_cache_size 1",
    "migrate_set_downtime 1",
    "migrate_set_speed 1",
    "netdev_add user,id=net1",
    "set_link net1 off",
    "set_link net1 on",
    "netdev_del net1",
    "nmi",
    "o /w 0 0x1234",
    "object_add memory-backend-ram,id=mem1,size=256M",
    "object_del mem1",
    "pmemsave 0 4096 \"/dev/null\"",
    "p $pc + 8",
    "qom-list /",
    "qom-set /machine initrd test",
    "screendump /dev/null",
    "sendkey x",
    "singlestep on",
    "wavcapture /dev/null",
    "stopcapture 0",
    "sum 0 512",
    "x /8i 0x100",
    "xp /16x 0",
    NULL
};

/* Run through the list of pre-defined commands */
static void test_commands(void)
{
    char *response;
    int i;

    for (i = 0; hmp_cmds[i] != NULL; i++) {
        if (verbose) {
            fprintf(stderr, "\t%s\n", hmp_cmds[i]);
        }
        response = hmp(hmp_cmds[i]);
        g_free(response);
    }

}

/* Run through all info commands and call them blindly (without arguments) */
static void test_info_commands(void)
{
    char *resp, *info, *info_buf, *endp;

    info_buf = info = hmp("help info");

    while (*info) {
        /* Extract the info command, ignore parameters and description */
        g_assert(strncmp(info, "info ", 5) == 0);
        endp = strchr(&info[5], ' ');
        g_assert(endp != NULL);
        *endp = '\0';
        /* Now run the info command */
        if (verbose) {
            fprintf(stderr, "\t%s\n", info);
        }
        resp = hmp(info);
        g_free(resp);
        /* And move forward to the next line */
        info = strchr(endp + 1, '\n');
        if (!info) {
            break;
        }
        info += 1;
    }

    g_free(info_buf);
}

static void test_machine(gconstpointer data)
{
    const char *machine = data;

    global_qtest = qtest_startf("-S -M %s", machine);

    test_info_commands();
    test_commands();

    qtest_quit(global_qtest);
    g_free((void *)data);
}

static void add_machine_test_case(const char *mname)
{
    char *path;

    /* Ignore blacklisted machines that have known problems */
    if (!strcmp("puv3", mname) || !strcmp("tricore_testboard", mname) ||
        !strcmp("xenfv", mname) || !strcmp("xenpv", mname)) {
        return;
    }

    path = g_strdup_printf("hmp/%s", mname);
    qtest_add_data_func(path, g_strdup(mname), test_machine);
    g_free(path);
}

int main(int argc, char **argv)
{
    char *v_env = getenv("V");

    if (v_env && *v_env >= '2') {
        verbose = true;
    }

    g_test_init(&argc, &argv, NULL);

    qtest_cb_for_every_machine(add_machine_test_case);

    return g_test_run();
}
