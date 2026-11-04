#include "ahci.h"

#include "console.h"
#include "lib.h"
#include "memory.h"
#include "pci.h"

/* The driver owns one command list, one FIS receive area, one command table
   and one bounce buffer and reuses them for every port of every controller: a
   bare-metal kernel issues one transfer at a time, so per-port copies would only
   cost frames. */
#define AHCI_MAX_PORTS 32
#define AHCI_MAX_CONTROLLERS 4
#define AHCI_PORT_REGISTER_BASE 0x100
#define AHCI_PORT_REGISTER_STRIDE 0x80

/* HBA registers, byte offsets from the ABAR. */
#define HBA_GHC 0x04
#define HBA_PI 0x0C
#define GHC_AE (1u << 31)               /* AHCI enable */

/* Port registers, byte offsets from the port base. */
#define PORT_CLB 0x00                   /* command list base, low */
#define PORT_CLBU 0x04                  /* command list base, high */
#define PORT_FB 0x08                    /* FIS receive base, low */
#define PORT_FBU 0x0C                   /* FIS receive base, high */
#define PORT_IS 0x10                    /* interrupt status */
#define PORT_CMD 0x18                   /* command and status */
#define PORT_TFD 0x20                   /* task file data */
#define PORT_SIG 0x24                   /* device signature */
#define PORT_SSTS 0x28                  /* SATA status */
#define PORT_SERR 0x30                  /* SATA error */
#define PORT_CI 0x38                    /* command issue */

#define PORT_CMD_ST (1u << 0)           /* start the command list processor */
#define PORT_CMD_FRE (1u << 4)          /* enable the FIS receive engine */
#define PORT_CMD_FR (1u << 14)          /* FIS receive running */
#define PORT_CMD_CR (1u << 15)          /* command list running */

#define PORT_TFD_ERR (1u << 0)
#define PORT_TFD_DRQ (1u << 3)
#define PORT_TFD_BSY (1u << 7)

/* A task file error, a host bus error, an interface or overflow error, or a
   protocol violation all mean the transfer did not complete and the buffer must
   not be trusted. */
#define PORT_IS_FATAL ((1u << 30) | (1u << 29) | (1u << 28) | (1u << 27) | \
                       (1u << 26) | (1u << 24) | (1u << 22))

#define SSTS_DET_MASK 0x0Fu
#define SSTS_DET_READY 0x03u            /* device present, link established */
#define SIG_ATA 0x00000101u

#define FIS_RECEIVE_BYTES 256
#define COMMAND_LIST_BYTES 1024
#define COMMAND_TABLE_OFFSET 0x80       /* PRDT starts after CFIS and ACMD */
#define TRANSFER_BYTES 4096             /* one PRDT entry: eight sectors */
#define SECTORS_PER_COMMAND (TRANSFER_BYTES / AHCI_SECTOR_SIZE)

#define WAIT_SPINS 10000000u

typedef struct {
    uintptr_t hba;                      /* virtual base of the HBA registers */
    uint32_t port;
} ahci_port_t;

typedef struct {
    uintptr_t hba;
    uint32_t implemented;
} ahci_controller_t;

static ahci_controller_t controllers[AHCI_MAX_CONTROLLERS];
static uint32_t controller_count;
static ahci_port_t ports[AHCI_MAX_PORTS];
static uint32_t port_count;
static uintptr_t clb_phys;
static uintptr_t fis_phys;
static uintptr_t table_phys;
static uintptr_t bounce_phys;
static const char *failure = "none";

static uint32_t hba_read(uintptr_t hba, uint32_t offset)
{
    return *(volatile uint32_t *)(hba + offset);
}

static void hba_write(uintptr_t hba, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(hba + offset) = value;
}

static uint32_t port_read(const ahci_port_t *entry, uint32_t offset)
{
    return hba_read(entry->hba, AHCI_PORT_REGISTER_BASE +
                    entry->port * AHCI_PORT_REGISTER_STRIDE + offset);
}

