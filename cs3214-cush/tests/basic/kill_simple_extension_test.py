#
# kill_test: tests the kill command with different signals
#
# Requires the following commands to be implemented
# or otherwise usable:
#
#	kill, sleep
#

import sys, atexit, pexpect, proc_check, signal, time, threading
import psutil  # Import the psutil library
from testutils import *

# Define a list of signals to test (by their signal numbers)
#signals_to_test = [1, 2, 3, 6, 9, 15, 17, 19, 23]
signals_to_test = [9]

for signal_to_send in signals_to_test:
    console = setup_tests()

    # ensure that shell prints expected prompt
    expect_prompt()

    # run a command
    sendline("sleep 30 &")

    # parse the jobid and pid output
    (jobid, pid) = parse_bg_status()

    # ensure that the shell prints the expected prompt
    expect_prompt("Shell did not print expected prompt (2)")

    # The job needs to be running when we call kill
    proc_check.count_children_timeout(console, 1, 1)
    
    # Run the kill command with the specified signal number
    sendline(f'kill -{signal_to_send} {jobid}')
    expect_prompt(f"Shell did not print expected prompt ({signal_to_send})")

    # Exit the shell
    sendline("exit")

    # ensure that no extra characters are output after exiting
    expect_exact("exit\r\n", "Shell output extraneous characters")

test_success()
