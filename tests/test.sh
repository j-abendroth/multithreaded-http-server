#!/bin/bash
# Certain techniques in these script inspired by Allen, Clark, & Daniel
# Tests written for loadbalancer

NORM='\033[0m'
RED='\033[0;1;31m'
GREEN='\033[0;1;32m'
BLUE='\033[0;1;34m'
PURP='\033[0;1;35m'
CYAN='\033[0;1;36m'
ASGN=3
NAME=loadbalancer
DIR=~/Desktop/tests
TEST=~/Desktop/tests

kill_p() {
   exec 3>&2
   exec 2>/dev/null
   pkill loadbalancer
   pkill httpserver
   pkill httpserver
   pkill httpserver
   exec 2>&3
   exec 3>&-
}

ctrl_c() {
   kill_p
   exit 1
}

check_test() {
   if [[ $? -eq 0 ]]; then
      printf "${GREEN}PASS$NORM\n"
   else
      printf "${RED}FAIL$NORM\n"
   fi
}

run_test() {
   chmod +x $1
   $1
   check_test
}

trap ctrl_c INT

cd $DIR
printf "\n${BLUE}===== Build $NAME =====$NORM\n"
make spotless
make
make clean
chmod +x loadbalancer
chmod +x httpserver

printf "\n${PURP}========> Running tests for asgn$ASGN <================$NORM\n"

./loadbalancer 1234 8080 8081 8082 &>/dev/null & ./httpserver 8080 -L &>/dev/null & ./httpserver 8081 -L &>/dev/null & ./httpserver 8082 -L &>/dev/null &
sleep 1
for test in $TEST/*.test; do
   name=${test%.test}
   printf "${CYAN}-> Running test: ${name##*/} ...$NORM\n"
   run_test $test
done
kill_p

: <<'END'
./httpserver 8080 -l log8080 &>/dev/null & ./httpserver 8081 -l log8081 &>/dev/null & ./httpserver 8082 -l log8082 &>/dev/null & wait & ./loadbalancer 1234 8080 8081 8082 &>/dev/null &
sleep 1
for test in $TEST/*.ltest; do
   name=${test%.ltest}
   printf "${CYAN}-> Running test: ${name##*/} ...$NORM\n"
   run_test $test
done
kill_p
END

printf "${PURP}========> End test script for asgn$ASGN <=========$NORM\n\n"
rm -rf $DIR/*.out $TEST/*.out
ls | grep -P '^(?!Makefile)(?!loadbalancer)(?!httpserver)([a-zA-Z0-9_-]+){1,27}$' | xargs rm &>/dev/null
