# hidpi_override.sh (root): Mac-style scaled HiDPI list for the 4K panel (vendor 09d1 model 803b).
O=/Library/Displays/Contents/Resources/Overrides/DisplayVendorID-9d1
N=/Users/dhruveen/nvbuild/perf/DisplayProductID-803b
[ -f "$N" ] && [ ! -L "$N" ] && plutil -lint "$N" || exit 1
mkdir -p $O; [ -e $O/DisplayProductID-803b ] && cp -p $O/DisplayProductID-803b $O/DisplayProductID-803b.before-$(date +%s)
install -o root -g wheel -m 644 "$N" $O/DisplayProductID-803b && rm -f "$N"
chown -R root:wheel /Library/Displays; ls -l $O
