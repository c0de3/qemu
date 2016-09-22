#include "qemu/osdep.h"
#include "libqtest.h"


static void test_object_add_without_props(void)
{
    QDict *ret, *error;
    const gchar *class, *desc;

    ret = qmp("{'execute': 'object-add',"
              " 'arguments': { 'qom-type': 'memory-backend-ram', 'id': 'ram1' } }");
    g_assert_nonnull(ret);

    error = qdict_get_qdict(ret, "error");
    class = qdict_get_try_str(error, "class");
    desc = qdict_get_try_str(error, "desc");

    g_assert_cmpstr(class, ==, "GenericError");
    g_assert_cmpstr(desc, ==, "can't create backend with size 0");

    QDECREF(ret);
}

static void test_qom_set_without_value(void)
{
    QDict *ret, *error;
    const gchar *class, *desc;

    ret = qmp("{'execute': 'qom-set',"
              " 'arguments': { 'path': '/machine', 'property': 'rtc-time' } }");
    g_assert_nonnull(ret);

    error = qdict_get_qdict(ret, "error");
    class = qdict_get_try_str(error, "class");
    desc = qdict_get_try_str(error, "desc");

    g_assert_cmpstr(class, ==, "GenericError");
    g_assert_cmpstr(desc, ==, "Parameter 'value' is missing");

    QDECREF(ret);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);

    qtest_start("");

    qtest_add_func("/qemu-qmp/object-add-without-props",
                   test_object_add_without_props);
    qtest_add_func("/qemu-qmp/qom-set-without-value",
                   test_qom_set_without_value);

    ret = g_test_run();

    qtest_end();

    return ret;
}
