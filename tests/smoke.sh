#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
command -v qemu-system-x86_64 >/dev/null || {
    echo "Missing qemu-system-x86_64; install qemu-system-x86 to run smoke tests." >&2
    exit 1
}
work=${WORK:?Expected build directory}
mkdir -p "$work/smoke"
gcc -Wall -Wextra -Werror tests/fixture.c -o "$work/smoke/fixture"
"$work/smoke/fixture" > "$work/smoke/vpd.bin"
printf 'bad' > "$work/smoke/bad.bin"
printf '%025d' 0 > "$work/smoke/malformed.bin"
gcc -m32 -ffreestanding -fno-pie -fno-stack-protector -Wall -Wextra -Werror \
    -Os -c tests/guest.c -o "$work/smoke/guest.o"
gcc -m32 -c tests/guest.S -o "$work/smoke/entry.o"
ld -m elf_i386 -T tests/guest.ld "$work/smoke/entry.o" \
    "$work/smoke/guest.o" -o "$work/smoke/guest.elf"

run_guest() {
    local name=$1 expected=$2 machine=$3
    shift 3
    local status=0
    timeout --kill-after=2s 20s qemu-system-x86_64 \
        -machine "$machine",accel=tcg -m 128 -smp 1 \
        -display none -serial none -monitor none -nic none -no-reboot \
        -bios "$work/bios-vpd.bin" -kernel "$work/smoke/guest.elf" \
        -append "vpd=$expected" \
        -device isa-debug-exit,iobase=0xf4,iosize=4 \
        -debugcon "file:$work/smoke/$name.log" \
        -chardev "file,id=bioslog,path=$work/smoke/$name-bios.log" \
        -device isa-debugcon,iobase=0x402,chardev=bioslog \
        "$@" || status=$?
    if [[ $expected == panic-* ]]; then
        [[ $status == 124 ]]
        if [[ $expected == panic-size ]]; then
            grep -F 'Certificate VPD: invalid fw_cfg size' "$work/smoke/$name-bios.log"
        else
            grep -F 'Certificate VPD: malformed certificate container' "$work/smoke/$name-bios.log"
        fi
        ! grep -q 'GUEST PASS' "$work/smoke/$name.log"
    else
        if [[ $status != 33 ]] || ! grep -q 'GUEST PASS' "$work/smoke/$name.log"; then
            cat "$work/smoke/$name.log" "$work/smoke/$name-bios.log" >&2
            echo "QEMU diagnostic $name failed (exit $status)" >&2
            return 1
        fi
    fi
    echo "PASS: QEMU TCG $name ($machine)"
}
run_guest absent 0 q35
run_guest absent-pc 0 pc
run_guest certificate-q35 1 q35 \
    -fw_cfg "name=opt/seabios/cert-vpd,file=$work/smoke/vpd.bin"
run_guest certificate-pc 1 pc \
    -fw_cfg "name=opt/seabios/cert-vpd,file=$work/smoke/vpd.bin"
run_guest invalid-size panic-size q35 \
    -fw_cfg "name=opt/seabios/cert-vpd,file=$work/smoke/bad.bin"
run_guest malformed panic-container q35 \
    -fw_cfg "name=opt/seabios/cert-vpd,file=$work/smoke/malformed.bin"