static void port_write(const ahci_port_t *entry, uint32_t offset, uint32_t value)
{
    hba_write(entry->hba, AHCI_PORT_REGISTER_BASE +
              entry->port * AHCI_PORT_REGISTER_STRIDE + offset, value);
}

static uint32_t popcount(uint32_t value)
{
    uint32_t count = 0;

    while (value != 0) {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}

/* Every wait below is a bound on a hardware handshake, not a delay: the loop
   leaves on the first iteration that observes the flag. */
static int wait_until(const ahci_port_t *entry, uint32_t offset, uint32_t mask,
                      int set)
{
    uint32_t spin;

    for (spin = 0; spin < WAIT_SPINS; spin++) {
        uint32_t value = port_read(entry, offset);

        if (((value & mask) == mask) == (set != 0)) {
            return 0;
        }
    }
    return -1;
}

static int wait_ready(const ahci_port_t *entry)
{
    uint32_t spin;

    for (spin = 0; spin < WAIT_SPINS; spin++) {
        uint32_t ssts = port_read(entry, PORT_SSTS);

        if ((ssts & SSTS_DET_MASK) == SSTS_DET_READY) {
            return 0;
        }
        if ((ssts & SSTS_DET_MASK) == 0) {
            return -1;                  /* nothing plugged into this port */
        }
    }
    return -1;
}

static int port_start(const ahci_port_t *entry)
{
    uint32_t command = port_read(entry, PORT_CMD);

    if ((command & PORT_CMD_ST) != 0) {
        command &= ~PORT_CMD_ST;
        port_write(entry, PORT_CMD, command);
        if (wait_until(entry, PORT_CMD, PORT_CMD_CR, 0) != 0) {
            failure = "the command engine did not stop";
            return -1;
        }
    }

    port_write(entry, PORT_CLB, (uint32_t)(clb_phys & 0xFFFFFFFFu));
    port_write(entry, PORT_CLBU, (uint32_t)(clb_phys >> 32));
    port_write(entry, PORT_FB, (uint32_t)(fis_phys & 0xFFFFFFFFu));
    port_write(entry, PORT_FBU, (uint32_t)(fis_phys >> 32));
    port_write(entry, PORT_SERR, 0xFFFFFFFFu);
    port_write(entry, PORT_IS, 0xFFFFFFFFu);

    command = port_read(entry, PORT_CMD) | PORT_CMD_FRE;
    port_write(entry, PORT_CMD, command);
    if (wait_until(entry, PORT_CMD, PORT_CMD_FR, 1) != 0) {
        failure = "the fis receive engine did not start";
        return -1;
    }

    command |= PORT_CMD_ST;
    port_write(entry, PORT_CMD, command);
    if (wait_until(entry, PORT_CMD, PORT_CMD_CR, 1) != 0) {
        failure = "the command list processor did not start";
        return -1;
    }
    return 0;
}

static int port_probe(const ahci_port_t *entry)
{
    uint32_t signature;

    if (wait_ready(entry) != 0) {
        return -1;
    }

    /* A port multiplier, an ATAPI device or a bridge also reports a device
       present; only the plain ATA signature describes a sector-addressed disk. */
    signature = port_read(entry, PORT_SIG);
    if (signature != SIG_ATA) {
        return -1;
    }
    return port_start(entry);
}

static void controller_add(const pci_device_t *device)
{
    ahci_controller_t *controller;
    uintptr_t hba;
    uint32_t bar;
    uint32_t command;
    uint32_t implemented;
    uint32_t port;

    bar = pci_config_read32(device->bus, device->slot, device->function, 0x24);
    if ((bar & 0x1u) != 0 || ((bar >> 1) & 0x3u) != 0) {
        failure = "bar5 is not a 32-bit memory window";
        return;
    }

    /* Memory space access and bus mastering: without the second bit the
       controller cannot fetch the command list or write a sector into RAM. */
    command = pci_config_read32(device->bus, device->slot, device->function, 0x04);
    pci_config_write32(device->bus, device->slot, device->function, 0x04,
                       command | 0x6u);

    hba = (uintptr_t)PHYS_TO_VIRT(bar & ~0x1Fu);
    implemented = hba_read(hba, HBA_PI);
    hba_write(hba, HBA_GHC, hba_read(hba, HBA_GHC) | GHC_AE);

    controller = &controllers[controller_count];
    controller->hba = hba;
    controller->implemented = implemented;
    controller_count++;

    for (port = 0; port < 32 && port_count < AHCI_MAX_PORTS; port++) {
        ahci_port_t candidate;

        if ((implemented & (1u << port)) == 0) {
            continue;
        }

        candidate.hba = hba;
        candidate.port = port;
        if (port_probe(&candidate) != 0) {
            continue;
        }
        ports[port_count] = candidate;
        port_count++;
    }
}

int ahci_init(void)
{
    uint32_t index;

    controller_count = 0;
    port_count = 0;
    failure = "none";

    clb_phys = pmm_alloc_frames(1);
    fis_phys = pmm_alloc_frames(1);
    table_phys = pmm_alloc_frames(1);
    bounce_phys = pmm_alloc_frames(1);
    if (clb_phys == 0 || fis_phys == 0 || table_phys == 0 || bounce_phys == 0) {
        failure = "no frames for the command engine";
        return -1;
    }
    memset(PHYS_TO_VIRT(clb_phys), 0, COMMAND_LIST_BYTES);
    memset(PHYS_TO_VIRT(fis_phys), 0, FIS_RECEIVE_BYTES);
    memset(PHYS_TO_VIRT(table_phys), 0, TRANSFER_BYTES);
    memset(PHYS_TO_VIRT(bounce_phys), 0, TRANSFER_BYTES);

    for (index = 0; index < pci_count(); index++) {
        const pci_device_t *entry = &pci_devices()[index];
        uint32_t class_register;

        if (controller_count >= AHCI_MAX_CONTROLLERS) {
            break;
        }
        if (entry->class_code != 0x01 || entry->subclass != 0x06) {
            continue;
        }
        class_register = pci_config_read32(entry->bus, entry->slot,
                                          entry->function, 0x08);
        if (((class_register >> 8) & 0xFFu) != 0x01u) {
            continue;                   /* a SATA controller in IDE mode */
        }
        controller_add(entry);
    }

    if (controller_count == 0) {
        failure = "no ahci controller";
        return -1;
    }
    if (port_count == 0) {
        failure = "no port with an ata drive";
        return -1;
    }
    return 0;
}

static int wait_idle(const ahci_port_t *entry)
{
    uint32_t spin;

    for (spin = 0; spin < WAIT_SPINS; spin++) {
        if ((port_read(entry, PORT_TFD) & (PORT_TFD_BSY | PORT_TFD_DRQ)) == 0) {
            return 0;
        }
    }
    return -1;
}

/* Builds one READ DMA EXT command in slot 0 and waits for the controller to
   retire it. The bounce buffer is the single PRDT target. */
static int issue_read(const ahci_port_t *entry, uint64_t lba, uint32_t sectors)
{
    volatile uint8_t *fis = (volatile uint8_t *)PHYS_TO_VIRT(table_phys);
    volatile uint32_t *header = (volatile uint32_t *)PHYS_TO_VIRT(clb_phys);
    volatile uint32_t *prdt;
    uint32_t bytes = sectors * AHCI_SECTOR_SIZE;
    uint32_t status;

    fis[0] = 0x27;                      /* host to device register FIS */
    fis[1] = 0x80;                      /* the FIS carries a command */
    fis[2] = 0x25;                      /* READ DMA EXT */
    fis[3] = 0x00;                      /* features */
    fis[4] = (uint8_t)(lba & 0xFFu);
    fis[5] = (uint8_t)((lba >> 8) & 0xFFu);
    fis[6] = (uint8_t)((lba >> 16) & 0xFFu);
    fis[7] = 0x40;                      /* LBA mode, not CHS */
    fis[8] = (uint8_t)((lba >> 24) & 0xFFu);
    fis[9] = (uint8_t)((lba >> 32) & 0xFFu);
    fis[10] = (uint8_t)((lba >> 40) & 0xFFu);
    fis[11] = 0x00;                     /* features, high */
    fis[12] = (uint8_t)((sectors - 1) & 0xFFu);
    fis[13] = (uint8_t)(((sectors - 1) >> 8) & 0xFFu);
    fis[14] = 0x00;
    fis[15] = 0x00;                     /* control */

    prdt = (volatile uint32_t *)(PHYS_TO_VIRT(table_phys) + COMMAND_TABLE_OFFSET);
    prdt[0] = (uint32_t)(bounce_phys & 0xFFFFFFFFu);
    prdt[1] = (uint32_t)(bounce_phys >> 32);
    prdt[2] = 0;
    prdt[3] = (bytes - 1) | (1u << 31); /* byte count, interrupt on completion */

    header[0] = 5u;                     /* command FIS length in dwords */
    header[1] = 1u;                     /* one PRDT entry */
    header[2] = 0;
    header[3] = (uint32_t)(table_phys & 0xFFFFFFFFu);
    header[4] = (uint32_t)(table_phys >> 32);
    header[5] = 0;
    header[6] = 0;
    header[7] = 0;

    port_write(entry, PORT_CI, 1u);
    if (wait_until(entry, PORT_CI, 1u, 0) != 0) {
        failure = "the transfer never completed";
        return -1;
    }

    status = port_read(entry, PORT_IS);
    if ((status & PORT_IS_FATAL) != 0 ||
        (port_read(entry, PORT_TFD) & PORT_TFD_ERR) != 0) {
        failure = "the controller reported a failed command";
        return -1;
    }
    return 0;
}

int ahci_read(uint32_t index, uint64_t lba, uint32_t count, void *buffer)
{
    uint8_t *destination = (uint8_t *)buffer;
    const uint8_t *bounce = (const uint8_t *)PHYS_TO_VIRT(bounce_phys);
    const ahci_port_t *entry;

    if (index >= port_count || buffer == 0 || count == 0) {
        failure = "invalid read request";
        return -1;
    }
    entry = &ports[index];

    while (count > 0) {
        uint32_t sectors = count > SECTORS_PER_COMMAND ? SECTORS_PER_COMMAND : count;
        uint32_t bytes = sectors * AHCI_SECTOR_SIZE;

        if (wait_idle(entry) != 0) {
            failure = "the port stayed busy";
            return -1;
        }
        port_write(entry, PORT_IS, 0xFFFFFFFFu);

        if (issue_read(entry, lba, sectors) != 0) {
            return -1;
        }

        memcpy(destination, bounce, bytes);
        destination += bytes;
        lba += sectors;
        count -= sectors;
    }
    return 0;
}

uint32_t ahci_port_count(void)
{
    return port_count;
}

uint32_t ahci_port_number(uint32_t index)
{
    if (index >= port_count) {
        return 0;
    }
    return ports[index].port;
}

void ahci_print(void)
{
    uint32_t index;

    for (index = 0; index < controller_count; index++) {
        console_write("ahci: abar ");
        console_hex64((uint64_t)controllers[index].hba - PHYS_BASE);
        console_write(" ports ");
        console_dec(popcount(controllers[index].implemented));
        console_write("\n");
    }

    for (index = 0; index < port_count; index++) {
        console_write("ahci: port ");
        console_dec(ports[index].port);
        console_write(" attached\n");
    }
}

const char *ahci_error(void)
{
    return failure;
}
