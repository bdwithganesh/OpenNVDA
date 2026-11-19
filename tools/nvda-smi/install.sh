#!/bin/sh
# Install nvda-smi system-wide (run as root on the Mac). Usage: sudo sh install.sh [path/to/nvda-smi]
set -e
SRC=${1:-$(dirname "$0")/nvda-smi}
/usr/bin/python3 -I -m py_compile "$SRC" 2>/dev/null || /usr/bin/python3 -I -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$SRC"
mkdir -p /usr/local/bin
install -o root -g wheel -m 755 "$SRC" /usr/local/bin/nvda-smi
echo "installed /usr/local/bin/nvda-smi"
