#!/usr/bin/python
#
# Tests the functionality of gback's glob implement
# Also serves as example of how to write your own
# custom functionality tests.
#
import atexit, proc_check, time
from testutils import *

console = setup_tests()

# ensure that shell prints expected prompt
expect_prompt()

#################################################################
# 
# Boilerplate ends here, now write your specific test.
#
#################################################################
# Step 1. Check that the "history" command prints the current history, starting from 1.

run_builtin('history')
expected_output = "1  history"
expect_exact(expected_output, "History command does not list history")

#################################################################
# Step 2. Check that empty lines are not added to the history, and
# that duplicate inputs are only added once.

sendline("  ")
sendline("echo 123")
sendline("echo 123")
run_builtin('history')
expected_output = "1  history\r\n2  echo 123\r\n3  history"
expect_exact(expected_output, "Duplicate and blank lines are not being filtered from history")

#################################################################
# Step 3. Check that the event designators !n, !-n, !!, !string, !?string,
# and ^string^string modify the entered command and add it to history

sendline("echo 456")
sendline("!!")
sendline("!-3")
sendline("!echo")
sendline("!?456")
sendline("^456^789")
sendline("!1")
expected_output = "1  history\r\n2  echo 123\r\n3  history\r\n4  echo 456\r\n5  echo 123\r\n6  echo 456\r\n7  echo 789\r\n8  history"
expect_exact(expected_output, "Event designators are not correctly modifying the string")

#################################################################
# Step 3. Check that the history substitutions are functional.

sendline("echo 123 456")
sendline("echo !$")
sendline("echo 123 456")
sendline("echo !^")
sendline("echo 123 456")
sendline("echo !*")
run_builtin('history')
expected_output = "1  history\r\n2  echo 123\r\n3  history\r\n4  echo 456\r\n5  echo 123\r\n6  echo 456\r\n7  echo 789\r\n8  history\r\n9  echo 123 456\r\n10  echo 456\r\n11  echo 123 456\r\n12  echo 123\r\n13  echo 123 456\r\n14  history"
expect_exact(expected_output, "History substitution is failing unexpectedly.")

#################################################################
# Step 5. Check that error handling for event designators is functional.

sendline("^abc^123")
expected_output = ":s^abc^123: substitution failed"
expect_exact(expected_output, "Failed to handle the case where event designator expansion fails.")

#################################################################
# Step 6. Check that arrow keys will populate the command line.

up_arrow = "[A"
down_arrow = "[B"
sendline(up_arrow + down_arrow + up_arrow + up_arrow)
expect_exact("123 456", "Arrow keys are not drawing from history")

test_success()