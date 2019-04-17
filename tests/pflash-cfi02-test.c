/*
 * QTest testcase for parallel flash with AMD command set
 *
 * Copyright (c) 2018 Stephen Checkoway
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include <err.h>
#include <unistd.h>
#include "libqtest.h"

/*
 * To test the pflash_cfi02 device, we run QEMU with the musicpal machine with
 * a pflash drive. This enables us to test some flash configurations, but not
 * all. In particular, we're limited to a 16-bit wide flash device.
 */

#define MP_FLASH_SIZE_MAX (32 * 1024 * 1024)
#define FLASH_SIZE (8 * 1024 * 1024)
#define BASE_ADDR (0x100000000ULL - MP_FLASH_SIZE_MAX)

/* Use a newtype to keep flash addresses separate from byte addresses. */
typedef struct {
    uint64_t addr;
} faddr;
#define FLASH_ADDR(x) ((faddr) { .addr = (x) })

#define CFI_ADDR FLASH_ADDR(0x55)
#define UNLOCK0_ADDR FLASH_ADDR(0x555)
#define UNLOCK1_ADDR FLASH_ADDR(0x2AA)

#define CFI_CMD 0x98
#define UNLOCK0_CMD 0xAA
#define UNLOCK1_CMD 0x55
#define AUTOSELECT_CMD 0x90
#define RESET_CMD 0xF0
#define PROGRAM_CMD 0xA0
#define SECTOR_ERASE_CMD 0x30
#define CHIP_ERASE_CMD 0x10
#define UNLOCK_BYPASS_CMD 0x20
#define UNLOCK_BYPASS_RESET_CMD 0x00

typedef struct {
    int bank_width;
    int device_width;
    int max_device_width;

    QTestState *qtest;
} FlashConfig;

static char image_path[] = "/tmp/qtest.XXXXXX";

/*
 * The pflash implementation allows some parameters to be unspecified. We want
 * to test those configurations but we also need to know the real values in
 * our testing code. So after we launch qemu, we'll need a new FlashConfig
 * with the correct values filled in.
 */
static FlashConfig expand_config_defaults(const FlashConfig *c)
{
    FlashConfig ret = *c;

    if (ret.device_width == 0) {
        ret.device_width = ret.bank_width;
    }
    if (ret.max_device_width == 0) {
        ret.max_device_width = ret.device_width;
    }
    return ret;
}

/*
 * Return a bit mask suitable for extracting the least significant
 * status/query response from an interleaved response.
 */
static inline uint64_t device_mask(const FlashConfig *c)
{
    if (c->device_width == 8) {
        return (uint64_t)-1;
    }
    return (1ULL << (c->device_width * 8)) - 1ULL;
}

/*
 * Return a bit mask exactly as long as the bank_width.
 */
static inline uint64_t bank_mask(const FlashConfig *c)
{
    if (c->bank_width == 8) {
        return (uint64_t)-1;
    }
    return (1ULL << (c->bank_width * 8)) - 1ULL;
}

static inline void flash_write(const FlashConfig *c, uint64_t byte_addr,
                               uint64_t data)
{
    /* Sanity check our tests. */
    assert((data & ~bank_mask(c)) == 0);
    uint64_t addr = BASE_ADDR + byte_addr;
    switch (c->bank_width) {
    case 1:
        qtest_writeb(c->qtest, addr, data);
        break;
    case 2:
        qtest_writew(c->qtest, addr, data);
        break;
    case 4:
        qtest_writel(c->qtest, addr, data);
        break;
    case 8:
        qtest_writeq(c->qtest, addr, data);
        break;
    default:
        abort();
    }
}

static inline uint64_t flash_read(const FlashConfig *c, uint64_t byte_addr)
{
    uint64_t addr = BASE_ADDR + byte_addr;
    switch (c->bank_width) {
    case 1:
        return qtest_readb(c->qtest, addr);
    case 2:
        return qtest_readw(c->qtest, addr);
    case 4:
        return qtest_readl(c->qtest, addr);
    case 8:
        return qtest_readq(c->qtest, addr);
    default:
        abort();
    }
}

/*
 * Convert a flash address expressed in the maximum width of the device as a
 * byte address.
 */
