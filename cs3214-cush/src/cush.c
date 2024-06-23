/*
 * cush - the customizable shell.
 *
 * Developed by Godmar Back for CS 3214 Summer 2020 
 * Virginia Tech.  Augmented to use posix_spawn in Fall 2021.
 */
#define _GNU_SOURCE    1
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>

/* Since the handed out code contains a number of unused functions. */
#pragma GCC diagnostic ignored "-Wunused-function"

#include "termstate_management.h"
#include "signal_support.h"
#include "shell-ast.h"
#include "utils.h"
#include "spawn.h"

extern char **environ;

static int z_update_jid = -1;
static int error_update_code = -1;
static void handle_child_status(pid_t pid, int status);
static void iterate_over_command_line(struct ast_command_line *cmdline);
static int iterate_over_pipeline(struct ast_pipeline *pipe, struct ast_command_line *cmdline);
static void print_all_jobs(void);
static void free_all_jobs(void);
static struct job *job_with_jid(int jid);
static void print_error_message(int termination_code);
static int check_for_builtin(struct ast_command *cmd);

static void
usage(char *progname)
{
    printf("Usage: %s -h\n"
        " -h            print this help\n",
        progname);

    exit(EXIT_SUCCESS);
}

/* Build a prompt */
static char *
build_prompt(void)
{
    return strdup("cush> ");
}

enum job_status {
    FOREGROUND,     /* job is running in foreground.  Only one job can be
                       in the foreground state. */
    BACKGROUND,     /* job is running in background */
    STOPPED,        /* job is stopped via SIGSTOP */
    NEEDSTERMINAL,  /* job is stopped because it was a background job
                       and requires exclusive terminal access */
    TERMINATED,     /* job was forcibly terminated */
    DONE,           /* job finished execution and exited */
};

struct job {
    struct list_elem elem;   /* Link element for jobs list. */
    struct ast_pipeline *pipe;  /* The pipeline of commands this job represents */
    int     jid;             /* Job id. */
    enum job_status status;  /* Job status. */ 
    int  num_processes_alive;   /* The number of processes that we know to be alive */
    struct termios saved_tty_state;  /* The state of the terminal when this job was 
                                        stopped after having been in foreground */

    /* Add additional fields here if needed. */
    int termination_code; /* The identifier number for the error/command that terminated the job. */
    pid_t* child_pid_array; /* The pid's of the child processes occurring within a job */
    bool state_saved_previously;
};

/* Utility functions for job list management.
 * We use 2 data structures: 
 * (a) an array jid2job to quickly find a job based on its id
 * (b) a linked list to support iteration
 */
#define MAXJOBS (1<<16)
static struct list job_list;

static struct job * jid2job[MAXJOBS];

/* Return job corresponding to jid */
static struct job * 
get_job_from_jid(int jid)
{
    if (jid > 0 && jid < MAXJOBS && jid2job[jid] != NULL)
        return jid2job[jid];

    return NULL;
}

/* Add a new job to the job list */
static struct job *
add_job(struct ast_pipeline *pipe)
{
    struct job * job = malloc(sizeof *job);
    job->pipe = pipe;
    job->num_processes_alive = 0;
    list_push_back(&job_list, &job->elem);
    for (int i = 1; i < MAXJOBS; i++) {
        if (jid2job[i] == NULL) {
            jid2job[i] = job;
            job->jid = i;
            return job;
        }
    }
    fprintf(stderr, "Maximum number of jobs exceeded\n");
    abort();
    return NULL;
}

/* Delete a job.
 * This should be called only when all processes that were
 * forked for this job are known to have terminated.
 */
static void
delete_job(struct job *job)
{
    int jid = job->jid;
    assert(jid != -1);
    jid2job[jid]->jid = -1;
    jid2job[jid] = NULL;
    free(job->child_pid_array);
    ast_pipeline_free(job->pipe);
    free(job);
}

/*
 * Returns a string with the print-version of the current job status.
 */
