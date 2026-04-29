#ifndef _KPNVME_H
#define _KPNVME_H

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/nvme.h>
#include <linux/blk-mq.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/cleanup.h>
#include <linux/iopoll.h>
#include <linux/fs.h>
#include <linux/blk-mq.h>
#include <linux/blk-mq-pci.h>

#define KPNVME_NAME    "kpnvme"
#define DEVICE_NAME    "kpnvme"
#define KPNVME_VERSION "0.1.0"

#define ADMIN_SQ_DEPTH 64
#define ADMIN_CQ_DEPTH 64

static   s32 major __maybe_unused;

DEFINE_FREE (kpnvme_iounmap, void __iomem *,
             if (_T) iounmap(_T))

DEFINE_FREE (kpnvme_pci_disable_device, struct pci_dev *,
             if (_T) pci_disable_device (_T))

DEFINE_FREE (kpnvme_pci_release_regions, struct pci_dev *,
             if (_T) pci_release_regions (_T))

DEFINE_FREE (kpnvme_pci_free_irq_vectors, struct pci_dev *,
             if (_T) pci_free_irq_vectors (_T))

DEFINE_FREE (kpnvme_pci_clear_master, struct pci_dev *,
             if (_T) pci_clear_master (_T))

struct kpnvme_queue;
struct kpnvme_dev;

struct kpnvme_queue
{
  u16 qid;       /* 0 = admin, 1..N = I/O */
  u16 vector;    /* MSI-X vector index */
  s32 irq;       /* Linux IRQ number */
  s32 cpu;

  struct nvme_command *vaddr;
  dma_addr_t dma;
  u32 depth;
  u32 head, tail;
  void *db;
  u16 phase;
};

struct kpnvme_dev
{
  struct pci_dev *pdev;
  void __iomem *bar;
  struct kpnvme_queue *queues;
  __u32 mqes;
  __s32 irq;
  /* Размера слота в очереди 3:0 */
  __u8 iosqes;
  __u8 iocqes;
  /* размера слота в байтах 2^n от ioqes */
  __u8 sqe_size;
  __u8 cqe_size;
  /* максимальное кол-во cq и sq очередей */
  __u16 io_pairs;
  __u16 io_queues;
  struct kpnvme_queue sq;
  struct kpnvme_queue cq;
};

s32 kpnvme_pci_init(void);
void kpnvme_pci_exit(void);

#endif /* _KPNVME_H */
