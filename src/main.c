// SPDX-License-Identifier: GPL-2.0
#include "NVMe.h"

MODULE_AUTHOR ("KiberPerdun");
MODULE_DESCRIPTION ("kpnvme - custom NVMe driver");
MODULE_VERSION (KPNVME_VERSION);
MODULE_LICENSE ("GPL");

static s32 __init
kpnvme_init (void)
{
  pr_info ("kpnvme: loading v%s\n", KPNVME_VERSION);
  return kpnvme_pci_init ();
}

static void __exit
kpnvme_exit (void)
{
  pr_info ("kpnvme: unloading\n");
  kpnvme_pci_exit ();
}

module_init (kpnvme_init);
module_exit (kpnvme_exit);