static const char *
get_status(enum job_status status)
{
    switch (status) {
    case FOREGROUND:
        return "Foreground";
    case BACKGROUND:
        return "Running";
    case STOPPED:
        return "Stopped";
    case NEEDSTERMINAL:
        return "Stopped (tty)";
    default:
        return "Unknown";
    }
}

/* Print the command line that belongs to one job. */
static void
print_cmdline(struct ast_pipeline *pipeline)
{
    struct list_elem * e = list_begin (&pipeline->commands); 
    for (; e != list_end (&pipeline->commands); e = list_next(e)) {
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);
        if (e != list_begin(&pipeline->commands))
            printf("| ");
        char **p = cmd->argv;
        printf("%s", *p++);
        while (*p)
            printf(" %s", *p++);
    }
}

/*
 * Prints a specialized error message based on the error code of the process.
 */
static void
print_error_message(int termination_code) {
    switch (termination_code) {
    case SIGFPE:
        printf("Floating point exception\n");
        return;
    case SIGSEGV:
        printf("Segmentation fault\n");
        return;
    case SIGABRT:
        printf("Aborted\n");
        return;
    case SIGKILL:
        printf("Killed\n");
        return;
    default:
        printf("Terminated\n");
        return;
    }
}

/* Print a job */
static void
print_job(struct job *job)
{

    if(job->status ==  TERMINATED){ //check to see if job is terminated
        print_error_message(job->termination_code);
    }
    else if(job->status == DONE){ //check to see if job is done
        printf("[%d]\tDone\n", job->jid);
    }
    else{
        printf("[%d]\t%s\t\t(", job->jid, get_status(job->status));
        print_cmdline(job->pipe);
        printf(")\n");
    }
}

/*
 * Prints out every job in the list of jobs.
 */
void 
print_all_jobs()
{

    struct list_elem *e = list_begin(&job_list);
    while (e != list_end(&job_list)) {
        struct job *current_job = list_entry(e, struct job, elem); // Gets the current job

        // If the job status is terminated or done, then we'll need to delete
        // this job after we print it out. Otherwise, we can just iterate
        // normally.
        print_job(current_job);
        if (current_job->status == TERMINATED || current_job->status == DONE) {
            e = list_remove(e);
            delete_job(current_job);
        } else {
            e = list_next(e);
        }
    }
}

/*
 * Frees everything remaining in the jobs list.
 */
void 
free_all_jobs()
{
    for (struct list_elem * e = list_begin(&job_list); e != list_end(&job_list); ) {
        struct job *current_job = list_entry(e, struct job, elem);
        e = list_remove(e);

        delete_job(current_job);
    }
}

/*
 * Returns the job with the given jid.
 */
static struct job *
job_with_jid(int jid)
{
    struct job * found_job = NULL;
    for (struct list_elem * e = list_begin (&job_list); 
        e != list_end (&job_list); 
        e = list_next (e)) {
        struct job *current_job = list_entry(e, struct job, elem);
        if(current_job->jid == jid) {
            found_job = current_job;
            break;
        }
    }
    return found_job;
}


/*
 * Suggested SIGCHLD handler.
 *
 * Call waitpid() to learn about any child processes that
 * have exited or changed status (been stopped, needed the
 * terminal, etc.)
 * Just record the information by updating the job list
 * data structures.  Since the call may be spurious (e.g.
 * an already pending SIGCHLD is delivered even though
 * a foreground process was already reaped), ignore when
 * waitpid returns -1.
 * Use a loop with WNOHANG since only a single SIGCHLD 
 * signal may be delivered for multiple children that have 
 * exited. All of them need to be reaped.
 */
static void
sigchld_handler(int sig, siginfo_t *info, void *_ctxt)
{
    pid_t child;
    int status;

    assert(sig == SIGCHLD);

    while ((child = waitpid(-1, &status, WUNTRACED|WNOHANG)) > 0) {
        handle_child_status(child, status);
    }
}

