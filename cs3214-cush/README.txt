Student Information
-------------------
Shayla Carter | shaylac
Kyle Peterson | kyle913

How to execute the shell
------------------------

Compile using "make" with the provided makefile; then use ./cush with no arguments to run the shell itself. Adjust the path to the cush executable based on the directory you're running from as needed.

Important Notes
---------------

No major notes.

Description of Base Functionality
---------------------------------

The builtin commands (jobs, fg, bg, kill, and stop) are checked for before starting other programs/child processes by comparing the value of first argument in the first command against the names of known builtins. If a builtin is found, then the builtin is run and the function returns with an integer signifying that a builtin was executed, which allows for the parent/calling function to free the pipeline which contained the builtin since it will no longer be used (otherwise, the pipeline is set up and executed as normal). The signal commands ^C and ^Z are handled naturally by passing terminal control over to foreground processes (and when the process which recieved the ^C/^Z changes its running status, our signal handler will update its associated job accordingly, updating the stored status/active children/saved terminal state/etc. as necessary).

To provide more detail on the implementation of each non-signal command: the jobs builtin simply traverses our jobs list in a loop and prints each job out. The fg builtin hands terminal ownership over to the desired job, updates its status, and waits for it to finish. The bg builtin continues any paused background jobs. Finally, the stop and kill commands send their respective signals to the given job.


Description of Extended Functionality
-------------------------------------

I/O is implemented using the posix_spawn addopen() file action. We know how many commands each pipeline may run based on the length of the pipeline list, and increment this number after each command we run. We can then use this number to determine if the current command is the first in the pipeline (and thus can check the current ast_pipeline has a file to use as input for that first command), as well as the opposite (if the command is the last one in the pipeline, and an output file exists, use that). We then call addopen() with the file descriptor of stdin (for input files) or stdout (for output files), using the appropriate read/write flags for input/output (as well as an append flag if we need to append to the output file rather than overwrite it).

Pipes are implemented by allocating an an array of arrays (leaving one int array of size two for each pipe needed, that being the number of commands in the pipeline minus 1) and using pipe2 on each int array to set up the pipes. We can use a similar process as when handling our I/O to determine if the current command is not the first command (and thus will have its input piped from somewhere), and if it's not the last command (and thus will have its output, and stderr if the flag in the command is set, piped somewhere). We can then find the appropriate pipe and pipe end and then use adddup2 to wire it up to the STDIN/STDOUT/STDERR file descriptors as appropriate.

Exclusive access is implemented through the use of signal handling. If a process that requires exclusive access is in the background (ex. Vim), it will send a SIGTTOU/SIGTTIN signal. Our signal handler then will call handle_child_status, which will find the job containing the process with the PID that sent the given signal, and will update the job's status to "NEEDSTERMINAL" and allows the process to continue to wait for terminal access. If the user later goes to put that job into the foreground, we send a SIGCONT signal to the job's process group after passing the process group terminal access so that the process can now run. 

List of Additional Builtins Implemented
---------------------------------------

Kill add-on (simple builtin)
    Extends the functionality of kill to support multiple signal types/user-selected signals from the kill command. The basic kill ("kill JID") remains functional; the extension adds an additional format option of "kill -# JID" with support for nine common signal numbers: 1, 2, 3, 6, 9, 15, 17, 19, and 23 (these signals were chosen as the most common signals as indicated by the Man page). Focuses on signals a user would most likely want to send an existing process, such as interrupts and stops.

History (complex builtin)
    Tracks user history of entered commands. User can enter "history" to get a numbered list printout of their history, and can use arrow keys to populate their command line with previous commands from their history. Performs mild filtering to exclude empty lines and back-to-back duplicate commands in the history. Supports event designators including ! history substitutions (ex. !^, !&, and !*), !n, !-n, !!, !string, !?string, and ^string1^string2 as described on the history(3) man page.