# pulsar-creator (C port)

This is a cross-platform C command-line port of the Pulsar Pack Creator build pipeline.

## Build

```bash
cmake -S tools/pulsar-creator-c -B build/pulsar-creator-c
cmake --build build/pulsar-creator-c
```

## Usage

```bash
./build/pulsar-creator-c/pulsar-creator --track-dir /path/to/szs/files
```

### Options

- `--track-dir <dir>`: Directory containing `.szs` files (required).
- `--mod-name <name>`: Pack folder name (default: `PulsarPack`).
- `--output-dir <dir>`: Output parent directory (default: `output`).
- `--wiimmfi-region <n>`: Wiimmfi region ID for config info (default: `0`).
- `--date <yyyy-mm-dd>`: Date text injected in BMG template.

## Notes

- Track files are sorted alphabetically and grouped into cups of 4.
- The tool copies Pulsar resource binaries/assets from `PulsarPackCreator/Resources`.
- `wbmgt` (Wiimm's SZS tools) must be installed and available in `PATH`.