/* Wait for all processes in this job to complete, or for
 * the job no longer to be in the foreground.
 * You should call this function from a) where you wait for
 * jobs started without the &; and b) where you implement the
 * 'fg' command.
 * 
 * Implement handle_child_status such that it records the 
 * information obtained from waitpid() for pid 'child.'
 *
 * If a process exited, it must find the job to which it
 * belongs and decrement num_processes_alive.
 *
 * However, note that it is not safe to call delete_job
 * in handle_child_status because wait_for_job assumes that
 * even jobs with no more num_processes_alive haven't been
 * deallocated.  You should postpone deleting completed
 * jobs from the job list until when your code will no
 * longer touch them.
 *
 * The code below relies on `job->status` having been set to FOREGROUND
 * and `job->num_processes_alive` having been set to the number of
 * processes successfully forked for this job.
 */
static void
wait_for_job(struct job *job)
{
    assert(signal_is_blocked(SIGCHLD));

    while (job->status == FOREGROUND && job->num_processes_alive > 0) {
        int status;

        pid_t child = waitpid(-1, &status, WUNTRACED);

        // When called here, any error returned by waitpid indicates a logic
        // bug in the shell.
        // In particular, ECHILD "No child process" means that there has
        // already been a successful waitpid() call that reaped the child, so
        // there's likely a bug in handle_child_status where it failed to update
        // the "job" status and/or num_processes_alive fields in the required
        // fashion.
        // Since SIGCHLD is blocked, there cannot be races where a child's exit
        // was handled via the SIGCHLD signal handler.
        if (child != -1)
            handle_child_status(child, status);
        else
            utils_fatal_error("waitpid failed, see code for explanation");
    }

    // If we reach this code, then either all of the processes in the job
    // are dead or the job was moved into the background for whatever reason.

    // We need to take a sample of the terminal state when the job finishes,
    // but only if the job was not killed via a signal such as using
    // the kill PID command from another terminal.
    // So, we don't want to sample if the job has a termination_code
    // other than the error ones? Or maybe just a termination code in general.
    if (job->termination_code == -1) {
        termstate_sample();
    }

    // We only want to delete the job if all of the processes are dead.
    // Recall that this function only handles processes that are running
    // in the foreground, so we don't need to preserve the job to notify
    // the user later on that the process finished.
    // This is to account for the case where we have 1 child still alive,
    // but we're using the bg command to move the entire job
    // into the background.
    if (job->num_processes_alive < 1) {
        list_remove(&job->elem);
        delete_job(job);
    }

}

static void
handle_child_status(pid_t pid, int status)
{
    assert(signal_is_blocked(SIGCHLD));

    // Starts by getting the job that contains the process that
    // just changed its status. Searches by PID.
    struct job * found_job = NULL;
    for (struct list_elem * e = list_begin (&job_list); 
        e != list_end (&job_list); 
        e = list_next (e)) {
    
        struct job *current_job = list_entry(e, struct job, elem);
        int num_pids_to_search = list_size(&current_job->pipe->commands);
        for (int i = 0; i < num_pids_to_search; i++) {
            if (current_job->child_pid_array[i] == pid) {
                found_job = current_job;
            }
        }
        if (found_job != NULL) {
            break;
        }

    }

    // Now updates the status, termination code, process alive, and so on
    // based on what particular signal the child is reporting.
    if (WIFEXITED(status)) {
        // The process finished normally.
        found_job->num_processes_alive--;
        if (found_job->num_processes_alive < 1) {
            found_job->status = DONE;
        }
    } else if (WIFSIGNALED(status)) {
        int termination_code = WTERMSIG(status);
        if (termination_code) {
            found_job->termination_code = termination_code;
            error_update_code = found_job->termination_code;
        }

        // The process was terminated.
        found_job->num_processes_alive--;
        if (found_job->num_processes_alive < 1) {
            found_job->status = TERMINATED;
        }
    } else if (WIFSTOPPED(status)) {
        // The process was stopped.
        if (WSTOPSIG(status) == SIGTTOU || WSTOPSIG(status) == SIGTTIN) {
            // A background process wants terminal access.
            found_job->status = NEEDSTERMINAL;
        } else {

            // We only need to print a status update and
            // save the terminal state if the job was originally
            // running in the foreground.
            if (found_job->status == FOREGROUND) {
                z_update_jid = found_job->jid;
                termstate_save(&found_job->saved_tty_state);
                found_job->state_saved_previously = true;
            }

            // However, even if the job was running in the background
            // originally, it needs to have the status of "STOPPED."
            found_job->status = STOPPED;
        }
    }
}