static inline uint64_t as_byte_addr(const FlashConfig *c, faddr flash_addr)
{
    /*
     * Command addresses are always given as addresses in the maximum
     * supported bus size for the flash chip. So an x8/x16 chip in x8 mode
     * uses addresses 0xAAA and 0x555 to unlock because the least significant
     * bit is ignored. (0x555 rather than 0x554 is traditional.)
     *
     * Interleaving flash chips use the least significant bits of a byte
     * address to refer to data from the individual chips. Two interleaved x8
     * devices would use command addresses 0xAAA and 0x554. Two interleaved
     * x16 devices would use 0x1554 and 0xAA8.
     *
     * More exotic configurations are possible. Two interleaved x8/x16 devices
     * in x8 mode would also use 0x1554 and 0xAA8.
     *
     * In general we need to multiply an address by the number of devices,
     * which is bank_width / device_width, and multiply that by the maximum
     * device width.
     */
    int num_devices = c->bank_width / c->device_width;
    return flash_addr.addr * (num_devices * c->max_device_width);
}

/*
 * Return the command value or expected status replicated across all devices.
 */
static inline uint64_t replicate(const FlashConfig *c, uint64_t data)
{
    /* Sanity check our tests. */
    assert((data & ~device_mask(c)) == 0);
    for (int i = c->device_width; i < c->bank_width; i += c->device_width) {
        data |= data << (c->device_width * 8);
    }
    return data;
}

static inline void flash_cmd(const FlashConfig *c, faddr cmd_addr,
                             uint8_t cmd)
{
    flash_write(c, as_byte_addr(c, cmd_addr), replicate(c, cmd));
}

static inline uint64_t flash_query(const FlashConfig *c, faddr query_addr)
{
    return flash_read(c, as_byte_addr(c, query_addr));
}

static inline uint64_t flash_query_1(const FlashConfig *c, faddr query_addr)
{
    return flash_query(c, query_addr) & device_mask(c);
}

static void unlock(const FlashConfig *c)
{
    flash_cmd(c, UNLOCK0_ADDR, UNLOCK0_CMD);
    flash_cmd(c, UNLOCK1_ADDR, UNLOCK1_CMD);
}

static void reset(const FlashConfig *c)
{
    flash_cmd(c, FLASH_ADDR(0), RESET_CMD);
}

static void sector_erase(const FlashConfig *c, uint64_t byte_addr)
{
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, 0x80);
    unlock(c);
    flash_write(c, byte_addr, replicate(c, SECTOR_ERASE_CMD));
}

static void wait_for_completion(const FlashConfig *c, uint64_t byte_addr)
{
    /* If DQ6 is toggling, step the clock and ensure the toggle stops. */
    const uint64_t dq6 = replicate(c, 0x40);
    if ((flash_read(c, byte_addr) & dq6) ^ (flash_read(c, byte_addr) & dq6)) {
        /* Wait for erase or program to finish. */
        qtest_clock_step_next(c->qtest);
        /* Ensure that DQ6 has stopped toggling. */
        g_assert_cmpint(flash_read(c, byte_addr), ==, flash_read(c, byte_addr));
    }
}

static void bypass_program(const FlashConfig *c, uint64_t byte_addr,
                           uint16_t data)
{
    flash_cmd(c, UNLOCK0_ADDR, PROGRAM_CMD);
    flash_write(c, byte_addr, data);
    /*
     * Data isn't valid until DQ6 stops toggling. We don't model this as
     * writes are immediate, but if this changes in the future, we can wait
     * until the program is complete.
     */
    wait_for_completion(c, byte_addr);
}

static void program(const FlashConfig *c, uint64_t byte_addr, uint16_t data)
{
    unlock(c);
    bypass_program(c, byte_addr, data);
}

static void chip_erase(const FlashConfig *c)
{
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, 0x80);
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, CHIP_ERASE_CMD);
}

/*
 * Check that the device interface code dic is appropriate for the given
 * width.
 *
 * Device interface codes are specified in JEP173.
 */
static bool device_supports_width(uint16_t dic, int width)
{
    switch (width) {
    case 1:
        /*
         * x8-only, x8/x16, or x32
         * XXX: Currently we use dic = 3 for an x8/x32 device even though
         * that's only for x32. If there's a more appropriate value, both this
         * test and pflash-cfi02.c should be modified.
         */
        return dic == 0 || dic == 2 || dic == 3;
    case 2:
        /* x16-only, x8/x16, or x16/x32. */
        return dic == 1 || dic == 2 || dic == 4;
    case 4:
        /* x32-only or x16/x32. */
        return dic == 3 || dic == 4;
    }
    g_test_incomplete("Device width test not supported");
    return true;
}

