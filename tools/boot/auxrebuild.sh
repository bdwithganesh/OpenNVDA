#!/bin/sh
# auxrebuild.sh (root): rebuild the AuxKC from /Library/Extensions exactly like build-install-fb8.sh (kexts untouched,
# so their approved cdhashes stay). Keeps a copy of the current AuxKC.
set -eu
BK=/Library/NativeDriverLab/backup/auxkc-$(date +%Y%m%dT%H%M%S); mkdir -p "$BK"
cp -p /Library/KernelCollections/AuxiliaryKernelExtensions.kc "$BK/"
CAND=$(mktemp -d /Library/KernelCollections/.native-kc.XXXXXX)
kmutil create -z -n aux --allow-missing-kdk -V release \
  -B /System/Library/KernelCollections/BootKernelExtensions.kc \
  -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
  -A "$CAND/AuxiliaryKernelExtensions.kc" -r /Library/Extensions > /tmp/kmutil-aux.log 2>&1 \
  || { echo "KMUTIL FAILED"; tail -25 /tmp/kmutil-aux.log; exit 1; }
chown root:wheel "$CAND/AuxiliaryKernelExtensions.kc"; chmod 644 "$CAND/AuxiliaryKernelExtensions.kc"
mv -f "$CAND/AuxiliaryKernelExtensions.kc" /Library/KernelCollections/AuxiliaryKernelExtensions.kc
rmdir "$CAND"
kmutil inspect -V release -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc | grep -i macosdevicelab
