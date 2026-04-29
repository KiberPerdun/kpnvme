#include "NVMe.h"

static s32
kpnvme_probe (struct pci_dev *pdev, const struct pci_device_id *id)
{
  s32 ret;

  dev_info (&pdev->dev, "kpnvme: probe (VID=0x%04x DID=0x%04x)\n",
            pdev->vendor, pdev->device);

  struct kpnvme_dev *dev __free (kfree) = kzalloc (sizeof (*dev), GFP_KERNEL);
  if (!dev)
    return -ENOMEM;

  ret = pci_enable_device_mem (pdev);
  if (ret)
    return ret;

  struct pci_dev *pdev_enable __free (kpnvme_pci_disable_device) = pdev;

  ret = pci_request_regions (pdev, KPNVME_NAME);
  if (ret)
    return ret;

  struct pci_dev *pdev_reg __free (kpnvme_pci_release_regions) = pdev;

  void __iomem *bar __free (kpnvme_iounmap) = pci_ioremap_bar (pdev, 0);
  if (!bar)
    return -ENOMEM;

  ret = dma_set_mask_and_coherent (&pdev->dev, DMA_BIT_MASK (64));
  if (ret)
    {
      ret = dma_set_mask_and_coherent (&pdev->dev, DMA_BIT_MASK (32));
      if (ret)
        return ret;
    }

  pci_set_master (pdev);
  struct pci_dev *pdev_master __free (kpnvme_pci_clear_master) = pdev;

  u64 cap = readq (bar + NVME_REG_CAP); /* read 64 bit */
  dev->mqes = NVME_CAP_MQES (READ_ONCE (cap));
  dev_info (&pdev->dev, "CAP=0x%016llx\n", cap);
  dev_info (&pdev->dev, "MQES=%u\n", dev->mqes);
  unsigned long timeout_us = NVME_CAP_TIMEOUT (cap) * 500 * 1000;  /* CAP.TO в единицах по 500 мс */

  u32 cc = readl(bar + NVME_REG_CC);
  if (cc & NVME_CC_ENABLE)
    {
      cc &= ~NVME_CC_ENABLE;
      writel(cc, bar + NVME_REG_CC);
    }

  u32 csts;
  ret = readl_poll_timeout(bar + NVME_REG_CSTS, csts,
                            !(csts & NVME_CSTS_RDY),
                            1000, timeout_us);

  if (ret)
    {
      dev_err(&pdev->dev, "disable timeout, CSTS=0x%08x\n", csts);
      return ret;
    }

  dev->sq.depth = ADMIN_SQ_DEPTH;
  dev->sq.vaddr = dmam_alloc_coherent (&pdev->dev,
                                             ADMIN_SQ_DEPTH * sizeof (struct nvme_command),
                                             &dev->sq.dma, GFP_KERNEL);
  if (!dev->sq.vaddr)
    return -ENOMEM;

  dev->cq.depth = ADMIN_CQ_DEPTH;
  dev->cq.vaddr = dmam_alloc_coherent (&pdev->dev,
                                             ADMIN_CQ_DEPTH * sizeof (struct nvme_completion),
                                             &dev->cq.dma, GFP_KERNEL);
  if (!dev->cq.vaddr)
    return -ENOMEM;

  dev->cq.phase = 1;

  u32 dstrd = NVME_CAP_STRIDE(cap);
  u32 db_stride = 4 << dstrd;

  dev->sq.db = bar + 0x1000 + (2 * 0    ) * db_stride;   /* admin SQ tail */
  dev->cq.db = bar + 0x1000 + (2 * 0 + 1) * db_stride;   /* admin CQ head */

  writeq (dev->sq.dma, bar + NVME_REG_ASQ);
  writeq (dev->cq.dma, bar + NVME_REG_ACQ);

  writel ((ADMIN_CQ_DEPTH - 1) << 16 | (ADMIN_SQ_DEPTH - 1), bar + NVME_REG_AQA);

  cc  = 0;
  cc |= (6 << NVME_CC_IOSQES_SHIFT);
  cc |= (4 << NVME_CC_IOCQES_SHIFT);
  cc |= ((PAGE_SHIFT - 12) << NVME_CC_MPS_SHIFT);
  cc |= NVME_CC_CSS_NVM;
  cc |= NVME_CC_ENABLE;
  writel(cc, bar + NVME_REG_CC);

  ret = readl_poll_timeout (bar + NVME_REG_CSTS, csts,
                            (csts & NVME_CSTS_RDY) || (csts & NVME_CSTS_CFS),
                            1000, timeout_us);
  if (ret)
    {
      dev_err(&pdev->dev, "enable timeout, CSTS=0x%08x\n", csts);
      return ret;
    }

  if (csts & NVME_CSTS_CFS)
    {
      dev_err(&pdev->dev, "controller fatal, CSTS=0x%08x\n", csts);
      return -EIO;
    }
  dev_info (&pdev->dev, "controller ready, CSTS=0x%08x\n", csts);

  dma_addr_t id_dma;
  void *id_buf = dmam_alloc_coherent (&pdev->dev, NVME_IDENTIFY_DATA_SIZE,
                                      &id_dma, GFP_KERNEL);
  if (!id_buf)
    return -ENOMEM;

  u16 cmd_id = 0;

  /* получить indetify controller (figure 251) */
  struct nvme_command cmd = {
    .identify = {
        .opcode = nvme_admin_identify,
        .command_id = cmd_id++,
        .nsid = 0,
        .cns = NVME_ID_CNS_CTRL,
        .dptr = {
            .prp1 = cpu_to_le64 (id_dma),
            .prp2 = 0,
        },
    }
  };

  dev->sq.vaddr[dev->sq.tail] = cmd;
  dev->sq.tail = (dev->sq.tail + 1) % dev->sq.depth;
  wmb ();

  writel (dev->sq.tail, dev->sq.db);

  struct nvme_completion *cqe = &((struct nvme_completion *)dev->cq.vaddr)[dev->cq.head];

  while ((le16_to_cpu (READ_ONCE (cqe->status)) & 1) != dev->cq.phase)
    cpu_relax ();

  rmb ();

  u16 status = le16_to_cpu (READ_ONCE (cqe->status)) >> 1;
  u16 command_id = le16_to_cpu (READ_ONCE (cqe->command_id));
  u16 sq_head __maybe_unused = le16_to_cpu (cqe->sq_head);
  if (status)
    dev_err (&pdev->dev, "identify failed, status=0x%04x\n", status);

  if (command_id != cmd_id)
    dev_err (&pdev->dev, "identify failed, command_id=0x%04x\n", command_id);

  ++dev->cq.head;
  if (dev->cq.head == dev->cq.depth)
    {
      dev->cq.head  = 0;
      dev->cq.phase ^= 1;
    }
  writel (dev->cq.head, dev->cq.db);

  dev->iosqes = ((struct nvme_id_ctrl *) id_buf)->sqes & 0x0f;
  dev->iocqes = ((struct nvme_id_ctrl *) id_buf)->cqes & 0x0f;

  dev->sqe_size = 1U << (dev->iosqes & 0x0f);
  dev->cqe_size = 1U << (dev->iocqes & 0x0f);
  dev_info (&pdev->dev, "kpnvme: IOSQES=%d IOCQES=%d\n", dev->iosqes, dev->iocqes);
  dev_info (&pdev->dev, "kpnvme: SQE_SIZE=%d CQE_SIZE=%d\n", dev->sqe_size, dev->cqe_size);

  /* Выставление фичи */
  struct nvme_command cmd_num_queues = {
    .features = {
        .opcode     = nvme_admin_set_features,
        .command_id = cmd_id,
        .nsid       = 0,
        .fid        = cpu_to_le32(NVME_FEAT_NUM_QUEUES),
        .dword11    = cpu_to_le32((0xffff - 1) | ((0xffff - 1) << 16)),
    },
  };

  dev->sq.vaddr[dev->sq.tail] = cmd_num_queues;
  dev->sq.tail = (dev->sq.tail + 1) % dev->sq.depth;
  wmb ();

  writel (dev->sq.tail, dev->sq.db);

  cqe = &((struct nvme_completion *)dev->cq.vaddr)[dev->cq.head];

  while ((le16_to_cpu (READ_ONCE (cqe->status)) & 1) != dev->cq.phase)
    cpu_relax ();

  rmb ();

  status = le16_to_cpu (READ_ONCE (cqe->status)) >> 1;
  command_id = le16_to_cpu (READ_ONCE (cqe->command_id));
  sq_head  = le16_to_cpu (cqe->sq_head);
  if (status)
    dev_err (&pdev->dev, "set feature failed, status=0x%04x\n", status);

  if (command_id != cmd_id)
    dev_err (&pdev->dev, "set feature failed, command_id=0x%04x\n", command_id);

  ++dev->cq.head;
  if (dev->cq.head == dev->cq.depth)
    {
      dev->cq.head  = 0;
      dev->cq.phase ^= 1;
    }
  writel (dev->cq.head, dev->cq.db);

  u32 result = le32_to_cpu (READ_ONCE (cqe->result.u32));
  u16 nr_sq = (result & 0xffff) + 1;
  u16 nr_cq = (result >> 16) + 1;
  u16 nr_io_pairs __maybe_unused = min_t (u16, nr_sq, nr_cq);
  u16 nr_io_queues = min_t (u16, nr_io_pairs, num_online_cpus ());
  dev_info (&pdev->dev, "kpnvme: NR_SQ=%d NR_CQ=%d NR_IO_QUEUES=%d\n", nr_sq, nr_cq, nr_io_queues);

  struct irq_affinity affd = {
    .pre_vectors = 1,
  };

  s32 nvecs = pci_alloc_irq_vectors_affinity (pdev,
                                          1,
                                          nr_io_queues,
                                          PCI_IRQ_MSIX | PCI_IRQ_AFFINITY,
                                          &affd);
  if (nvecs < 0)
    return nvecs;

  dev->io_queues = min_t (u16, nr_io_queues, nvecs - 1);
  dev->queues = devm_kcalloc (&pdev->dev, dev->io_queues + 1,
                              sizeof (*dev->queues), GFP_KERNEL);

  if (dev->queues)
    return -ENOMEM;

  dev->queues[0].qid = 0;
  dev->queues[0].vector = 0;
  dev->queues[0].irq = pci_irq_vector (pdev, 0);

  for (u16 qid = 1; qid <= dev->io_queues; qid++)
    {
      struct kpnvme_queue *q = &dev->queues[qid];
      const struct cpumask *mask;

      q->qid = qid;
      q->vector = qid;
      q->irq = pci_irq_vector(pdev, qid);

      mask = pci_irq_get_affinity (pdev, qid);
      q->cpu = mask ? cpumask_first (mask) : -1;
    }

  struct nvme_command __maybe_unused cmd_cq = {
    .create_cq = {
        .opcode = nvme_admin_create_cq,
        .command_id = cmd_id++,
        .cqid = cpu_to_le16 (dev->cq.qid),
        .qsize = cpu_to_le16 (dev->cq.depth - 1),
        .cq_flags = cpu_to_le16 (NVME_CQ_IRQ_ENABLED),
        .irq_vector = cpu_to_le16 (dev->queues[1].vector),
    }
  };

  dev->pdev = pdev;
  dev->bar = bar;
  dev_info (&pdev->dev, "kpnvme: BAR0=%pK IRQ=%d\n", dev->bar, dev->irq);

  struct kpnvme_dev *out = no_free_ptr (dev);
  void __iomem *bar_kept __maybe_unused = no_free_ptr (bar);
  struct pci_dev *p1 __maybe_unused = no_free_ptr (pdev_enable);
  struct pci_dev *p2 __maybe_unused = no_free_ptr (pdev_reg);
  struct pci_dev *p3 __maybe_unused = no_free_ptr (pdev_master);
  pci_set_drvdata(pdev, out);
  return 0;
}

