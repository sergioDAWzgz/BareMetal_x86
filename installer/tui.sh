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

# --- The frame every dialog menu uses --------------------------------------
# dialog draws its own screens on the alternate screen, so anything that stays
# visible while the installer works (the countdown and the progress screen) is
# painted here with the same compiled-in theme: blue screen, white panel, white
# border with separator ticks, blue title and a black shadow.

screen_size() {
    rows=$(stty size < /dev/tty0 2>/dev/null | cut -d' ' -f1)
    cols=$(stty size < /dev/tty0 2>/dev/null | cut -d' ' -f2)
    case "$rows" in '' | *[!0-9]*) rows=25 ;; esac
    case "$cols" in '' | *[!0-9]*) cols=80 ;; esac
}

repeat_char() {
    n=$1
    glyph=$2
    out=
    while [ "$n" -gt 0 ]; do
        out="$out$glyph"
        n=$((n - 1))
    done
    printf '%s' "$out"
}

panel_draw() {
    panel_top=$1
    panel_left=$2
    panel_width=$3
    panel_height=$4
    panel_title=$5
    panel_inner=$((panel_width - 2))
    dashes=$((panel_inner - ${#panel_title} - 2))
    [ "$dashes" -lt 0 ] && dashes=0
    dleft=$((dashes / 2))
    dright=$((dashes - dleft))
    lrule=$(repeat_char "$dleft" '─')
    rrule=$(repeat_char "$dright" '─')
    rule=$(repeat_char "$panel_inner" '─')

    printf '\033[44m\033[2J\033[H' > /dev/tty0
    row=$panel_top
    while [ "$row" -lt $((panel_top + panel_height)) ]; do
        printf '\033[%d;%dH\033[47m%*s' "$row" "$panel_left" "$panel_width" '' > /dev/tty0
        row=$((row + 1))
    done
    printf '\033[%d;%dH\033[47;1m┌%s\033[47;34;1m┤%s├\033[47;1m%s┐' \
        "$panel_top" "$panel_left" "$lrule" "$panel_title" "$rrule" > /dev/tty0
    row=$((panel_top + 1))
    while [ "$row" -lt $((panel_top + panel_height - 1)) ]; do
        printf '\033[%d;%dH\033[47;1m│' "$row" "$panel_left" > /dev/tty0
        printf '\033[%d;%dH\033[47;1m│' "$row" $((panel_left + panel_width - 1)) > /dev/tty0
        row=$((row + 1))
    done
    printf '\033[%d;%dH\033[47;1m└%s┘' \
        $((panel_top + panel_height - 1)) "$panel_left" "$rule" > /dev/tty0
    if [ $((panel_left + panel_width)) -le "$cols" ]; then
        row=$((panel_top + 1))
        while [ "$row" -le $((panel_top + panel_height)) ] && [ "$row" -le "$rows" ]; do
            printf '\033[%d;%dH\033[40m ' "$row" $((panel_left + panel_width)) > /dev/tty0
            row=$((row + 1))
        done
    fi
    if [ $((panel_top + panel_height)) -le "$rows" ]; then
        printf '\033[%d;%dH\033[40m%*s' $((panel_top + panel_height)) \
            $((panel_left + 1)) $((panel_width - 1)) '' > /dev/tty0
    fi
    PANEL_TOP=$panel_top
    PANEL_LEFT=$panel_left
    PANEL_INNER=$panel_inner
}

# Writes one body line and blanks the rest of the row, so a shorter message never
# leaves part of the previous one on screen.
panel_line() {
    line_row=$((PANEL_TOP + $1))
    printf '\033[%d;%dH\033[47;30m%*s' "$line_row" $((PANEL_LEFT + 1)) \
        "$PANEL_INNER" '' > /dev/tty0
    printf '\033[%d;%dH\033[47;30m%s' "$line_row" $((PANEL_LEFT + 1)) "$2" \
        > /dev/tty0
}

# The countdown bar in the style of dialog's pause box: a blue segment that
# shrinks with the remaining seconds, number centred on it.
countdown_bar() {
    remaining=$1
    filled=$((remaining * PANEL_INNER / 10))
    [ "$filled" -gt "$PANEL_INNER" ] && filled=$PANEL_INNER
    empty=$((PANEL_INNER - filled))
    bar_row=$((PANEL_TOP + 2))
    bar_left=$((PANEL_LEFT + 1))
    printf '\033[%d;%dH\033[44;37m%*s\033[47;30m%*s' "$bar_row" "$bar_left" \
        "$filled" '' "$empty" '' > /dev/tty0
    if [ "$remaining" -lt 10 ]; then digits=" $remaining"; else digits=$remaining; fi
    offset=$(((PANEL_INNER - ${#digits}) / 2))
    if [ "$offset" -lt "$filled" ]; then
        printf '\033[%d;%dH\033[44;37m%s' "$bar_row" $((bar_left + offset)) \
            "$digits" > /dev/tty0
    else
        printf '\033[%d;%dH\033[47;30m%s' "$bar_row" $((bar_left + offset)) \
            "$digits" > /dev/tty0
    fi
}

# --- Progress screen --------------------------------------------------------
# The console output of the install stays on COM1; tty0 shows one status line
# with the last step (like Debian's installer) and a bar that moves while the
# rootfs is extracted, which is the only step that takes real time.

PROGRESS_READY=0

progress_init() {
    screen_size
    width=72
    height=8
    [ "$width" -gt $((cols - 2)) ] && width=$((cols - 2))
    [ "$height" -gt $((rows - 2)) ] && height=$((rows - 2))
    top=$(((rows - height) / 2 + 1))
    left=$(((cols - width) / 2 + 1))
    panel_draw "$top" "$left" "$width" "$height" " Instalando BareMetal_x86 "
    P_TOP=$top
    P_BAR=$((PANEL_INNER - 12))
    [ "$P_BAR" -lt 10 ] && P_BAR=10
    PROGRESS_READY=1
}

progress() {
    pct=$1
    msg=$2
    case "$pct" in '' | *[!0-9]*) pct=0 ;; esac
    [ "$pct" -gt 100 ] && pct=100
    if [ "$PROGRESS_READY" -ne 1 ]; then
        progress_init
    fi
    panel_line 1 "   $msg"
    filled=$((pct * P_BAR / 100))
    empty=$((P_BAR - filled))
    fill=$(printf '%*s' "$filled" '' | tr ' ' '#')
    gap=$(printf '%*s' "$empty" '' | tr ' ' ' ')
    printf '\033[%d;%dH\033[47;30m   [\033[47;34m%s\033[47;30m%s] %3d%%' \
        $((P_TOP + 4)) $((PANEL_LEFT + 1)) "$fill" "$gap" "$pct" > /dev/tty0
}

# --- Install flows ----------------------------------------------------------

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
    # The engine's own output is the COM1 log; tty0 shows only the progress
    # screen. die() reopens tty0 for its error dialog.
    install_system "$DISK" "$LAYOUT" > /dev/console 2>&1
    # dialog's pause widget shows the remaining seconds and one OK button: the
    # box reboots on its own after 10 s, or immediately when OK is pressed.
    dialog --title "Instalación completada" --no-cancel --pause \
        "Retira el medio de instalación.\n\nEl equipo se reiniciará solo." 11 66 10
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
    # The engine's own output is the COM1 log; tty0 shows only the progress
    # screen. die() reopens tty0 for its error dialog.
    install_system "$DISK" "$LAYOUT" > /dev/console 2>&1
    # dialog's pause widget shows the remaining seconds and one OK button: the
    # box reboots on its own after 10 s, or immediately when OK is pressed.
    dialog --title "Instalación completada" --no-cancel --pause \
        "Retira el medio de instalación.\n\nEl equipo se reiniciará solo." 11 66 10
    sync
    reboot -f
}

# The countdown is painted once and only its own line is rewritten, so the
# number changes without a full repaint.
countdown_aborted() {
    pressed=0
    screen_size
    width=64
    height=10
    [ "$width" -gt $((cols - 2)) ] && width=$((cols - 2))
    [ "$height" -gt $((rows - 2)) ] && height=$((rows - 2))
    top=$(((rows - height) / 2 + 1))
    left=$(((cols - width) / 2 + 1))
    panel_draw "$top" "$left" "$width" "$height" " BareMetal_x86 "
    panel_line 1 "   Instalación por defecto"
    panel_line 3 "   Se usará el primer disco interno, teclado español"
    panel_line 4 "   y todo el disco."
    panel_line 6 "   Pulsa cualquier tecla para configurar manualmente."

    stty -icanon min 1 time 0 < /dev/tty0 2>/dev/null || true
    i=10
    while [ "$i" -gt 0 ]; do
        countdown_bar "$i"
        if timeout 1 dd bs=1 count=1 < /dev/tty0 >/dev/null 2>&1; then
            pressed=1
            break
        fi
        i=$((i - 1))
    done
    if [ "$pressed" -eq 0 ]; then
        panel_line 1 "   Instalando el sistema..."
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
    quit)
        # Leaving the installer must not drop to a root shell on the live
        # image; reboot like the finished-install screens do.
        sync
        reboot -f
        ;;
    esac
done
