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
        --menu "Elige el teclado de la consola:" 15 60 8 \
        es "Español" us "Inglés (US)" latam "Latinoamérica" de "Alemán" \
        fr "Francés" it "Italiano" pt "Portugués")
    [ -n "$choice" ] || return 0
    load_keymap "/usr/share/bkeymaps/$choice/$choice.bmap.gz" ||
        dialog --msgbox "No se encontró el mapa de teclado $choice." 8 50
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
        "Se borrará el disco seleccionado:" 15 70 6 $items)
    [ -n "$choice" ] && DISK=/dev/$choice
}

choose_manual() {
    [ -n "$DISK" ] || choose_disk
    [ -n "$DISK" ] || return 0
    dialog --title "Particionado manual" --msgbox \
        "Se abrirá cfdisk sobre $DISK.\nCrea una partición FAT32 (tipo c) para el ESP y una Linux (tipo 83) para la raíz.\nLa primera partición debe empezar después del sector 144." \
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
        "Instalación completada.\nRetira el medio de instalación y reinicia." 10 60
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
    install_system "$DISK" "$LAYOUT"
    dialog --title "Listo" --msgbox \
        "Instalación completada.\nRetira el medio de instalación y reinicia." 10 60
    sync
    reboot -f
}

# The first screen is the one screen in the installer that cannot use dialog:
# dialog draws on the alternate screen and takes it away when it exits, so a
# per-second infobox repaints the whole screen. This reproduces dialog's
# compiled-in theme instead (cyan on blue screen, bold white frame, blue title,
# black on white body) and paints it once. Only the countdown line is rewritten,
# so the number changes without a full repaint.
countdown_aborted() {
    pressed=0
    rows=$(stty size < /dev/tty0 2>/dev/null | cut -d' ' -f1)
    cols=$(stty size < /dev/tty0 2>/dev/null | cut -d' ' -f2)
    case "$rows" in '' | *[!0-9]*) rows=25 ;; esac
    case "$cols" in '' | *[!0-9]*) cols=80 ;; esac
    width=64
    height=10
    [ "$width" -gt $((cols - 2)) ] && width=$((cols - 2))
    [ "$height" -gt $((rows - 2)) ] && height=$((rows - 2))
    top=$(((rows - height) / 2 + 1))
    left=$(((cols - width) / 2 + 1))
    inner=$((width - 2))
    text_left=$((left + 1))
    title=" BareMetal_x86 "
    dashes=$((inner - 17))
    [ "$dashes" -lt 0 ] && dashes=0
    dleft=$((dashes / 2))
    dright=$((dashes - dleft))
    repeat() {
        n=$1
        glyph=$2
        out=
        while [ "$n" -gt 0 ]; do
            out="$out$glyph"
            n=$((n - 1))
        done
        printf '%s' "$out"
    }
    rule=$(repeat "$inner" '─')
    lrule=$(repeat "$dleft" '─')
    rrule=$(repeat "$dright" '─')

    printf '\033[44m\033[2J\033[H' > /dev/tty0
    row=$top
    while [ "$row" -lt $((top + height)) ]; do
        printf '\033[%d;%dH\033[47m%*s' "$row" "$left" "$width" '' > /dev/tty0
        row=$((row + 1))
    done
    printf '\033[%d;%dH\033[47;1m┌%s\033[47;34;1m┤%s├\033[47;1m%s┐' \
        "$top" "$left" "$lrule" "$title" "$rrule" > /dev/tty0
    row=$((top + 1))
    while [ "$row" -lt $((top + height - 1)) ]; do
        printf '\033[%d;%dH\033[47;1m│' "$row" "$left" > /dev/tty0
        printf '\033[%d;%dH\033[47;1m│' "$row" $((left + width - 1)) > /dev/tty0
        row=$((row + 1))
    done
    printf '\033[%d;%dH\033[47;1m└%s┘' \
        $((top + height - 1)) "$left" "$rule" > /dev/tty0
    if [ $((left + width)) -le "$cols" ]; then
        row=$((top + 1))
        while [ "$row" -le $((top + height)) ] && [ "$row" -le "$rows" ]; do
            printf '\033[%d;%dH\033[40m ' "$row" $((left + width)) > /dev/tty0
            row=$((row + 1))
        done
    fi
    if [ $((top + height)) -le "$rows" ]; then
        printf '\033[%d;%dH\033[40m%*s' $((top + height)) $((left + 1)) \
            $((width - 1)) '' > /dev/tty0
    fi
    printf '\033[%d;%dH\033[47;30m%s' $((top + 3)) "$text_left" \
        "   Se usará el primer disco interno, teclado español" > /dev/tty0
    printf '\033[%d;%dH\033[47;30m%s' $((top + 4)) "$text_left" \
        "   y todo el disco." > /dev/tty0
    printf '\033[%d;%dH\033[47;30m%s' $((top + 6)) "$text_left" \
        "   Pulsa cualquier tecla para configurar manualmente." > /dev/tty0

    stty -icanon min 1 time 0 < /dev/tty0 2>/dev/null || true
    i=10
    while [ "$i" -gt 0 ]; do
        if [ "$i" -eq 1 ]; then unit=segundo; else unit=segundos; fi
        printf '\033[%d;%dH\033[47;30m   Instalación por defecto en %2d %s. ' \
            $((top + 1)) "$text_left" "$i" "$unit" > /dev/tty0
        if timeout 1 dd bs=1 count=1 < /dev/tty0 >/dev/null 2>&1; then
            pressed=1
            break
        fi
        i=$((i - 1))
    done
    if [ "$pressed" -eq 0 ]; then
        printf '\033[%d;%dH\033[47;30m%*s' $((top + 1)) "$text_left" "$inner" '' \
            > /dev/tty0
        printf '\033[%d;%dH\033[47;30m   Instalando el sistema... ' \
            $((top + 1)) "$text_left" > /dev/tty0
    fi
    printf '\033[0m' > /dev/tty0
    stty sane < /dev/tty0 2>/dev/null || true
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