/*
 * Calls iterate_over_pipeline for each complete command group.
 * Ex. echo 74 > midterm.txt; cat midterm.txt | rev > other_midterm.txt
 * The above splits into two complete/distinct groups: echo 74 > midterm.txt, and cat midterm.txt | rev > other_midterm.txt
 */
void 
iterate_over_command_line(struct ast_command_line *cmdline)
{
    for (struct list_elem * e = list_begin(&cmdline->pipes); e != list_end(&cmdline->pipes); ) {
        
        // Gets the current pipeline and then removes the node from the command_line list
        // since we won't need to access the *full command line* after this point.
        struct ast_pipeline *pipe = list_entry(e, struct ast_pipeline, elem);
        e = list_remove(e);

        // Now iterates over each command in that pipeline
        int delete_pipeline = iterate_over_pipeline(pipe, cmdline);
        
        //If delete_pipeline is 1, then we were running a builtin, so we
        // won't need the pipe later and can go ahead and free it now.
        if (delete_pipeline == 1) {
            ast_pipeline_free(pipe);
        }
    }
}


// Returns 1 if we need to free the pipeline after it returns.
// (We only need to free pipelines when we've completed running a builtin command.)
int
iterate_over_pipeline(struct ast_pipeline *pipe, struct ast_command_line *cmdline)
{
    
    // Starts by blocking SIGCHLD until we're done adding children and managing the job.
    // We do this to ensure that we don't handle the signals from finishing processes until
    // after we've finished setting up the job. We don't need to check the return of signal_block
    // because we don't care if it's already been blocked, so long as it is currently blocked.
    signal_block(SIGCHLD);

    // Before we start dealing with piping and jobs, we want to confirm that we're not
    // just running a builtin. Exit is a special case even among the builtins, as we
    // need to clean our memory up before we exit directly from here. So we handle it separately.
    struct ast_command *builtin_cmd = list_entry(list_begin(&pipe->commands), struct ast_command, elem);
    if (strcmp(builtin_cmd->argv[0],"exit") == 0) {            
        free_all_jobs();
        ast_pipeline_free(pipe);
        free(cmdline);
        exit(0);
    }
    // Now checks for the other builtin commands.
    if (check_for_builtin(builtin_cmd) == 1) {
        signal_unblock(SIGCHLD);
        return 1;
    }

    // Sets up some variables to use while setting up our child processes.
    int child_count = 0;
    pid_t pgid = 0;
    int pipeline_length = list_size(&pipe->commands);
    pid_t* child_pid_array = (pid_t*)malloc(pipeline_length * sizeof(pid_t));

    // The length of the pipeline = #pipes needed - 1. So if we have 2 commands
    // in the pipeline, we need one pipe. If we have only 1 command in the pipeline,
    // then we need zero pipes. We must use malloc here because it's possible
    // to dynamically allocate an array of length 0, but we cannot statically
    // allocate an array in such a case.
    const int READ_END = 0;
    const int WRITE_END = 1;
    int (*pipes)[2] = malloc(sizeof(int[pipeline_length - 1][2]));
    for (int i = 0; i < (pipeline_length - 1); i++) {
        pipe2(pipes[i], O_CLOEXEC);
    }

    // Each iteration of the for loop is for a different command.
    for (struct list_elem * e = list_begin(&pipe->commands); 
         e != list_end(&pipe->commands); 
         e = list_next(e)) {

        // Gets the current command.
        struct ast_command *cmd = list_entry(e, struct ast_command, elem);

        // Set up variables and flags for use with posix_spawnp
        int file_actions_status;
        int spawn_attr_status;
        sigset_t child_sigmask;
        sigemptyset(&child_sigmask);
        posix_spawn_file_actions_t child_file_attr;
        posix_spawnattr_t child_spawn_attr;
        if ((spawn_attr_status = posix_spawnattr_init(&child_spawn_attr))) {
            perror("posix_spawnattr_init");
            return 1;
        }
        if ((file_actions_status = posix_spawn_file_actions_init(&child_file_attr))) {
            perror("posix_spawn_file_actions_init");
            return 1;
        }
        short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK;
        if (!(pipe->bg_job)) {
            flags = flags | POSIX_SPAWN_TCSETPGROUP;
            spawn_attr_status = spawn_attr_status | posix_spawnattr_tcsetpgrp_np(&child_spawn_attr, termstate_get_tty_fd());
        }

        spawn_attr_status = spawn_attr_status | posix_spawnattr_setflags(&child_spawn_attr, flags);
        spawn_attr_status = spawn_attr_status | posix_spawnattr_setpgroup(&child_spawn_attr, pgid);
        spawn_attr_status = spawn_attr_status | posix_spawnattr_setsigmask(&child_spawn_attr, &child_sigmask);
        if (spawn_attr_status) {
            perror("spawn_attr_status");
            return 1;
        }

        // If we're the first command in the pipeline and we have an input file
        if (child_count == 0 && pipe->iored_input != NULL) {
            file_actions_status = file_actions_status | posix_spawn_file_actions_addopen(&child_file_attr, STDIN_FILENO, pipe->iored_input, O_RDONLY, 0);
        }
        // If we're the last command in the pipeline and we have an output file
        if (child_count == pipeline_length - 1 && pipe->iored_output != NULL) {
            int oflags = (O_CREAT | O_WRONLY) | ((pipe->append_to_output) ? O_APPEND : O_TRUNC);
            file_actions_status = posix_spawn_file_actions_addopen(&child_file_attr, STDOUT_FILENO, pipe->iored_output, oflags, S_IROTH | S_IWOTH | S_IRGRP | S_IWGRP | S_IRUSR | S_IWUSR);
            if (!file_actions_status && cmd->dup_stderr_to_stdout) {
                file_actions_status = posix_spawn_file_actions_adddup2(&child_file_attr, STDOUT_FILENO, STDERR_FILENO); 
            }
        }

        if (file_actions_status) {
            perror("posix_spawn_file_actions_addopen");
            return 1;
        }

        // If we're setting up piping and we're not the first command in the pipeline
        // (i.e. we're getting our input from a pipe)
        if (pipeline_length > 1 && child_count > 0) {
            file_actions_status = posix_spawn_file_actions_adddup2(&child_file_attr, pipes[child_count - 1][READ_END], STDIN_FILENO);        
        }
        // If we're setting up piping and we're not the last command in the pipeline
        // (i.e. we're sending our output to a pipe)
        if (pipeline_length > 1 && child_count < pipeline_length - 1) {
            file_actions_status = posix_spawn_file_actions_adddup2(&child_file_attr, pipes[child_count][WRITE_END], STDOUT_FILENO);
            if (!file_actions_status && cmd->dup_stderr_to_stdout) {
                file_actions_status = posix_spawn_file_actions_adddup2(&child_file_attr, STDOUT_FILENO, STDERR_FILENO);
            }
        }

        if (file_actions_status) {
            perror("posix_spawn_file_actions_adddup2");
            return 1;
        }

        pid_t new_child_pid;
        int posix_spawn_status = posix_spawnp(&new_child_pid, cmd->argv[0], &child_file_attr, &child_spawn_attr, cmd->argv, environ);
        if (posix_spawn_status != 0) {
            // We return 1 if the posix_spawnp failed as we won't be
            // adding this pipeline to a running job.
            termstate_sample();
            termstate_give_terminal_back_to_shell();
            perror("posix_spawnp");
            signal_unblock(SIGCHLD);
            for (int i = 0; i < (pipeline_length - 1); i++) {
                int pipe1_result = close(pipes[i][READ_END]);
                int pipe2_result = close(pipes[i][WRITE_END]);
                if (pipe1_result == -1 || pipe2_result == -1) {
                    perror("Error closing pipes");
                }
            }
            if (posix_spawn_file_actions_destroy(&child_file_attr)) {
                perror("posix_spawn_file_actions_destroy");
            }
            free(pipes);
            free(child_pid_array);
            return 1;
        }

        if (posix_spawn_file_actions_destroy(&child_file_attr)) {
            perror("posix_spawn_file_actions_destroy");
        }

        // Adds the newly-spawned child's PID into the PID array the job will keep track of.
        child_pid_array[child_count] = new_child_pid;
        if (pgid == 0) {
            pgid = new_child_pid;
        }

        child_count++; // Now increments the number of children we've successfully spawned.

    }

    // Now close all of the pipes we created.
    for (int i = 0; i < (pipeline_length - 1); i++) {
        int pipe1_result = close(pipes[i][READ_END]);
        int pipe2_result = close(pipes[i][WRITE_END]);
        if (pipe1_result == -1 || pipe2_result == -1) {
            perror("Error");
        }
    }
    free(pipes);

    // Adds the current pipeline to the jobs list as one new job
    // Then updates the status of that job based on what the pipeline intends that job to be.
    struct job *current_job = add_job(pipe);
    current_job->num_processes_alive = child_count;
    current_job->child_pid_array = child_pid_array;
    current_job->termination_code = -1;
    current_job->state_saved_previously = false;
    if (pipe->bg_job) {
        current_job->status = BACKGROUND;
        printf("[%d] %d\n", current_job->jid, current_job->child_pid_array[0]);
    } else {
        current_job->status = FOREGROUND;
        wait_for_job(current_job);

        // Once the current foreground process is done,
        // we can pass terminal ownership back to the shell.
        termstate_give_terminal_back_to_shell();
    }

    // Once all of that is done, we can unblock SIGCHLD again.
    signal_unblock(SIGCHLD);

    return 0;
}

