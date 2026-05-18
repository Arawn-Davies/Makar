# demo.sh -- shows what the Makar shell scripting layer can do.
# Run with:  sh /cdrom/apps/demo.sh

echo === Makar shell-script demo ===

# 1. Plain commands -- pwd is a makbox applet
echo
echo -- pwd --
pwd

# 2. Variables + expansion
NAME=tester
GREETING=hello
echo
echo -- variables --
echo $GREETING $NAME
echo from cwd: $(pwd)   # no command substitution yet; literal

# 3. cat through a $VAR-expanded path
PROC=/proc/uname
echo
echo -- cat $PROC --
cat $PROC

# 4. if / then / else / fi
echo
echo -- if branches --
if [ $NAME = tester ]; then
echo if-branch: NAME is tester
else
echo else-branch: NAME is something else
fi

# 5. for ... in
echo
echo -- for loop --
for x in alpha beta gamma; do
echo  loop: $x
done

# 6. while ... do ... done (use a counter via assignment + test)
echo
echo -- while loop --
N=3
while [ $N -gt 0 ]; do
echo  countdown: $N
N=$N
# decrement via simple replacement -- one-shot for 3..1
if [ $N = 3 ]; then N=2; else
  if [ $N = 2 ]; then N=1; else N=0; fi
fi
done

# 7. wall clock
echo
echo -- clock.elf --
/cdrom/apps/clock.elf

echo
echo === demo complete ===
