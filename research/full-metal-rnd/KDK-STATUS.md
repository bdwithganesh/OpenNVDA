# KDK download status (25 Sep, isi M1 par)

## Koshish (terminal se, fail - evidence ke saath)
- System me KDK nahi hai (`/Library/Developer/KDKs/` missing).
- `developer.apple.com/download/all/` → Apple ID sign-in par redirect (IDMSWebAuth).
- Direct DMG URL guess → `302 https://developer.apple.com/unauthorized/` (login session chahiye).
- `softwareupdate -l` (isi M1 par, Apple ka apna tool): KDK offer hi nahi hota - sirf CLT/Safari/macOS 27 dikhe.
- Public swscan catalog URLs ab 0-byte/404 dete hain; aur KDK public catalog me kabhi tha hi nahi (ADC-portal-only distribution).
- Matlab: **KDK sirf browser-login se milega. Terminal/curl se possible nahi - ye Apple ki distribution limit hai.**

## Tumhe kya karna hai (is M1 ke browser me, login already hai)
1. Browser me kholo: `https://developer.apple.com/download/all/`
2. Search: `Kernel Debug Kit Sonoma`
3. Download karo: **Kernel Debug Kit for macOS Sonoma 14.8.9 (build 23J631)**
   - Exact 23J631 na dikhe to sabse kareeb wala **14.x KDK** le lo (doc me allowed hai).
   - **Galat version mat lena:** KDK target ke OS se match hona chahiye (Sonoma 14.x, x86_64).
     macOS 26/27 ka KDK Sonoma panic symbolize nahi karega - major version ka KDK bekaar hai.
4. DMG file ko yahan rakho: `full-metal-rnd/_dl/kdk/` (folder already banaya jayega - bas file drop kar do)
5. Mujhe bolo "KDK rakh diya" - main andar hi verify karunga (size/type check, mount nahi karunga).

## KDK milte hi (main karunga, isi folder se)
- Symbolized kernel nikaal kar panic traces symbolize karna (S3 debug 30–60 min → ~5 min)
- IOGraphics/IOAccelerator crash lines ka exact source mapping

## DONE (25 Sep night) - KDK installed + verified
- Location: `/Library/Developer/KDKs/KDK_14.8.9_23J631.kdk` (1.6G, system path - read-only use, folder me copy nahi)
- kernel UUID `3CF18F50-FBD8-36B9-8EB8-70F566D2934A` (x86_64); version string target-identical
  (`Darwin 23.6.0 xnu-10063.141.1.713.39~1/RELEASE_X86_64`)
- IOGraphicsFamily.kext + .dSYM present. Demo (atos, file offsets):
  - `handleEvent @0x17578 → IOFramebuffer.cpp:6629`
  - `sleepGate @0xa14a → IOFramebuffer.cpp:1112`
  - `extAcknowledgeNotification @0x1a386 → IOFramebuffer.cpp:9244`
  (Ye S3-wedge path ke exact functions hain - ab panic PCs turant file:line me badlenge.)
- Helper: `KDK-USE/kdk_symbols.py` (`sym` = addr→symbol, `nm` = symbol search, `--selftest` OK 4/4)
- Note: KDK line numbers (6629/1112/9244) authoritative hain 23J631 ke liye; purane source-drop
  references (6580/6913/4176) doosre xnu drop ke the.

## GIT (26 Sep) - full KDK compressed + pushed
- Location (repo): `full-metal-rnd/_dl/kdk/KDK_14.8.9_23J631.tar.zst.part-aa..ak`
  (11 parts × ≤90M, total ~979M, zstd -19) + `SHA256SUMS.txt`. Har part GitHub 100M limit se kam.
- Source: `/Library/Developer/KDKs/KDK_14.8.9_23J631.kdk` (isi M1 par, 12713 entries).
  Verify: `cat part-* | zstd -d -c | tar -t | wc -l` = **12713** (source count-exact).
- Extract: `cat KDK_14.8.9_23J631.tar.zst.part-* | zstd -d -c | tar -x -C <dest>`
  (sparse holes zero-fill hokar aayenge - disk par ~4G lagega; original sparse 1.6G chahiye to
  `rsync -aS` se `/Library/Developer/KDKs/` se copy karo).
- `.gitignore`: extracted `KDK_*.kdk/` local-only, sirf compressed parts git me.