static void test_flash(const void *opaque)
{
    const FlashConfig *config = opaque;
    QTestState *qtest = qtest_initf("-M musicpal,accel=qtest"
                                    " -drive if=pflash,file=%s,format=raw,"
                                    "copy-on-read"
                                    " -global driver=cfi.pflash02,"
                                    "property=device-width,value=%d"
                                    " -global driver=cfi.pflash02,"
                                    "property=max-device-width,value=%d",
                                    image_path,
                                    config->device_width,
                                    config->max_device_width);

    FlashConfig explicit_config = expand_config_defaults(config);
    explicit_config.qtest = qtest;
    const FlashConfig *c = &explicit_config;

    /* Check the IDs. */
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, AUTOSELECT_CMD);
    g_assert_cmpint(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, 0xBF));
    if (c->device_width >= 2) {
        /*
         * XXX: The ID returned by the musicpal flash chip is 16 bits which
         * wouldn't happen with an 8-bit device. It would probably be best to
         * prohibit addresses larger than the device width in pflash_cfi02.c,
         * but then we couldn't test smaller device widths at all.
         */
        g_assert_cmpint(flash_query(c, FLASH_ADDR(1)), ==,
                        replicate(c, 0x236D));
    }
    reset(c);

    /* Check the erase blocks. */
    flash_cmd(c, CFI_ADDR, CFI_CMD);
    g_assert_cmpint(flash_query(c, FLASH_ADDR(0x10)), ==, replicate(c, 'Q'));
    g_assert_cmpint(flash_query(c, FLASH_ADDR(0x11)), ==, replicate(c, 'R'));
    g_assert_cmpint(flash_query(c, FLASH_ADDR(0x12)), ==, replicate(c, 'Y'));

    /* Num erase regions. */
    g_assert_cmpint(flash_query_1(c, FLASH_ADDR(0x2C)), >=, 1);

    /* Check device length. */
    uint32_t device_len = 1 << flash_query_1(c, FLASH_ADDR(0x27));
    g_assert_cmpint(device_len * (c->bank_width / c->device_width), ==,
                    FLASH_SIZE);

    /* Check nb_sectors * sector_len is device_len. */
    uint32_t nb_sectors = flash_query_1(c, FLASH_ADDR(0x2D)) +
                          (flash_query_1(c, FLASH_ADDR(0x2E)) << 8) + 1;
    uint32_t sector_len = (flash_query_1(c, FLASH_ADDR(0x2F)) << 8) +
                          (flash_query_1(c, FLASH_ADDR(0x30)) << 16);
    g_assert_cmpint(nb_sectors * sector_len, ==, device_len);

    /* Check the device interface code supports the width and max width. */
    uint16_t device_interface_code = flash_query_1(c, FLASH_ADDR(0x28)) +
                                     (flash_query_1(c, FLASH_ADDR(0x29)) << 8);
    g_assert_true(device_supports_width(device_interface_code,
                                        c->device_width));
    g_assert_true(device_supports_width(device_interface_code,
                                        c->max_device_width));
    reset(c);

    const uint64_t dq7 = replicate(c, 0x80);
    const uint64_t dq6 = replicate(c, 0x40);
    /* Erase and program sector. */
    for (uint32_t i = 0; i < nb_sectors; ++i) {
        uint64_t byte_addr = i * sector_len;
        sector_erase(c, byte_addr);
        /* Read toggle. */
        uint64_t status0 = flash_read(c, byte_addr);
        /* DQ7 is 0 during an erase. */
        g_assert_cmpint(status0 & dq7, ==, 0);
        uint64_t status1 = flash_read(c, byte_addr);
        /* DQ6 toggles during an erase. */
        g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
        /* Wait for erase to complete. */
        qtest_clock_step_next(c->qtest);
        /* Ensure DQ6 has stopped toggling. */
        g_assert_cmpint(flash_read(c, byte_addr), ==, flash_read(c, byte_addr));
        /* Now the data should be valid. */
        g_assert_cmpint(flash_read(c, byte_addr), ==, bank_mask(c));

        /* Program a bit pattern. */
        program(c, byte_addr, 0x55);
        g_assert_cmpint(flash_read(c, byte_addr) & 0xFF, ==, 0x55);
        program(c, byte_addr, 0xA5);
        g_assert_cmpint(flash_read(c, byte_addr) & 0xFF, ==, 0x05);
    }

    /* Erase the chip. */
    chip_erase(c);
    /* Read toggle. */
    uint64_t status0 = flash_read(c, 0);
    /* DQ7 is 0 during an erase. */
    g_assert_cmpint(status0 & dq7, ==, 0);
    uint64_t status1 = flash_read(c, 0);
    /* DQ6 toggles during an erase. */
    g_assert_cmpint(status0 & dq6, ==, ~status1 & dq6);
    /* Wait for erase to complete. */
    qtest_clock_step_next(c->qtest);
    /* Ensure DQ6 has stopped toggling. */
    g_assert_cmpint(flash_read(c, 0), ==, flash_read(c, 0));
    /* Now the data should be valid. */

    for (uint32_t i = 0; i < nb_sectors; ++i) {
        uint64_t byte_addr = i * sector_len;
        g_assert_cmpint(flash_read(c, byte_addr), ==, bank_mask(c));
    }

    /* Unlock bypass */
    unlock(c);
    flash_cmd(c, UNLOCK0_ADDR, UNLOCK_BYPASS_CMD);
    bypass_program(c, 0 * c->bank_width, 0x01);
    bypass_program(c, 1 * c->bank_width, 0x23);
    bypass_program(c, 2 * c->bank_width, 0x45);
    /*
     * Test that bypass programming, unlike normal programming can use any
     * address for the PROGRAM_CMD.
     */
    flash_cmd(c, FLASH_ADDR(3 * c->bank_width), PROGRAM_CMD);
    flash_write(c, 3 * c->bank_width, 0x67);
    wait_for_completion(c, 3 * c->bank_width);
    flash_cmd(c, FLASH_ADDR(0), UNLOCK_BYPASS_RESET_CMD);
    bypass_program(c, 4 * c->bank_width, 0x89); /* Should fail. */
    g_assert_cmpint(flash_read(c, 0 * c->bank_width), ==, 0x01);
    g_assert_cmpint(flash_read(c, 1 * c->bank_width), ==, 0x23);
    g_assert_cmpint(flash_read(c, 2 * c->bank_width), ==, 0x45);
    g_assert_cmpint(flash_read(c, 3 * c->bank_width), ==, 0x67);
    g_assert_cmpint(flash_read(c, 4 * c->bank_width), ==, bank_mask(c));

    /* Test ignored high order bits of address. */
    flash_cmd(c, FLASH_ADDR(0x5555), UNLOCK0_CMD);
    flash_cmd(c, FLASH_ADDR(0x2AAA), UNLOCK1_CMD);
    flash_cmd(c, FLASH_ADDR(0x5555), AUTOSELECT_CMD);
    g_assert_cmpint(flash_query(c, FLASH_ADDR(0)), ==, replicate(c, 0xBF));
    reset(c);

    qtest_quit(qtest);
}

