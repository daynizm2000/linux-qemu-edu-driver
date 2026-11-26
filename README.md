# QEMU EDU PCI Driver

A Linux kernel driver for the QEMU `edu` PCI device, exposing its functionality through three character devices: factorial computation, DMA transfers, and direct MMIO access.

The driver targets recent kernel versions (7.x branch) and relies on modern kernel APIs, including `cleanup.h` scoped guards, `srcu`, `kref`, and `devm`/`pcim` managed resources.

---

## Overview

The QEMU `edu` device (`1234:11e8`) is a simple PCI educational device that provides:

- A liveness/version register block
- A factorial computation engine driven by interrupts
- A DMA engine (read/write between host memory and device memory)
- A generic interrupt source

This driver exposes those resources through three character devices per physical card. Multiple cards are supported; each is assigned a unique minor namespace.

---

## Architecture

### Kernel side

- **`edu.c`** — the driver itself: PCI probe/remove, character device creation, IRQ handling, SRCU-protected hardware lifetime, DMA and factorial submission paths.
- **`edu.h`** — shared register definitions, ioctl codes, and constants used by both the kernel module and userspace test program.
- **`Makefile`** — out-of-tree kernel module build.

### Userspace side

- **`test.c`** — a small program that exercises all three character devices.
- **`test.sh`** — compiles `test.c` and runs it (requires the module to be loaded and a device present).

### Device nodes

For a device with index `N` (from `/dev/eduN` naming), the following nodes are created:

| Node                   | Purpose                                  | Operations                     |
|------------------------|------------------------------------------|--------------------------------|
| `/dev/eduN/fac`        | Factorial engine                         | `read`, `write`, `poll`        |
| `/dev/eduN/dma`        | DMA buffer read/write                    | `read`, `write`, `poll`, `pread`, `pwrite`, `llseek` |
| `/dev/eduN/mmio`       | Direct MMIO BAR mapping                  | `mmap`, `ioctl`, `open`, `close` |

`N` is the IDA-allocated device index, capped at `EDU_DEV_COUNT` (255).

---

## Requirements

- Linux kernel **7.x** (built and tested against the latest branch)
- Kernel headers for the running kernel
- `gcc` for the userspace test
- QEMU with the `edu` PCI device, e.g.:

```sh
qemu-system-x86_64 \
    -device edu \
    -kernel bzImage -append "console=ttyS0 root=..." \
    ...
```

Only one EDU card is required to run the tests; the driver supports up to 255.

---

## Building

### Kernel module

```sh
make
sudo make modules_install
sudo insmod edu.ko
```

Or install via your distribution's `dkms` flow if preferred. To remove:

```sh
sudo rmmod edu
```

The build uses `KDIR ?= /lib/modules/$(shell uname -r)/build`, so it can be pointed at an arbitrary kernel source tree:

```sh
make KDIR=/path/to/kernel/build
```

### Userspace test

```sh
./test.sh
```

`test.sh` compiles `test.c` with `gcc`, runs it, and removes the binary. The module must already be loaded and a device must be present (`/dev/edu0/...`).

---

## Usage

### Factorial device (`/dev/eduN/fac`)

Write a `u32` to compute its factorial; the operation is submitted to hardware and completed via interrupt. Read returns the result after the computation finishes.

```c
uint32_t val = 4;
write(fac_fd, &val, sizeof(val));
read(fac_fd, &val, sizeof(val)); /* val == 24 */
```

- Reads block until a result is available, unless the file descriptor was opened with `O_NONBLOCK` (in which case `-EAGAIN` is returned if the result is not ready).
- `poll()` reports `POLLIN` when a result is ready, and `POLLERR | POLLHUP` if the device was removed while the descriptor is open.
- Only one userspace client can drive the hardware at a time; concurrent submissions are serialized by the internal client abstraction.

### DMA device (`/dev/eduN/dma`)

Backed by a single coherent DMA buffer of `EDU_DMA_SIZE` (4096 bytes). Writes move data from userspace into the buffer; reads move data from the buffer to userspace. The DMA engine is engaged transparently for both directions, and the internal buffer preserves state across operations.