static void
kpnvme_remove (struct pci_dev *pdev)
{
  struct kpnvme_dev *dev = pci_get_drvdata (pdev);

  dev_info (&pdev->dev, "kpnvme: remove\n");

  pci_free_irq_vectors (pdev);
  if (dev->bar)
    iounmap (dev->bar);

  pci_release_regions (pdev);
  pci_disable_device (pdev);
  kfree (dev);
}

static const struct pci_device_id kpnvme_id_table[]
    = { { PCI_DEVICE_CLASS (PCI_CLASS_STORAGE_EXPRESS, 0xffffff) }, { 0 } };
MODULE_DEVICE_TABLE (pci, kpnvme_id_table);

static struct pci_driver kpnvme_pci_driver = {
  .name = KPNVME_NAME,
  .id_table = kpnvme_id_table,
  .probe = kpnvme_probe,
  .remove = kpnvme_remove,
};

s32
kpnvme_pci_init (void)
{
  s32 ret = 0;
  major = register_blkdev (0, DEVICE_NAME);
  if (major < 0)
    {
      pr_err ("kpnvme: failed to register block device\n");
      return major;
    }

  ret = pci_register_driver (&kpnvme_pci_driver);
  if (ret)
    {
      pr_err ("kpnvme: failed to register PCI driver\n");
      unregister_blkdev (major, DEVICE_NAME);
    }

  return ret;
}

void
kpnvme_pci_exit (void)
{
  pci_unregister_driver (&kpnvme_pci_driver);
}
