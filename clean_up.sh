#!/bin/bash
# clean_up.sh - Cleanup script for busybox_pacman

echo "Cleaning up busybox_pacman temporary files..."

rm -f *.o *.a
rm -f built-in.o
rm -f .*.cmd
rm -f busybox_pacman.patch
rm -f *.db

echo "Cleanup complete."