static int
check_for_builtin(struct ast_command *cmd) {

    if (strcmp(cmd->argv[0],"jobs") == 0) {
        print_all_jobs();
        return 1;
    }
        if (strcmp(cmd->argv[0],"history") == 0) {
        HISTORY_STATE *history_state = history_get_history_state();
        for (int i = 0; i < history_state->length; i++) {
            printf("%d  %s\n", (i + 1), history_state->entries[i]->line);
        }
        free(history_state);
        return 1;
    }
    if (strcmp(cmd->argv[0],"fg") == 0) {            
        // Start by getting the job with the given jid
        struct job * found_job = job_with_jid(atoi(cmd->argv[1]));
        if (found_job == NULL) {
            printf("fg %s: No such job\n", cmd->argv[1]);
            return 1;
        }

        // Now print the command
        print_cmdline(found_job->pipe);
        printf("\n");

        // Then give the terminal to that job using the pgid.
        pid_t pgid_to_target = found_job->child_pid_array[0];
        termstate_give_terminal_to((found_job->state_saved_previously) ? &found_job->saved_tty_state : NULL, pgid_to_target);


        if (found_job->status == STOPPED || found_job->status == NEEDSTERMINAL) {
            int kill_status = killpg(pgid_to_target, SIGCONT);
            if (kill_status == -1) {
                // Then there must have been an error sending the signal.
                // In this case, we don't want to wait for this job, since it
                // won't know that it's supposed to be continuing.
                termstate_give_terminal_back_to_shell();
                return 1;
            }
        }
        found_job->status = FOREGROUND;
        wait_for_job(found_job);
        termstate_give_terminal_back_to_shell();

        return 1;
    }
    if (strcmp(cmd->argv[0],"bg") == 0) {
        struct job * found_job = job_with_jid(atoi(cmd->argv[1]));
        pid_t pgid_to_target = found_job->child_pid_array[0];
        if (found_job == NULL) {
            printf("bg %s: No such job\n", cmd->argv[1]);
            return 1;
        }

        if (found_job->status == BACKGROUND) {
            printf("bg: %s already in background\n", cmd->argv[1]);
        } else {
            found_job->status = BACKGROUND;
            int kill_status = killpg(pgid_to_target, SIGCONT);
            if (kill_status == -1) {
                perror("bg:");
            }
        }

        return 1;
    }
    if (strcmp(cmd->argv[0],"stop") == 0) {
        struct job * found_job = job_with_jid(atoi(cmd->argv[1]));
        if (found_job == NULL) {
            printf("stop %s: No such job\n", cmd->argv[1]);
            return 1;
        }
        pid_t pgid_to_target = found_job->child_pid_array[0];
        int kill_status = killpg(pgid_to_target, SIGSTOP);
        if (kill_status == -1) {
            perror("stop:");
        }

        return 1;
    }
    if (strcmp(cmd->argv[0],"kill") == 0) {

        int length_of_argv = 0;

        // Calculate the length of cmd->argv
        while (cmd->argv[length_of_argv] != NULL) {
            length_of_argv++;
        }

        if (length_of_argv < 2) {
            printf("kill: usage: kill [jid]\n");
            return 1;
        }
        else if (length_of_argv < 3) {
            struct job * found_job = job_with_jid(atoi(cmd->argv[1]));
            pid_t pgid_to_target = found_job->child_pid_array[0];
            int kill_status = killpg(pgid_to_target, SIGKILL);
            if(kill_status == 0){
               printf("Sent kill signal\n");
            } else {
                perror("kill");
            }
            return 1;
        } else {
            int signal_to_send = atoi(cmd->argv[1] + 1); //this is to remove the dash from the signal
            struct job * found_job = job_with_jid(atoi(cmd->argv[2]));
            pid_t pgid_to_target = found_job->child_pid_array[0];
            int kill_status = -1;
            //check to make sure we are using the common kill signals otherwise do not kill
            if(signal_to_send == 1 || signal_to_send == 2 || signal_to_send == 3 || signal_to_send == 6 || signal_to_send == 9 || signal_to_send == 15 || signal_to_send == 17 || signal_to_send == 19 || signal_to_send == 23){
                if(signal_to_send == 1){
                    kill_status = killpg(pgid_to_target, SIGHUP);
               } else if(signal_to_send == 2){
                    kill_status = killpg(pgid_to_target, SIGINT);
               } else if(signal_to_send == 3){
                    kill_status = killpg(pgid_to_target, SIGQUIT);
               } else if(signal_to_send == 6){
                    kill_status = killpg(pgid_to_target, SIGABRT);
               } else if(signal_to_send == 9){
                    kill_status = killpg(pgid_to_target, SIGKILL);
               } else if(signal_to_send == 15){
                    kill_status = killpg(pgid_to_target, SIGTERM);
               } else if(signal_to_send == 17){
                   kill_status = killpg(pgid_to_target, SIGSTOP);
               } else if(signal_to_send == 19){
                    kill_status = killpg(pgid_to_target, SIGSTOP);
               } else if(signal_to_send == 23){
                    kill_status = killpg(pgid_to_target, SIGSTOP);
               }
            } else {
                printf("kill: usage: kill [-SIGNAL] [jid]\n");
                kill_status = 1;
            }
            //print out the signal that was sent
            if(kill_status == 0){
                if(signal_to_send == 1){
                    printf("Sent kill signal SIGHUP\n");
               } else if(signal_to_send == 2){
                    printf("Sent kill signal SIGINT\n");
               } else if(signal_to_send == 3){
                    printf("Sent kill signal SIGQUIT\n");
               } else if(signal_to_send == 6){
                    printf("Sent kill signal SIGABRT\n");
               } else if(signal_to_send == 9){
                    printf("Sent kill signal SIGKILL\n");
               } else if(signal_to_send == 15){
                    printf("Sent kill signal SIGTERM\n");
               } else if(signal_to_send == 17){
                    printf("Sent kill signal SIGSTOP\n");
               } else if(signal_to_send == 19){
                    printf("Sent kill signal SIGSTOP\n");
               } else if(signal_to_send == 23){
                    printf("Sent kill signal SIGSTOP\n");
               }
            } else {
                perror("kill");
            }
            return 1;
        }
    }

    
    return 0;
}

