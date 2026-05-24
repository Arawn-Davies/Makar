# demo.sh -- exercise every part of the Makar shell scripting layer.
# Run with:  sh /apps/demo.sh   (rootfs election routes to the active volume)
# No user input required -- safe to invoke from ui-test.

echo === Makar shell-script demo ===

sleep 1
sleep 1
# 1. plain commands (makbox applets)
echo
echo -- 1. plain commands --
pwd
echo cwd-ok

sleep 1
# 2. variable assignment + $VAR + ${VAR}
echo
echo -- 2. variables --
NAME=tester
GREETING=hello
echo $GREETING, ${NAME}!
echo current name is $NAME

sleep 1
# 3. cat through an expanded path
echo
echo -- 3. expansion into command args --
PROC=/proc/uname
cat $PROC

sleep 1
# 4. unset
echo
echo -- 4. unset --
THROWAWAY=keep
unset THROWAWAY
echo throwaway-now: $THROWAWAY-end

sleep 1
# 5. env (dump table)
echo
echo -- 5. env dump --
env

sleep 1
# 6. # comments inline -- nothing prints after this
echo done-with-comment   # this trailing fragment is a comment
echo
echo -- 6. comments handled --

sleep 1
# 7. string tests via [ ... ]
echo
echo -- 7. string tests --
if [ $NAME = tester ]; then
echo str-eq-ok
fi
if [ $NAME != stranger ]; then
echo str-neq-ok
fi
if [ -z "" ]; then echo z-ok; fi
if [ -n yes ]; then echo n-ok; fi

sleep 1
# 8. integer tests
echo
echo -- 8. integer tests --
A=7
B=4
if [ $A -gt $B ]; then echo gt-ok; fi
if [ $B -lt $A ]; then echo lt-ok; fi
if [ $A -ne $B ]; then echo ne-ok; fi
if [ $A -eq 7 ]; then echo eq-ok; fi
if [ $A -ge 7 ]; then echo ge-ok; fi
if [ $B -le 4 ]; then echo le-ok; fi

sleep 1
# 9. if / elif / else / fi  (chained elif)
echo
echo -- 9. elif chain --
COLOR=blue
if [ $COLOR = red ]; then
echo elif-wrong-red
elif [ $COLOR = green ]; then
echo elif-wrong-green
elif [ $COLOR = blue ]; then
echo elif-correct-blue
else
echo elif-fell-through
fi

sleep 1
# 10. for ... in WORDS
echo
echo -- 10. for loop --
for x in alpha beta gamma; do
echo for: $x
done

sleep 1
# 11. for with variable substitution
echo
echo -- 11. for over variable --
WORDS=one two three
for w in $WORDS; do
echo word: $w
done

sleep 1
# 12. while with countdown
echo
echo -- 12. while loop --
N=3
while [ $N -gt 0 ]; do
echo countdown: $N
if [ $N = 3 ]; then N=2; elif [ $N = 2 ]; then N=1; else N=0; fi
done

sleep 1
# 13. exit code observable via $?
echo
echo -- 13. exit code in dollar-question --
true
echo after-true: $?
# An unknown command sets $? to 127.
nonsense-command-that-does-not-exist
echo after-bad: $?

sleep 1
# 14. cmdline wall clock (one-liner, scriptable)
echo
echo -- 14. datetime --
datetime

echo
echo === demo complete ===
