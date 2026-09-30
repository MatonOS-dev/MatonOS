// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/err.h>

static int __init matonos_addon_test_init(void)
{
	struct device *dev;
	const struct firmware *fw;
	int ret;

	pr_info("matonos_addon_test: add-on slot test module loaded\n");
	dev = root_device_register("matonos-addon-test");
	if (IS_ERR(dev)) {
		pr_warn("matonos_addon_test: root test device unavailable\n");
		return 0;
	}
	ret = request_firmware(&fw, "matonos/test-addon.bin", dev);
	if (ret) {
		pr_info("matonos_addon_test: optional firmware unavailable: %d\n", ret);
	} else {
		pr_info("matonos_addon_test: firmware served (%zu bytes)\n", fw->size);
		release_firmware(fw);
	}
	root_device_unregister(dev);
	return 0;
}

static void __exit matonos_addon_test_exit(void)
{
	pr_info("matonos_addon_test: add-on slot test module unloaded\n");
}

module_init(matonos_addon_test_init);
module_exit(matonos_addon_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MatonOS out-of-tree add-on persistence test");
MODULE_ALIAS("matonos:addon-test");
