#ifndef _LINUX_VIRTIO_CONFIG_H
#define _LINUX_VIRTIO_CONFIG_H
/* Virtio devices use a standardized configuration space to define their
 * features and pass configuration information, but each implementation can
 * store and access that space differently. */
#include <linux/types.h>

/* Status byte for guest to report progress, and synchronize features. */
/* We have seen device and processed generic fields (VIRTIO_CONFIG_F_VIRTIO) */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE	1
/* We have found a driver for the device. */
#define VIRTIO_CONFIG_S_DRIVER		2
/* Driver has used its parts of the config, and is happy */
#define VIRTIO_CONFIG_S_DRIVER_OK	4
/* We've given up on this device. */
#define VIRTIO_CONFIG_S_FAILED		0x80

#ifdef __KERNEL__
struct virtio_device;

/**
 * virtio_config_ops - operations for configuring a virtio device
 * @get: read the value of a configuration field
 *	vdev: the virtio_device
 *	offset: the offset of the configuration field
 *	buf: the buffer to write the field value into.
 *	len: the length of the buffer
 *	Note that contents are conventionally little-endian.
 * @set: write the value of a configuration field
 *	vdev: the virtio_device
 *	offset: the offset of the configuration field
 *	buf: the buffer to read the field value from.
 *	len: the length of the buffer
 *	Note that contents are conventionally little-endian.
 * @get_status: read the status byte
 *	vdev: the virtio_device
 *	Returns the status byte
 * @set_status: write the status byte
 *	vdev: the virtio_device
 *	status: the new status byte
 * @reset: reset the device
 *	vdev: the virtio device
 *	After this, status and feature negotiation must be done again
 * @find_vq: find a virtqueue and instantiate it.
 *	vdev: the virtio_device
 *	index: the 0-based virtqueue number in case there's more than one.
 *	callback: the virqtueue callback
 *	Returns the new virtqueue or ERR_PTR() (eg. -ENOENT).
 * @del_vq: free a virtqueue found by find_vq().
 * @get_features: get the array of feature bits for this device.
 *	vdev: the virtio_device
 *	Returns the first 32 feature bits (all we currently need).
 * @set_features: confirm what device features we'll be using.
 *	vdev: the virtio_device
 *	feature: the first 32 feature bits
 */
struct virtio_config_ops
{
	void (*get)(struct virtio_device *vdev, unsigned offset,
		    void *buf, unsigned len);
	void (*set)(struct virtio_device *vdev, unsigned offset,
		    const void *buf, unsigned len);
	u8 (*get_status)(struct virtio_device *vdev);
	void (*set_status)(struct virtio_device *vdev, u8 status);
	void (*reset)(struct virtio_device *vdev);
	struct virtqueue *(*find_vq)(struct virtio_device *vdev,
				     unsigned index,
				     void (*callback)(struct virtqueue *));
	void (*del_vq)(struct virtqueue *vq);
	u32 (*get_features)(struct virtio_device *vdev);
	void (*set_features)(struct virtio_device *vdev, u32 features);
};

/* If driver didn't advertise the feature, it will never appear. */
void virtio_check_driver_offered_feature(const struct virtio_device *vdev,
					 unsigned int fbit);

/**
 * virtio_has_feature - helper to determine if this device has this feature.
 * @vdev: the device
 * @fbit: the feature bit
 */
static inline bool virtio_has_feature(const struct virtio_device *vdev,
				      unsigned int fbit)
{
	/* Did you forget to fix assumptions on max features? */
	if (__builtin_constant_p(fbit))
		BUILD_BUG_ON(fbit >= 32);

	virtio_check_driver_offered_feature(vdev, fbit);
	return test_bit(fbit, vdev->features);
}

/**
 * virtio_config_val - look for a feature and get a single virtio config.
 * @vdev: the virtio device
 * @fbit: the feature bit
 * @offset: the type to search for.
 * @val: a pointer to the value to fill in.
 *
 * The return value is -ENOENT if the feature doesn't exist.  Otherwise
 * the value is endian-corrected and returned in v. */
#define virtio_config_val(vdev, fbit, offset, v) ({			\
	int _err;							\
	if (virtio_has_feature((vdev), (fbit))) {			\
		__virtio_config_val((vdev), (offset), (v));		\
		_err = 0;						\
	} else								\
		_err = -ENOENT;						\
	_err;								\
})

/**
 * __virtio_config_val - get a single virtio config without feature check.
 * @vdev: the virtio device
 * @offset: the type to search for.
 * @val: a pointer to the value to fill in.
 *
 * The value is endian-corrected and returned in v. */
#define __virtio_config_val(vdev, offset, v) do {			\
	BUILD_BUG_ON(sizeof(*(v)) != 1 && sizeof(*(v)) != 2		\
		     && sizeof(*(v)) != 4 && sizeof(*(v)) != 8);	\
	(vdev)->config->get((vdev), (offset), (v), sizeof(*(v)));	\
	switch (sizeof(*(v))) {						\
	case 2: le16_to_cpus((__u16 *) v); break;			\
	case 4: le32_to_cpus((__u32 *) v); break;			\
	case 8: le64_to_cpus((__u64 *) v); break;			\
	}								\
} while(0)
#endif /* __KERNEL__ */
#endif /* _LINUX_VIRTIO_CONFIG_H */
