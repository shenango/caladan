/*
 * pci.h - PCI bus support
 */

#pragma once

#include <base/types.h>
#include <base/kref.h>

struct pci_bar {
	uint64_t start;	/* the start address, or zero if no resource */
	uint64_t len;	/* the length of the resource */
	uint64_t flags; /* Linux resource flags */
};

/* NOTE: these are the same as the Linux PCI sysfs resource flags */
#define PCI_BAR_IO		0x00000100
#define PCI_BAR_MEM		0x00000200
#define PCI_BAR_PREFETCH	0x00002000 /* typically WC memory */
#define PCI_BAR_READONLY	0x00004000 /* typically option ROMs */
#define PCI_MAX_BARS		7

struct pci_addr {
	uint16_t domain;
	uint8_t bus;
	uint8_t slot;
	uint8_t func;
} __packed;

extern int pci_str_to_addr(const char *str, struct pci_addr *addr);

struct pci_dev {
	struct pci_addr addr;
	struct kref ref;

	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subsystem_vendor_id;
	uint16_t subsystem_device_id;

	struct pci_bar bars[PCI_MAX_BARS];
	int numa_node;
	int max_vfs;
};

extern struct pci_dev *pci_alloc_dev(const struct pci_addr *addr);
extern void pci_release_dev(struct kref *ref);
extern struct pci_bar *pci_find_mem_bar(struct pci_dev *dev, int count);
extern void *pci_map_mem_bar(struct pci_dev *dev, struct pci_bar *bar, bool wc);
extern void pci_unmap_mem_bar(struct pci_bar *bar, void *vaddr);

/**
 * pci_dev_get - increments the PCI device refcount
 * @dev: the PCI device
 *
 * Returns the device.
 */
static inline struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
	kref_get(&dev->ref);
	return dev;
}

/**
 * pci_dev_put - decrements the PCI device refcount, freeing at zero
 * @dev: the PCI device
 */
static inline void pci_dev_put(struct pci_dev *dev)
{
	kref_put(&dev->ref, pci_release_dev);
}