```c
char buf[4096];
memset(buf, 'a', sizeof(buf));
write(dma_fd, buf, sizeof(buf));

pread(dma_fd, buf, sizeof(buf), 0); /* buffer contents readable again */
```

- File offsets (`*ppos`) are honored for both read and write; `llseek` is supported.
- Requests past `EDU_DMA_SIZE` are clamped to the buffer end.
- `poll()` reports readiness similarly to the factorial device.

### MMIO device (`/dev/eduN/mmio`)

Exposes the PCI BAR (BAR 0) directly through `mmap`. The size of the BAR is obtainable via `ioctl`.

```c
__u32 size;
ioctl(mmio_fd, EDU_MMIO_SIZE, &size);

void *bar = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, mmio_fd, 0);
/* direct register access via volatile u32 pointers */
```

- The mapping is `pgprot_noncached`, suitable for MMIO access.
- Access through `mmap` is independent of the driver's serialization; do not use it concurrently with the `fac` or `dma` devices for the same hardware, as that would violate the driver's client model.

### ioctl

```c
#define EDU_MMIO_SIZE _IOR('k', 1, __u32)
```

Returns the length of the MMIO BAR for the device. No other ioctls are defined.

---

## Register map

Defined in `edu.h`:

| Offset  | Name                        | Description                                   |
|---------|-----------------------------|-----------------------------------------------|
| `0x00`  | `EDU_VERSION_ID_REG`        | Device version ID (`EDU_VERSION` = `0x10000ed`) |
| `0x04`  | `EDU_LIVENESS_CHECK_REG`    | Write `0x10`, read must equal `~0x10`         |
| `0x08`  | `EDU_FAC_COMP_REG`          | Factorial: write input, read result           |
| `0x20`  | `EDU_STATUS_REG`            | Bit 0: factorial busy                         |
| `0x24`  | `EDU_INTERRUPT_STATUS_REG`  | Pending interrupt bits                        |
| `0x60`  | `EDU_INTERRUPT_RAISE_REG`   | Raise interrupt (write-only)                  |
| `0x64`  | `EDU_INTERRUPT_ACKNOW_REG`  | Acknowledge interrupts                        |
| `0x80`  | `EDU_DMA_SOURCE_REG`        | DMA source address                            |
| `0x88`  | `EDU_DMA_DEST_REG`          | DMA destination address                       |
| `0x90`  | `EDU_DMA_COUNT_REG`         | DMA transfer length                           |
| `0x98`  | `EDU_DMA_CMD_REG`           | DMA command and status (bit 0 = busy)         |
| `0x40000` | `EDU_DMA_BASE`            | Base of DMA-accessible device memory          |

---

## Design notes

- **Hardware lifetime** is protected by an `srcu` domain: userspace entry points hold a read lock around any MMIO access, and `remove()` swaps the `hw` pointer to `NULL` and calls `synchronize_srcu()` before freeing hardware resources.
- **Client model**: The factorial engine and DMA engine each have a single hardware slot. Submissions go through a `edu_client` abstraction that coordinates the IRQ handler, the submitter, and cancellation on device removal. Concurrent submissions from multiple file descriptors are serialized through `edu_client_wait_free()`, which respects `O_NONBLOCK`.
- **IRQ handler** acknowledges the interrupt status register and dispatches to per-client wake callbacks under the client's spinlock, waking the corresponding wait queue.
- **Reference counting** is layered: `edu_dev` (one per card, tied to the PCI device), `edu_fac` (one per open file), `edu_dma` (one per open file). This allows descriptors to outlive the PCI device as long as they are not performing an operation.
- **Poll semantics** are consistent with `O_NONBLOCK` read/write behavior: a `WAIT_DONE` status with no in-flight operation reports `POLLIN | POLLOUT`, an in-flight operation reports `0`, and device removal reports `POLLERR | POLLHUP`.

---

## Test program

`test.c` exercises all three device types against `/dev/edu0`:

1. **Factorial** — submits `4`, expects `24`.
2. **DMA** — writes a buffer of `'a'`, reads it back, and verifies the contents.
3. **MMIO** — maps the BAR, computes `5!` directly through the registers, and verifies `120`.

The program reports the number of passed tests and returns non-zero if any test fails.

---
