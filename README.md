# SeaBIOS VPD

SeaBIOS firmware for QEMU with certificate VPD publication and TPM
platform-authorization handoff, configured through QEMU's `fw_cfg` interface.

## Install

Download [`bios-vpd.bin`](https://github.com/philsphicas/seabios-vpd/releases/latest/download/bios-vpd.bin)
from the latest release, then install it on the Linux host:

```sh
sudo install -D -m 0644 bios-vpd.bin \
  /usr/local/share/seabios-vpd/bios-vpd.bin
```

## Use

Select the firmware and supply the desired options when starting QEMU:

```sh
qemu-system-x86_64 \
  -bios /usr/local/share/seabios-vpd/bios-vpd.bin \
  -fw_cfg name=opt/seabios/cert-vpd,file=certificate-vpd.bin \
  -fw_cfg name=opt/seabios/tpm-platform-auth-handoff,string=1 \
  ...
```

| Option | Effect |
|---|---|
| `opt/seabios/cert-vpd` | Publishes a certificate VPD record for the guest. |
| `opt/seabios/tpm-platform-auth-handoff` | Preserves TPM 2.0 platform authorization for guest initialization when set to ASCII `1`. |

Both options are optional. Omitting them retains standard SeaBIOS behavior.

### Certificate VPD

`certificate-vpd.bin` contains a Google CBMEM VPD container with one `cert`
record. A Linux guest with ACPI, coreboot-table, and Google VPD support can read
the certificate at:

```text
/sys/firmware/vpd/ro/cert
```

The container starts with four little-endian 32-bit integers:

| Offset | Value |
|---|---|
| 0 | `0x43524f53` (Google CBMEM VPD magic) |
| 4 | `1` (format version) |
| 8 | Length of the following read-only record |
| 12 | `0` (read-write region length) |

The record is `01 04 63 65 72 74`, followed by the certificate length, the
certificate bytes, and a `00` terminator. Encode the length in base 128,
most-significant group first, setting the high bit on each group that has a
successor. For example, length 128 is `81 00`. Certificates may contain
1..16384 bytes; the length field uses at most three bytes.

The firmware validates the container and publishes it through an ACPI
`BOOT0000` device referencing a coreboot table in reserved memory. Certificate
authentication is the caller's responsibility. Invalid input or a publication
failure halts boot with a diagnostic. ACPI root addresses must be below 4 GiB.

### TPM platform authorization

ASCII `1` preserves the platform hierarchy's existing authorization by
skipping `tpm20_hierarchychangeauth` during TPM 2.0 boot preparation. TPM
measurements and the remaining preparation steps still run. ASCII `0` selects
standard behavior.

The value must be exactly one ASCII byte. Invalid values halt boot with a
diagnostic when the option is evaluated. Firmware updates can change TPM PCR
measurements.

## Build

Build from an x86-64 Linux checkout. On Ubuntu 24.04, install the build and test
dependencies:

```sh
sudo apt-get install build-essential binutils git patch python3 qemu-system-x86
make
```

The firmware is written to `.build/bios-vpd.bin`.

| Command | Result |
|---|---|
| `make test` | Runs firmware and packaging tests. |
| `make check` | Runs tests and builds the firmware. |
| `make smoke` | Exercises the firmware in QEMU on `q35` and `pc`. |
| `make dist` | Builds and packages release files in `dist/`. |
| `make clean` | Removes build and package outputs. |

The upstream version and commit are pinned in [`config.mk`](config.mk).
Firmware configuration is in [`config/seabios.config`](config/seabios.config).

Release source archives contain this repository's code and the upstream
SeaBIOS source. Extract an archive and run `make` inside its directory to build it.

## License

Project-authored code is licensed under the [MIT License](LICENSE).
SeaBIOS-derived patches and upstream components retain their applicable
licenses; see [NOTICE](NOTICE).