static void cleanup(void *opaque)
{
    unlink(image_path);
}

/*
 * XXX: Tests are limited to bank_width = 2 for now because that's what
 * hw/arm/musicpal.c has.
 */
static const FlashConfig configuration[] = {
    /* One x16 device. */
    {
        .bank_width = 2,
        .device_width = 2,
        .max_device_width = 2,
    },
    /* Implicitly one x16 device. */
    {
        .bank_width = 2,
        .device_width = 0,
        .max_device_width = 0,
    },
    /* Implicitly one x16 device. */
    {
        .bank_width = 2,
        .device_width = 2,
        .max_device_width = 0,
    },
    /* Interleave two x8 devices. */
    {
        .bank_width = 2,
        .device_width = 1,
        .max_device_width = 1,
    },
    /* Interleave two implicit x8 devices. */
    {
        .bank_width = 2,
        .device_width = 1,
        .max_device_width = 0,
    },
    /* Interleave two x8/x16 devices in x8 mode. */
    {
        .bank_width = 2,
        .device_width = 1,
        .max_device_width = 2,
    },
    /* One x16/x32 device in x16 mode. */
    {
        .bank_width = 2,
        .device_width = 2,
        .max_device_width = 4,
    },
    /* Two x8/x32 devices in x8 mode; I am not sure if such devices exist. */
    {
        .bank_width = 2,
        .device_width = 1,
        .max_device_width = 4,
    },
};

int main(int argc, char **argv)
{
    int fd = mkstemp(image_path);
    if (fd == -1) {
        err(1, "Failed to create temporary file %s", image_path);
    }
    if (ftruncate(fd, FLASH_SIZE) < 0) {
        int error_code = errno;
        close(fd);
        unlink(image_path);
        g_printerr("Failed to truncate file %s to 8 MB: %s\n", image_path,
                   strerror(error_code));
        exit(EXIT_FAILURE);
    }
    close(fd);

    qtest_add_abrt_handler(cleanup, NULL);
    g_test_init(&argc, &argv, NULL);

    size_t nb_configurations = sizeof configuration / sizeof configuration[0];
    for (size_t i = 0; i < nb_configurations; ++i) {
        const FlashConfig *config = &configuration[i];
        char *path = g_strdup_printf("pflash-cfi02/%d-%d-%d",
                                     config->bank_width,
                                     config->device_width,
                                     config->max_device_width);
        qtest_add_data_func(path, config, test_flash);
        g_free(path);
    }
    int result = g_test_run();
    cleanup(NULL);
    return result;
}

/* vim: set sw=4 sts=4 ts=8 et: */
