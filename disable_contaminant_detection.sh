#!/vendor/bin/sh

for f in /sys/devices/platform/10cb0000.hsi2c/i2c-*/*-0025; do
  if [ -d $f ]; then
    echo 0 > $f/contaminant_detection;
  fi
done
