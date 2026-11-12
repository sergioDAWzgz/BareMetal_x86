#!/bin/sh
# Installer TUI. Runs with stdin/stdout on /dev/tty0; the engine logs progress to
# /dev/console (COM1) so a headless install can be followed from the serial line.
set -u
. /installer/install.sh

KEYMAP_FILE=/usr/share/bkeymaps/es/es.bmap.gz
LAYOUT=auto
DISK=

load_keymap() {
    [ -f "$1" ] || return 1
    zcat "$1" | loadkmap
    KEYMAP_FILE=$1
}

choose_keymap() {
    choice=$(dialog --stdout --title "Distribución de teclado" --default-item es \
        --menu "Elegí el teclado de la consola:" 15 60 8 \
        es "Español" us "Inglés (US)" latam "Latinoamérica" de "Alemán" \
        fr "Francés" it "Italiano" pt "Portugués")
    [ -n "$choice" ] || return 0
    load_keymap "/usr/share/bkeymaps/$choice/$choice.bmap.gz" ||
        dialog --msgbox "No se encontró el keymap $choice." 8 50
}

choose_disk() {
    items=
    for name in $(list_disks); do
        items="$items $name $(disk_label "$name")"
    done
    [ -n "$items" ] || {
        dialog --msgbox "No se encontró ningún disco interno." 8 50
        return 0
    }
    choice=$(dialog --stdout --title "Disco de destino" --menu \
        "Se borrará el disco elegido:" 15 70 6 $items)
    [ -n "$choice" ] && DISK=/dev/$choice
}

choose_manual() {
    [ -n "$DISK" ] || choose_disk
    [ -n "$DISK" ] || return 0
    dialog --title "Particionado manual" --msgbox \
        "Se abrirá cfdisk sobre $DISK.\nCreá una partición FAT32 (tipo c) para el ESP y una Linux (tipo 83) para la raíz.\nLa primera partición debe empezar después del sector 144." \
        12 70
    cfdisk "$DISK"
    if validate_manual "$DISK"; then
        LAYOUT=manual
    else
        dialog --title "Particionado manual" --msgbox "$VALIDATION_ERROR" 10 70
        LAYOUT=auto
    fi
}

do_install() {
    [ -n "$DISK" ] || {
        name=$(first_disk)
        [ -n "$name" ] || {
            dialog --msgbox "No se encontró ningún disco interno." 8 50
            return 0
        }
        DISK=/dev/$name
    }
    dialog --title "Confirmar" --yesno \
        "Disco: $DISK\nParticionado: $LAYOUT\nTeclado: $KEYMAP_FILE\n\nSe borrará el disco. ¿Instalar?" \
        12 70 || return 0
    dialog --title "Instalando" --infobox \
        "Particionando y copiando el sistema en $DISK..." 6 60
    install_system "$DISK" "$LAYOUT"
    dialog --title "Listo" --msgbox \
        "Instalación completa.\nQuitá el medio de instalación y reiniciá." 10 60
    sync
    reboot -f
}

default_install() {
    name=$(first_disk)
    [ -n "$name" ] || {
        dialog --msgbox "No se encontró ningún disco interno." 8 50
        exit 1
    }
    DISK=/dev/$name
    LAYOUT=auto
    log "default install on $DISK"
    dialog --title "Instalando" --infobox \
        "Instalación por defecto en $DISK..." 6 60
    install_system "$DISK" "$LAYOUT"
    dialog --title "Listo" --msgbox \
        "Instalación completa.\nQuitá el medio de instalación y reiniciá." 10 60
    sync
    reboot -f
}

countdown_aborted() {
    pressed=0
    stty -icanon min 1 time 0 < /dev/tty0 2>/dev/null || true
    i=20
    while [ "$i" -gt 0 ]; do
        printf '\rInstalación por defecto en %2d s; cualquier tecla para elegir ' "$i" > /dev/tty0
        if timeout 1 dd bs=1 count=1 < /dev/tty0 >/dev/null 2>&1; then
            pressed=1
            break
        fi
        i=$((i - 1))
    done
    stty sane < /dev/tty0 2>/dev/null || true
    printf '\n' > /dev/tty0
    [ "$pressed" -eq 1 ]
}

if countdown_aborted; then
    : # any key: fall through to the menu
else
    default_install
fi

while :; do
    choice=$(dialog --stdout --title "Instalador BareMetal_x86" --default-item install \
        --menu "Disco: ${DISK:-sin elegir}   Particionado: $LAYOUT   Teclado: $KEYMAP_FILE" \
        16 78 6 \
        install "Instalar ahora" \
        keyboard "Cambiar teclado" \
        disk "Elegir disco" \
        manual "Particionado manual (cfdisk)" \
        quit "Salir")
    case "$choice" in
    install) do_install ;;
    keyboard) choose_keymap ;;
    disk) choose_disk ;;
    manual) choose_manual ;;
    quit | "") exit 0 ;;
    esac
done
