#!/usr/bin/env bash
# Builds facetos.iso. Works in Termux (Android) and on Linux.
# Needs: clang, lld, make, git, xorriso
set -e
cd "$(dirname "$0")"

if [ ! -d limine ]; then
  git clone https://github.com/limine-bootloader/limine.git --branch=v9.x-binary --depth=1
fi
[ -x limine/limine ] || make -C limine >/dev/null

CFLAGS="--target=i686-unknown-none-elf -m32 -march=i686 -std=gnu11 -O2 -Wall \
 -ffreestanding -fno-stack-protector -fno-PIC -fno-builtin -mno-sse -mno-mmx -mno-80387"
clang $CFLAGS -c kernel.c -o kernel.o
ld.lld -m elf_i386 -nostdlib -static -T linker.ld kernel.o -o facetos.elf

rm -rf iso_root && mkdir -p iso_root/boot/limine iso_root/EFI/BOOT
cp facetos.elf iso_root/boot/
cp limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/boot/limine/
cp limine/BOOTX64.EFI limine/BOOTIA32.EFI iso_root/EFI/BOOT/

rm -f facetos.iso
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
  -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
  -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
  -efi-boot-part --efi-boot-image --protective-msdos-label \
  iso_root -o facetos.iso >/dev/null 2>&1
./limine/limine bios-install facetos.iso >/dev/null 2>&1
echo "Built facetos.iso ($(du -h facetos.iso | cut -f1))"

# The Facet Disk: a blank 8 MB hard disk image. FacetOS sets it up on first boot.
# It is never overwritten, so your saved files are safe when you rebuild.
if [ ! -f facetos-disk.img ]; then
  truncate -s 8M facetos-disk.img 2>/dev/null || dd if=/dev/zero of=facetos-disk.img bs=1M count=8 2>/dev/null
  echo "Created blank facetos-disk.img"
fi

if [ -w /sdcard/Download ]; then
  cp -f facetos.iso /sdcard/Download/facetos.iso && echo "Copied facetos.iso to /sdcard/Download"
  if [ ! -f /sdcard/Download/facetos-disk.img ]; then
    cp facetos-disk.img /sdcard/Download/ && echo "Copied facetos-disk.img to /sdcard/Download"
  fi
fi
