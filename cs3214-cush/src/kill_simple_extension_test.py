#
# kill_test: tests the kill command with the default
# semantics of:
#
# kill <jid>
#
# This test may require updating such that we test other signals
# 
# Requires the following commands to be implemented
# or otherwise usable:
#
#	kill, sleep
#

import sys, atexit, pexpect, proc_check, signal, time, threading
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

# run a command in the background
sendline("sleep 30 &")

# parse the jobid and pid output
(jobid, pid) = parse_bg_status()

# ensure that the shell prints the expected prompt
expect_prompt("Shell did not print expected prompt (2)")

# The job needs to be running when we call kill
proc_check.count_children_timeout(console, 1, 1)

# Test killing the process by JID
sendline("kill " + jobid)

# ensure that the shell prints the expected prompt
expect_prompt("Shell did not print expected prompt (3)")

# ensure there is enough time for the process to be killed
time.sleep(.5)

# check the proc file that the process has actually been stopped
# the proc file should not exist
#
# please note that the OS will remove this entry only after the process
# has been both killed and reaped.  (If it's just killed but not reaped,
# it's a Zombie but retains its /proc entry.)
# Make sure your shell does not with SIGCHLD blocked at the prompt
# and fails to reap the sleep 30 & background job from above after you
# kill it.
#
assert not os.path.exists("/proc/" + pid + "/stat"), 'the process was not killed'

# List of signals to test
signals_to_test = ["-1", "-2", "-3", "-6", "-9", "-15", "-17", "-19", "-23"]

# Loop through the signals and test each one
for signal_name in signals_to_test:
    sendline("kill " + signal_name + " " + jobid)

    # Ensure that the shell prints the expected prompt
    expect_prompt("Shell did not print expected prompt")

    # Add more checks as needed for each signal, e.g., checking if the signal was sent correctly

# Continue with any other tests as needed for your implementation

sendline("exit")

# ensure that no extra characters are output after exiting
expect_exact("exit\r\n", "Shell output extraneous characters")

test_success()
