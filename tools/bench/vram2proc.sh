# process A takes 10 GiB of VRAM textures, frees them and stays alive; B then asks for 13 GiB.
cd /Users/dhruveen/nvbuild
sudo -u dhruveen env OS_ACTIVITY_DT_MODE=1 ./vramfill 10 tex 60 > /tmp/vA.txt 2>&1 &
for i in $(seq 1 60); do grep -q holding /tmp/vA.txt && break; sleep 1; done
sudo -u dhruveen env OS_ACTIVITY_DT_MODE=1 ./vramfill 13 > /tmp/vB.txt 2>&1
echo "--- A"; grep -E "allocated|after release|returned|holding" /tmp/vA.txt | sed -E 's/^.*\] //'
echo "--- B"; grep -E "allocated|after release|returned|NoSpace|e00002db|  [0-9]+ x" /tmp/vB.txt | sed -E 's/^.*\] //' | uniq -c | cut -c1-150
kill %1 2>/dev/null; wait
