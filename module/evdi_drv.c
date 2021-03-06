/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2016 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include "evdi_drv.h"
#include "evdi_drm.h"
#include "evdi_debug.h"

MODULE_AUTHOR("DisplayLink (UK) Ltd.");
MODULE_DESCRIPTION("Extensible Virtual Display Interface");
MODULE_LICENSE("GPL");

static struct evdi_context {
	struct device *root_dev;
	unsigned dev_count;
	struct platform_device *devices[16];
} evdi_context;

static struct drm_driver driver;

struct drm_ioctl_desc evdi_painter_ioctls[] = {
	DRM_IOCTL_DEF_DRV(EVDI_CONNECT, evdi_painter_connect_ioctl,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_REQUEST_UPDATE,
			  evdi_painter_request_update_ioctl, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GRABPIX, evdi_painter_grabpix_ioctl,
			  DRM_UNLOCKED),
};

static const struct vm_operations_struct evdi_gem_vm_ops = {
	.fault = evdi_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct file_operations evdi_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.mmap = evdi_drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = drm_ioctl,
	.release = drm_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static struct drm_driver driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME,
	.load = evdi_driver_load,
	.unload = evdi_driver_unload,
	.preclose = evdi_driver_preclose,

	/* gem hooks */
	.gem_free_object = evdi_gem_free_object,
	.gem_vm_ops = &evdi_gem_vm_ops,

	.dumb_create = evdi_dumb_create,
	.dumb_map_offset = evdi_gem_mmap,
	.dumb_destroy = drm_gem_dumb_destroy,

	.ioctls = evdi_painter_ioctls,
	.num_ioctls = ARRAY_SIZE(evdi_painter_ioctls),

	.fops = &evdi_driver_fops,

	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = evdi_gem_prime_import,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static void evdi_add_device(void)
{
	struct platform_device_info pdevinfo = {
		.parent = NULL,
		.name = "evdi",
		.id = evdi_context.dev_count,
		.res = NULL,
		.num_res = 0,
		.data = NULL,
		.size_data = 0,
		.dma_mask = DMA_BIT_MASK(32),
	};
	evdi_context.devices[evdi_context.dev_count] =
	    platform_device_register_full(&pdevinfo);
	if (dma_set_mask(&evdi_context.devices[evdi_context.dev_count]->dev,
			 DMA_BIT_MASK(64))) {
		EVDI_DEBUG("Unable to change dma mask to 64 bit. ");
		EVDI_DEBUG("Sticking with 32 bit\n");
	}
	evdi_context.dev_count++;
}

static int evdi_platform_probe(struct platform_device *pdev)
{
	EVDI_CHECKPT();
	return drm_platform_init(&driver, pdev);
}

static int evdi_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm_dev =
	    (struct drm_device *)platform_get_drvdata(pdev);
	EVDI_CHECKPT();

	drm_unplug_dev(drm_dev);

	return 0;
}

void evdi_driver_preclose(struct drm_device *drm_dev, struct drm_file *file)
{
	struct evdi_device *evdi = drm_dev->dev_private;

	EVDI_CHECKPT();
	if (evdi)
		evdi_painter_close(evdi, file);
}

static void evdi_remove_all(void)
{
	int i;

	EVDI_DEBUG("removing all evdi devices\n");
	for (i = 0; i < evdi_context.dev_count; ++i) {
		if (evdi_context.devices[i]) {
			EVDI_DEBUG("removing evdi %d\n", i);

			platform_device_unregister(evdi_context.devices[i]);
			evdi_context.devices[i] = NULL;
		}
	}
	evdi_context.dev_count = 0;
}

static struct platform_driver evdi_platform_driver = {
	.probe = evdi_platform_probe,
	.remove = evdi_platform_remove,
	.driver = {
		   .name = "evdi",
		   .mod_name = KBUILD_MODNAME,
		   .owner = THIS_MODULE,
		   },
};

static ssize_t version_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u.%u.%u\n", DRIVER_MAJOR,
			DRIVER_MINOR, DRIVER_PATCHLEVEL);
}

static ssize_t count_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", evdi_context.dev_count);
}

static ssize_t add_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	int parsed;
	unsigned int val;

	parsed = kstrtouint(buf, 10, &val);
	if (parsed != 0) {
		EVDI_DEBUG(" invalid device count \"%s\"\n", buf);
	} else if (val == 0) {
		EVDI_VERBOSE(" adding 0 devices has no effect\n");
	} else {
		unsigned new_dev_count = evdi_context.dev_count + val;

		EVDI_DEBUG(" increasing device count to %u\n", new_dev_count);
		while (val--)
			evdi_add_device();
	}

	return count;
}

static ssize_t remove_all_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	evdi_remove_all();
	return count;
}

static ssize_t loglevel_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", g_evdi_loglevel);
}

static ssize_t loglevel_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	int parsed;
	unsigned int val;

	parsed = kstrtouint(buf, 10, &val);
	if (parsed == 0) {
		if (val >= EVDI_LOGLEVEL_ALWAYS &&
		    val <= EVDI_LOGLEVEL_VERBOSE) {
			EVDI_LOG("setting loglevel to %u\n", val);
			g_evdi_loglevel = val;
		} else {
			EVDI_ERROR("invalid loglevel %u\n", val);
		}
	} else {
		EVDI_ERROR("unable to parse %u\n", val);
	}
	return count;
}

static struct device_attribute evdi_device_attributes[] = {
	__ATTR_RO(count),
	__ATTR_RO(version),
	__ATTR_RW(loglevel),
	__ATTR_WO(add),
	__ATTR_WO(remove_all)
};

static int __init evdi_init(void)
{
	int i;

	EVDI_LOG("Initialising logging on level %u\n", g_evdi_loglevel);
	evdi_context.root_dev = root_device_register("evdi");
	if (!PTR_RET(evdi_context.root_dev))
		for (i = 0; i < ARRAY_SIZE(evdi_device_attributes); i++) {
			device_create_file(evdi_context.root_dev,
					   &evdi_device_attributes[i]);
		}

	return platform_driver_register(&evdi_platform_driver);
}

static void __exit evdi_exit(void)
{
	int i;

	EVDI_CHECKPT();
	evdi_remove_all();
	platform_driver_unregister(&evdi_platform_driver);

	if (!PTR_RET(evdi_context.root_dev)) {
		for (i = 0; i < ARRAY_SIZE(evdi_device_attributes); i++) {
			device_remove_file(evdi_context.root_dev,
					   &evdi_device_attributes[i]);
		}
		root_device_unregister(evdi_context.root_dev);
	}
}

module_init(evdi_init);
module_exit(evdi_exit);
