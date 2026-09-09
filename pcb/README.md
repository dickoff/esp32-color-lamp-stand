# PCB development

The KiCad project files and the project-local symbol library are the hardware
sources. Edit the schematic in KiCad, update the PCB with `F8`, and route or
refill zones in the PCB Editor.

KiCad 10 and Python 3 are required. On macOS the exporter finds KiCad in
`/Applications/KiCad`; elsewhere, put `kicad-cli` on `PATH` or set
`KICAD_CLI`.

From this directory:

```sh
make check
make manufacturing
```

`make check` runs schematic ERC and PCB DRC, including schematic/PCB parity.
Error-level violations block the command. Review warnings in KiCad before export.

`make manufacturing` performs the checks and writes these upload-ready files
to `manufacturing/jlcpcb/`:

- `esp32-color-lamp-stand-gerbers.zip`
- `jlcpcb_bom.csv`
- `jlcpcb_cpl.csv`
- top and bottom preview images

The exporter omits test points, requires an LCSC number for every assembled
part, and verifies that BOM and CPL designators match. Review the previews and
run JLCPCB's online DFM analysis before ordering.

Some JLCPCB/EasyEDA parts use placement origins or orientations that differ from
the corresponding KiCad footprints. Vendor-specific translation and rotation
corrections for those parts are documented in `jlcpcb-placement-offsets.toml`.
They change only the exported CPL; they do not move PCB footprints or alter the
Gerbers. The exporter looks up each correction by LCSC part number, verifies the
KiCad footprint named in the configuration, rotates local position offsets with
the component, and warns if the expected reference designators have changed.
