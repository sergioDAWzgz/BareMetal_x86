#!/bin/sh
# Install engine. Sourced by tui.sh; it runs on the installer console and logs
# progress to /dev/console (COM1) so a headless run is observable.
set -u

PAYLOAD=/media/payload
ESP_START=2048
ESP_SECTORS=131072            # 64 MiB
ROOT_START=$((ESP_START + ESP_SECTORS))
MIN_FIRST_START=145           # stage 1 (1) + stage 2 (16) + kernel (128)

log() {
    echo "BMX-INSTALLER: $*" > /dev/console
}

die() {
    log "error: $*"
    dialog --title "Error" --msgbox "$*" 10 70
    # Keep PID 1 alive: a failed install drops to a shell instead of panicking.
    exec /bin/sh
}

part_dev() {
    case "$1" in
    *nvme* | *mmcblk* | *loop*) echo "$1p$2" ;;
    *) echo "$1$2" ;;
    esac
}

part_num() {
    # Last run of digits in the device name.
    echo "${1##*[!0-9]}"
}

list_disks() {
    for sys in /sys/block/sd* /sys/block/vd* /sys/block/nvme*; do
        [ -e "$sys" ] || continue
        name=${sys##*/}
        [ "$name" = "${ISO_DISK:-}" ] && continue
        # Only internal disks are targets: removable media and USB transports
        # are never selected by the one-button default.
        [ "$(cat "/sys/block/$name/removable" 2>/dev/null)" = "0" ] || continue
        case "$(readlink -f "/sys/block/$name/device" 2>/dev/null)" in
        */usb*) continue ;;
        esac
        echo "$name"
    done
}

disk_label() {
    size=$(cat "/sys/block/$1/size" 2>/dev/null || echo 0)
    echo "$1 ($((size / 2048)) MiB)"
}

first_disk() {
    list_disks | head -1
}

new_disk_id() {
    cut -c1-8 /proc/sys/kernel/random/uuid
}

write_mbr_default() {
    disk=$1
    id=$2
    sfdisk "$disk" >/dev/null <<EOF || die "no se pudo escribir la tabla en $disk"
label: dos
label-id: 0x$id
start=$ESP_START, size=$ESP_SECTORS, type=c
start=$ROOT_START, size=+, type=83
EOF
}

read_table() {
    table=$(sfdisk -d "$1" 2>/dev/null) || return 1
    ESP_DEV=$(printf '%s\n' "$table" |
        awk '/(type|Id)=(c|ef)([ ,]|$)/{print $1; exit}')
    ROOT_DEV=$(printf '%s\n' "$table" |
        awk '/(type|Id)=83([ ,]|$)/{print $1; exit}')
    first_start=$(printf '%s\n' "$table" |
        awk -F'start=' '/start=/{split($2, a, ","); print a[1] + 0; exit}')
    [ -n "$first_start" ] || first_start=0
    return 0
}

VALIDATION_ERROR=
validate_manual() {
    read_table "$1" || {
        VALIDATION_ERROR="no se pudo leer la tabla de $1"
        return 1
    }
    [ -n "$ESP_DEV" ] || {
        VALIDATION_ERROR="hace falta una partición FAT32 (tipo c o ef)"
        return 1
    }
    [ -n "$ROOT_DEV" ] || {
        VALIDATION_ERROR="hace falta una partición Linux (tipo 83)"
        return 1
    }
    [ "$first_start" -ge "$MIN_FIRST_START" ] || {
        VALIDATION_ERROR="la primera partición debe empezar después del sector $MIN_FIRST_START"
        return 1
    }
    ESP_NUM=$(part_num "$ESP_DEV")
    ROOT_NUM=$(part_num "$ROOT_DEV")
    [ -n "$ESP_NUM" ] && [ -n "$ROOT_NUM" ] || {
        VALIDATION_ERROR="no se pudo leer el número de partición"
        return 1
    }
    return 0
}

install_system() {
    disk=$1
    layout=$2
    id=$(new_disk_id)
    [ -n "$id" ] || die "no se pudo generar el identificador de disco"
    log "disk $disk layout $layout id $id"
    if [ "$layout" = auto ]; then
        write_mbr_default "$disk" "$id"
        ESP_NUM=1
        ROOT_NUM=2
    else
        sfdisk --disk-id "$disk" "0x$id" >/dev/null 2>&1 || true
        validate_manual "$disk" || die "$VALIDATION_ERROR"
    fi
    ESP_PART=$(part_dev "$disk" "$ESP_NUM")
    ROOT_PART=$(part_dev "$disk" "$ROOT_NUM")
    partx -u "$disk" >/dev/null 2>&1 || blockdev --rereadpt "$disk" >/dev/null 2>&1 || true
    sleep 1
    [ -b "$ESP_PART" ] || die "no apareció $ESP_PART"
    [ -b "$ROOT_PART" ] || die "no apareció $ROOT_PART"

    # Boot code first (440 bytes), then the table tools already wrote 446..509.
    dd if="$PAYLOAD/stage1.bin" of="$disk" bs=440 count=1 conv=notrunc 2>/dev/null ||
        die "no se pudo escribir stage 1"

    cp "$PAYLOAD/kernel.bin" /tmp/kernel.bin || die "falta el kernel del payload"
    offset=$(cat "$PAYLOAD/kernel.uuid-offset") || die "falta el offset del PARTUUID"
    printf '%s-%02d' "$id" "$ROOT_NUM" |
        dd of=/tmp/kernel.bin bs=1 seek="$offset" conv=notrunc 2>/dev/null ||
        die "no se pudo parchear el PARTUUID"

    log "format esp $ESP_PART"
    mkfs.fat -F 32 -n BMXDATA "$ESP_PART" >/dev/null || die "mkfs.fat falló en $ESP_PART"
    mkdir -p /mnt/esp
    mount "$ESP_PART" /mnt/esp || die "no se pudo montar $ESP_PART"
    cp -a "$PAYLOAD/esp/." /mnt/esp/ || die "no se pudo copiar el ESP"
    cp /tmp/kernel.bin /mnt/esp/EFI/BOOT/KERNEL.BIN || die "no se pudo copiar KERNEL.BIN"
    umount /mnt/esp || die "no se pudo desmontar el ESP"

    log "format root $ROOT_PART"
    mkfs.ext4 -q -L BMXROOT "$ROOT_PART" || die "mkfs.ext4 falló en $ROOT_PART"
    mkdir -p /mnt/root
    mount "$ROOT_PART" /mnt/root || die "no se pudo montar $ROOT_PART"
    tar -xzf "$PAYLOAD/rootfs.tar.gz" -C /mnt/root || die "no se pudo extraer el rootfs"
    mkdir -p /mnt/root/etc
    cp "$KEYMAP_FILE" /mnt/root/etc/keymap || die "no se pudo copiar el keymap"
    umount /mnt/root || die "no se pudo desmontar la raíz"

    log "write stage 2 and kernel"
    dd if="$PAYLOAD/stage2.bin" of="$disk" bs=512 seek=1 conv=notrunc 2>/dev/null ||
        die "no se pudo escribir stage 2"
    dd if=/tmp/kernel.bin of="$disk" bs=512 seek=17 conv=notrunc 2>/dev/null ||
        die "no se pudo escribir el kernel"
    sync
    log "complete on $disk root=$ROOT_NUM keymap=$KEYMAP_FILE"
}
