# hello.sh - basic scripting smoke test
NAME=world
echo Hello $NAME
if [ $NAME = world ]; then
echo if-branch ran
fi
for x in a b c; do
echo loop $x
done
