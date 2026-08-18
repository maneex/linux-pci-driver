#ifndef VELOCITOR_DMA_H
#define VELOCITOR_DMA_H

// Linux headers.
#include <linux/pci.h>

// https://www.kernel.org/doc/html/v6.8/core-api/dma-api-howto.html
// https://www.kernel.org/doc/html/v6.6/driver-api/dmaengine/provider.html

struct velocitor_dma_buf {
  void *cpu;
  dma_addr_t dma;
};

int velocitor_dma_initialize(struct pci_dev *dev);

int velocitor_dma_dbg_write(struct pci_dev *dev, u32 offset, u32 pool_offset,
                            u32 len, u32 *err_code);

int velocitor_dma_dbg_read(struct pci_dev *dev, u32 offset, u32 pool_offset,
                           u32 len, u32 *err_code);

#endif // not VELOCITOR_DMA_H
