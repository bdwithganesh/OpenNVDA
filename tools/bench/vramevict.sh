# 0.180.195 eviction: process A keeps 10 GiB of VRAM textures (or buffers) alive and idle; B asks for 13 GiB.
# A then re-reads every resource. PASS = both PASS, B got VRAM (demotions > 0), A's data intact.
cd /Users/dhruveen/nvbuild
M=${1:-tex}
P() { ioreg -l -w0 | grep -oE '"NVGspControl-(vram-(demot|promot)[a-z-]*|mem-vram-bytes|native615-ready)" = [^,}]*' | sort -u | tr '\n' ' '; echo; }
echo "before: $(P)"
sudo -u dhruveen env VRAMFILL_HOLD=40 ./vramfill 10 $M > /tmp/eA.txt 2>&1 &
for i in $(seq 1 90); do grep -q holding /tmp/eA.txt && break; sleep 1; done
sleep 3
sudo -u dhruveen env OS_ACTIVITY_DT_MODE=1 ./vramfill 13 $M > /tmp/eB.txt 2>&1
echo "after B: $(P)"
wait
echo "--- A"; grep -E "allocated|after hold|after release" /tmp/eA.txt
echo "--- B"; grep -E "allocated|after release|e00002db" /tmp/eB.txt | sed -E 's/^.*\] //' | sort | uniq -c | cut -c1-150
echo "final: $(P)"
