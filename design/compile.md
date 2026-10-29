# Design compile

## Compile one

```bash
satria src/bootstrap.c \
  --output='build/bootstrap.o' \
  --binary='bin/satria'

# short
satria src/bootstrap.c -o build/bootstrap.o -b bin/satria
```

Langsung masuk fase binary

```bash
satria src/bootstrap.c -f Buildfile
```

Baca konfigurasi untuk menempatkan hasil compile dan binary.
