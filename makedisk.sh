#!/bin/sh
set -e

# ---------------------------------------------------------------
#  AmiFetch disk builder
#  Usage: ./makedisk.sh [path/to/exe]   (default: out/AmiFetch.exe)
# ---------------------------------------------------------------

if ! command -v xdftool >/dev/null 2>&1; then
    echo "xdftool not found. Install with:  pip3 install amitools"
    exit 1
fi

EXE="${1:-out/AmiFetch.exe}"
if [ ! -f "$EXE" ]; then
    echo "$EXE not found. Build first (F5 / gnumake)."
    exit 1
fi

echo
echo " AmiFetch disk builder"
echo " Input: $EXE"
echo
echo " [1] Shrinkler-crunch the exe, then build the disk  (release)"
echo " [2] Build the disk with the exe as-is              (quick test)"
echo
printf " Pick 1 or 2: "
read -r ANSWER

case "$ANSWER" in
    1)
        # find shrinkler: PATH first, then the amiga-debug extension
        SHRINKLER="$(command -v shrinkler || true)"
        if [ -z "$SHRINKLER" ]; then
            # pick the binary matching this OS (extension ships one per platform)
            case "$(uname -s)" in
                Darwin) PLAT="darwin" ;;
                Linux)  PLAT="linux"  ;;
                *)      PLAT=""       ;;
            esac
            if [ -n "$PLAT" ]; then
                SHRINKLER="$(find "$HOME/.vscode/extensions" -path "*amiga-debug*/bin/$PLAT/*" -iname 'shrinkler*' -type f 2>/dev/null | head -n1)"
            fi
            # fallback: any shrinkler, if the per-platform search missed
            if [ -z "$SHRINKLER" ]; then
                SHRINKLER="$(find "$HOME/.vscode/extensions" -path '*amiga-debug*' -iname 'shrinkler*' -type f 2>/dev/null | head -n1)"
            fi
        fi
        if [ -z "$SHRINKLER" ]; then
            echo "Shrinkler not found on PATH or in the amiga-debug extension."
            echo "Get it from https://github.com/askeksa/Shrinkler/releases"
            exit 1
        fi
        echo "Using Shrinkler: $SHRINKLER"

        # crunch: same flags as the extension's "slow" preset in amiga.json
        #   -h = Amiga hunk executable, -f dff180 = flash background while
        #   decrunching, -9 = max compression
        DISKEXE="out/AmiFetch.shr"
        echo "Crunching (this takes a moment)..."
        "$SHRINKLER" -h -f dff180 -9 "$EXE" "$DISKEXE"
        ;;
    2)
        DISKEXE="$EXE"
        ;;
    *)
        echo "No such option, bailing."
        exit 1
        ;;
esac

ADF="AmiFetch.adf"
rm -f "$ADF"
xdftool "$ADF" format "AmiFetch" ofs
xdftool "$ADF" boot install
xdftool "$ADF" write "$DISKEXE" AmiFetch
xdftool "$ADF" write disk/AmiFetch.info AmiFetch.info
xdftool "$ADF" write disk/Disk.info Disk.info
xdftool "$ADF" makedir s
xdftool "$ADF" write disk/startup-sequence s/startup-sequence

echo
xdftool "$ADF" list
echo
echo "Done: $ADF  (contains $DISKEXE)"
