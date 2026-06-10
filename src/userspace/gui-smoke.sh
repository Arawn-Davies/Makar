# gui-smoke.sh -- headless GUI/desktop self-test suite (marker: GUI-SMOKE).
#
# A separate logical section from the shell/VFS smoke matrix (shell-smoke.sh)
# and the kernel ktests: this drives only the window-server's framebuffer-free
# self-tests, so it can run as its own fast CI job (./run.sh guismoke) without
# pulling in the exec-heavy tcc/libc drivers.
#
# Run by the kernel test-mode dispatch when the cmdline is a bare `test_mode`
# (all suites) or names `gui-smoke` explicitly (`test=gui-smoke`).  Each test is
# a gui.elf sub-command that asserts in-process and exits 0 on PASS; it also
# prints its own GUI-UITEST / GUI-FSTEST / GUI-DESKTEST marker.

fail=0

# Immediate-mode widget toolkit (button/slider/textbox) against a synthetic
# off-screen surface -- the same gui_ui the clients use.
echo GUI-SMOKE: uitest
exec /apps/gui.elf uitest
if [ $? -eq 0 ]
then
    echo GUI-SMOKE: [PASS] uitest
else
    echo GUI-SMOKE: [FAIL] uitest
    fail=1
fi

# File-browser navigation against the real VFS (chdir/readdir/getcwd) -- the
# Files window + the editor's open/save dialog move about the filesystem.
echo GUI-SMOKE: fstest
exec /apps/gui.elf fstest
if [ $? -eq 0 ]
then
    echo GUI-SMOKE: [PASS] fstest
else
    echo GUI-SMOKE: [FAIL] fstest
    fail=1
fi

# Desktop logic: the Windows-style icon grid (column-major, height-derived rows,
# always on-screen), on-screen clamping, the case-folded alphabetical sort, and
# the ~/.mxrc icon-position key + value parse + save/load round-trip.
echo GUI-SMOKE: desktest
exec /apps/gui.elf desktest
if [ $? -eq 0 ]
then
    echo GUI-SMOKE: [PASS] desktest
else
    echo GUI-SMOKE: [FAIL] desktest
    fail=1
fi

if [ $fail -eq 0 ]
then
    echo GUI-SMOKE: ALL PASS
else
    echo GUI-SMOKE: FAIL
fi