int
main(int ac, char *av[])
{
    int opt;

    /* Process command-line arguments. See getopt(3) */
    while ((opt = getopt(ac, av, "h")) > 0) {
        switch (opt) {
        case 'h':
            usage(av[0]);
            break;
        }
    }

    list_init(&job_list);
    signal_set_handler(SIGCHLD, sigchld_handler);
    termstate_init(); // This handles saving the terminal termstate and the terminal's pgid
    using_history(); // This handles initializing values for the command history

    /* Read/eval loop. */
    for (;;) {

        /* If you fail this assertion, you were about to enter readline()
         * while SIGCHLD is blocked.  This means that your shell would be
         * unable to receive SIGCHLD signals, and thus would be unable to
         * wait for background jobs that may finish while the
         * shell is sitting at the prompt waiting for user input.
         */
        assert(!signal_is_blocked(SIGCHLD));

        /* Before we print the prompt, we need to check if there was
         * a process stopped by ^Z that we need to report.
         */
        if (z_update_jid != -1) {
            struct job * found_job = job_with_jid(z_update_jid);
            printf("[%d]+\t%s\t\t(", found_job->jid, get_status(found_job->status));
            print_cmdline(found_job->pipe);
            printf(")\n");
            z_update_jid = -1;
        }

        if (error_update_code != -1) {
            print_error_message(error_update_code);
            error_update_code = -1;
        }


        /* If you fail this assertion, you were about to call readline()
         * without having terminal ownership.
         * This would lead to the suspension of your shell with SIGTTOU.
         * Make sure that you call termstate_give_terminal_back_to_shell()
         * before returning here on all paths.
         */
        assert(termstate_get_current_terminal_owner() == getpgrp());

        /* Do not output a prompt unless shell's stdin is a terminal */
        char * prompt = isatty(0) ? build_prompt() : NULL;
        char * cmdline = readline(prompt);
        free (prompt);

        if (cmdline == NULL)  /* User typed EOF */
            break;

        char *expanded_cmdline;
        int expansion_status = 0;
        expansion_status = history_expand(cmdline, &expanded_cmdline);
        free (cmdline);
        if (expansion_status == 1) {
            printf("%s\n", expanded_cmdline);
        } else if (expansion_status == -1) {
            // This happens if there's an error in expansion.
            // Ex. trying to replace a string that doesn't exist.
            printf("%s\n", expanded_cmdline);
            free (expanded_cmdline);
            continue;
        }

        int where_in_history = where_history();
        HIST_ENTRY *current_history_entry = history_get(where_in_history);
        struct ast_command_line * cline = ast_parse_command_line(expanded_cmdline);
        if (cline == NULL) {                  /* Error in command line */
            free (expanded_cmdline);
            continue;
        }
        if (list_empty(&cline->pipes)) {    /* User hit enter */
            free (expanded_cmdline);
            ast_command_line_free(cline);
            continue;
        }
        if (where_in_history == 0 || (current_history_entry != NULL && strcmp(current_history_entry->line, expanded_cmdline) != 0)) {
            add_history(expanded_cmdline);
        }
        free (expanded_cmdline);


        iterate_over_command_line(cline);

        // The below will free the command line itself
        // Without freeing any of the pipes, since they may
        // be needed elsewhere and should be freed at another time.
        free(cline);
    }
    return 0;
}