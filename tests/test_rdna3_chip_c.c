/*
 * test_rdna3_chip_c.c — rdna3_chip.h compiled as C99 (mesboot and phxctl are C): the header must build there with
 * -Wall -Wextra -Werror and give the same answers as in C++.
 */
#include "rdna3_chip.h"

uint32_t rdna3_chip_c_translate(uint16_t pci_id, uint32_t phx_abs, int *ok);

uint32_t rdna3_chip_c_translate(uint16_t pci_id, uint32_t phx_abs, int *ok) {
    uint32_t out = 0;
    *ok = rdna3_gcvm_translate(rdna3_chip_for_pci(pci_id), phx_abs, &out);
    return out;
}
