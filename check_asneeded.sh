#!/bin/bash
# check_asneeded.sh - Test for the -lgcc_s_asneeded linker error

echo "Verifying environment for -lgcc_s_asneeded linker bug..."

# Create a minimal test program
cat <<EOF > link_test.c
int main() { return 0; }
EOF

# Attempt to compile with the problematic flag
# We use -v to see exactly what ld is doing if it fails
COMPILE_OUTPUT=$(gcc link_test.c -lgcc_s_asneeded -o link_test 2>&1)
EXIT_CODE=$?

if [ $EXIT_CODE -ne 0 ] && echo "$COMPILE_OUTPUT" | grep -q "cannot find -lgcc_s_asneeded"; then
    echo "----------------------------------------------------------------"
    echo "CRITICAL ERROR REPRODUCED:"
    echo "The linker cannot find -lgcc_s_asneeded."
    echo "This is consistent with the failure in needed_script.txt."
    echo "----------------------------------------------------------------"
    echo "Diagnostic Details:"
    echo "$COMPILE_OUTPUT"
    echo "----------------------------------------------------------------"

    # Attempt to locate libgcc_s to suggest a fix
    LIBGCC_PATH=$(gcc -print-file-name=libgcc_s.so)
    if [ -f "$LIBGCC_PATH" ]; then
        echo "Found libgcc_s.so at: $LIBGCC_PATH"
        echo "You can likely fix this by creating a compatibility symlink:"
        echo "sudo ln -s $LIBGCC_PATH /usr/lib/libgcc_s_asneeded.so"
    else
        echo "Could not even find libgcc_s.so. Check your gcc installation."
    fi

    rm -f link_test.c link_test
    exit 1
else
    if [ $EXIT_CODE -eq 0 ]; then
        echo "SUCCESS: Linker found -lgcc_s_asneeded (or it was ignored)."
        rm -f link_test.c link_test
        exit 0
    else
        echo "Compilation failed with a different error:"
        echo "$COMPILE_OUTPUT"
        rm -f link_test.c link_test
        exit 2
    fi
fi
